#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <pthread.h>

#include "monitor_ioctl.h"

#define MAX_CONTAINERS 16
#define MAX_NAME 64
#define MAX_CMD_ARGS 32
#define SOCK_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define MONITOR_DEV "/dev/container_monitor"

#define STACK_SIZE (1024 * 1024)
static char clone_stack[STACK_SIZE];

typedef enum {
    STATE_EMPTY = 0,
    STATE_STARTING,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED
} ContainerState;

typedef struct {
    char name[MAX_NAME];
    pid_t host_pid;
    time_t start_time;
    ContainerState state;
    int stop_requested;

    int pipe_rd;
    char log_path[256];

    unsigned long soft_limit;
    unsigned long hard_limit;

} Container;

static Container containers[MAX_CONTAINERS];
static int supervisor_running = 1;
static int monitor_fd = -1;

/* ───────────── container entry ───────────── */

typedef struct {
    char rootfs[256];
    char cmd[MAX_CMD_ARGS][256];
    int argc;
    char name[MAX_NAME];
    int pipe_wr;
    int interactive;
} Args;

static int container_main(void *arg)
{
    Args *a = (Args *)arg;

    sethostname(a->name, strlen(a->name));

    char proc_path[512];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", a->rootfs);
    mkdir(proc_path, 0555);
    mount("proc", proc_path, "proc", 0, NULL);

    if (chroot(a->rootfs) < 0) {
        perror("chroot");
        return 1;
    }

    chdir("/");

    if (!a->interactive) {
        dup2(a->pipe_wr, STDOUT_FILENO);
        dup2(a->pipe_wr, STDERR_FILENO);
        close(a->pipe_wr);
    }

    char *argv[MAX_CMD_ARGS + 1];
    for (int i = 0; i < a->argc; i++)
        argv[i] = a->cmd[i];
    argv[a->argc] = NULL;

    if (a->interactive && strcmp(argv[0], "/bin/sh") == 0)
        execl("/bin/sh", "/bin/sh", "-i", NULL);
    else
        execv(argv[0], argv);

    perror("exec");
    return 1;
}

/* ───────────── monitor helpers ───────────── */

static void register_monitor(pid_t pid, unsigned long soft, unsigned long hard)
{
    if (monitor_fd < 0) return;

    struct monitor_request req = {0};
    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;

    ioctl(monitor_fd, MONITOR_REGISTER, &req);
}

static void unregister_monitor(pid_t pid)
{
    if (monitor_fd < 0) return;

    struct monitor_request req = {0};
    req.pid = pid;

    ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
}

/* ───────────── logging ───────────── */

static void *log_reader(void *arg)
{
    Container *c = (Container *)arg;

    int fd = open(c->log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);

    char buf[4096];
    ssize_t n;

    while ((n = read(c->pipe_rd, buf, sizeof(buf))) > 0)
        write(fd, buf, n);

    close(fd);
    close(c->pipe_rd);
    return NULL;
}

/* ───────────── helpers ───────────── */

static Container *alloc_container()
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state == STATE_EMPTY)
            return &containers[i];
    return NULL;
}

static Container *find_container(const char *name)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state != STATE_EMPTY &&
            strcmp(containers[i].name, name) == 0)
            return &containers[i];
    return NULL;
}

/* ───────────── SIGCHLD ───────────── */

static void sigchld_handler(int sig)
{
    (void)sig;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].host_pid == pid) {
                containers[i].state = STATE_STOPPED;
                unregister_monitor(pid);
                break;
            }
        }
    }
}

/* ───────────── launch ───────────── */

static pid_t launch_container(const char *name, const char *rootfs,
                              char *argv[], int interactive,
                              unsigned long soft, unsigned long hard)
{
    Container *c = alloc_container();
    if (!c) return -1;

    memset(c, 0, sizeof(*c));
    strcpy(c->name, name);

    c->soft_limit = soft;
    c->hard_limit = hard;

    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, name);

    int pipefd[2];
    if (!interactive) {
        pipe(pipefd);
        c->pipe_rd = pipefd[0];
    }

    Args *args = calloc(1, sizeof(Args));
    strcpy(args->rootfs, rootfs);
    strcpy(args->name, name);
    args->interactive = interactive;

    int i = 0;
    while (argv[i]) {
        strcpy(args->cmd[i], argv[i]);
        i++;
    }
    args->argc = i;

    args->pipe_wr = interactive ? -1 : pipefd[1];

    pid_t pid = clone(container_main, clone_stack + STACK_SIZE,
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
        args);

    if (!interactive) {
        close(pipefd[1]);
        pthread_t t;
        pthread_create(&t, NULL, log_reader, c);
        pthread_detach(t);
    }

    c->host_pid = pid;
    c->state = STATE_RUNNING;

    register_monitor(pid, soft, hard);

    return pid;
}

/* ───────────── IPC ───────────── */

static void handle_client(int cfd)
{
    char buf[1024];
    int n = recv(cfd, buf, sizeof(buf)-1, 0);
    buf[n] = '\0';

    char *tok[16];
    int tc = 0;

    char *p = strtok(buf, " \n");
    while (p) {
        tok[tc++] = p;
        p = strtok(NULL, " \n");
    }

    char reply[1024] = {0};

    if (strcmp(tok[0], "start") == 0 || strcmp(tok[0], "run") == 0) {

        int interactive = strcmp(tok[0], "run") == 0;

        unsigned long soft = 40UL << 20;
        unsigned long hard = 64UL << 20;

        char *cmdv[MAX_CMD_ARGS];
        int cmdc = 0;

        for (int i = 3; i < tc; i++) {
            if (!strcmp(tok[i], "--soft-mib"))
                soft = strtoul(tok[++i], NULL, 10) << 20;
            else if (!strcmp(tok[i], "--hard-mib"))
                hard = strtoul(tok[++i], NULL, 10) << 20;
            else
                cmdv[cmdc++] = tok[i];
        }

        cmdv[cmdc] = NULL;

        pid_t pid = launch_container(tok[1], tok[2], cmdv,
                                     interactive, soft, hard);

        sprintf(reply, "OK pid=%d\n", pid);
    }

    else if (!strcmp(tok[0], "ps")) {
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].state != STATE_EMPTY) {
                char line[128];
                sprintf(line, "%s pid=%d\n",
                        containers[i].name,
                        containers[i].host_pid);
                strcat(reply, line);
            }
        }
    }

    else if (!strcmp(tok[0], "stop")) {
        Container *c = find_container(tok[1]);
        if (c) {
            kill(c->host_pid, SIGTERM);
            sprintf(reply, "OK stopped\n");
        }
    }

    send(cfd, reply, strlen(reply), 0);
    close(cfd);
}

/* ───────────── supervisor ───────────── */

static void run_supervisor()
{
    mkdir(LOG_DIR, 0755);

    monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (monitor_fd >= 0)
        printf("monitor connected\n");

    signal(SIGCHLD, sigchld_handler);

    unlink(SOCK_PATH);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 8);
    chmod(SOCK_PATH, 0777);

    while (1) {
        int cfd = accept(srv, NULL, NULL);
        handle_client(cfd);
    }
}

/* ───────────── client ───────────── */

static void run_client(int argc, char *argv[])
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    char buf[1024] = {0};

    for (int i = 1; i < argc; i++) {
        strcat(buf, argv[i]);
        strcat(buf, " ");
    }

    send(sock, buf, strlen(buf), 0);

    char reply[1024];
    int n = recv(sock, reply, sizeof(reply)-1, 0);
    reply[n] = '\0';

    printf("%s", reply);
    close(sock);
}

/* ───────────── main ───────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) return 1;

    if (!strcmp(argv[1], "supervisor"))
        run_supervisor();
    else
        run_client(argc, argv);

    return 0;
}
