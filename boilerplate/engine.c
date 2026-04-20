/*
 * engine.c — Multi-Container Runtime with Parent Supervisor
 *
 * Usage:
 *   sudo ./engine supervisor <rootfs>          # start long-running supervisor
 *   sudo ./engine start <n> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]
 *   sudo ./engine run   <n> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]
 *   sudo ./engine ps
 *   sudo ./engine logs  <n>
 *   sudo ./engine stop  <n>
 */

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
#include <stdatomic.h>

#include "monitor_ioctl.h"

/* ─── Constants ─────────────────────────────────────────────────────────── */
#define MAX_CONTAINERS   16
#define MAX_NAME         64
#define MAX_CMD_ARGS     32
#define SOCK_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR          "logs"
#define LOG_BUF_SLOTS    256
#define LOG_SLOT_SIZE    512
#define MONITOR_DEV      "/dev/container_monitor"

/* ─── Container state ────────────────────────────────────────────────────── */
typedef enum {
    STATE_EMPTY = 0,
    STATE_STARTING,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_HARD_LIMIT_KILLED,
} ContainerState;

static const char *state_str(ContainerState s) {
    switch (s) {
        case STATE_EMPTY:            return "empty";
        case STATE_STARTING:         return "starting";
        case STATE_RUNNING:          return "running";
        case STATE_STOPPED:          return "stopped";
        case STATE_KILLED:           return "killed";
        case STATE_HARD_LIMIT_KILLED:return "hard_limit_killed";
        default:                     return "unknown";
    }
}

typedef struct {
    char          name[MAX_NAME];
    pid_t         host_pid;
    time_t        start_time;
    ContainerState state;
    long          soft_mib;
    long          hard_mib;
    char          log_path[256];
    int           exit_status;
    int           exit_signal;
    int           stop_requested;
    int           log_pipe_rd;
    int           log_pipe_wr;
} Container;

/* ─── Bounded ring buffer (extended with log path per entry) ─────────────── */
typedef struct {
    char data[LOG_SLOT_SIZE];
    int  len;
    char log_path[256];
} LogEntry;

static LogEntry  log_entries[LOG_BUF_SLOTS];
static int       le_head = 0, le_tail = 0, le_count = 0;
static pthread_mutex_t le_mu   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  le_ne   = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  le_nf   = PTHREAD_COND_INITIALIZER;
static volatile int    le_shut = 0;

static void le_push(const char *data, int len, const char *path) {
    if (len <= 0) return;
    pthread_mutex_lock(&le_mu);
    while (le_count == LOG_BUF_SLOTS && !le_shut)
        pthread_cond_wait(&le_nf, &le_mu);
    if (le_shut) { pthread_mutex_unlock(&le_mu); return; }
    LogEntry *e = &log_entries[le_head];
    int copy = len < LOG_SLOT_SIZE - 1 ? len : LOG_SLOT_SIZE - 1;
    memcpy(e->data, data, copy);
    e->data[copy] = '\0';
    e->len = copy;
    strncpy(e->log_path, path, sizeof(e->log_path) - 1);
    e->log_path[sizeof(e->log_path)-1] = '\0';
    le_head = (le_head + 1) % LOG_BUF_SLOTS;
    le_count++;
    pthread_cond_signal(&le_ne);
    pthread_mutex_unlock(&le_mu);
}

/* Consumer thread */
static void *logger_consumer(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&le_mu);
        while (le_count == 0 && !le_shut)
            pthread_cond_wait(&le_ne, &le_mu);
        if (le_count == 0 && le_shut) {
            pthread_mutex_unlock(&le_mu);
            break;
        }
        LogEntry e = log_entries[le_tail];
        le_tail = (le_tail + 1) % LOG_BUF_SLOTS;
        le_count--;
        pthread_cond_signal(&le_nf);
        pthread_mutex_unlock(&le_mu);

        int fd = open(e.log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t written = write(fd, e.data, e.len);
            (void)written;
            close(fd);
        }
    }
    return NULL;
}

/* Per-container producer thread */
typedef struct { int pipe_fd; char log_path[256]; } PipeReaderArg;

static void *pipe_reader(void *arg) {
    PipeReaderArg *a = (PipeReaderArg *)arg;
    char buf[LOG_SLOT_SIZE];
    ssize_t n;
    while ((n = read(a->pipe_fd, buf, sizeof(buf) - 1)) > 0) {
        le_push(buf, (int)n, a->log_path);
    }
    close(a->pipe_fd);
    free(a);
    return NULL;
}

/* ─── Shared supervisor state ────────────────────────────────────────────── */
static Container    containers[MAX_CONTAINERS];
static pthread_mutex_t containers_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t    logger_thread;
static int          monitor_fd = -1;
static volatile sig_atomic_t supervisor_running = 1;

/* ─── Helpers ────────────────────────────────────────────────────────────── */
static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static Container *find_container(const char *name) {
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].state != STATE_EMPTY &&
            strcmp(containers[i].name, name) == 0)
            return &containers[i];
    }
    return NULL;
}

static Container *alloc_container(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].state == STATE_EMPTY)
            return &containers[i];
    }
    return NULL;
}

/* ─── Namespace / container setup ───────────────────────────────────────── */
typedef struct {
    char rootfs[256];
    char cmd[MAX_CMD_ARGS][256];
    int  cmd_argc;
    int  pipe_wr;
    char name[MAX_NAME];
} ContainerArgs;

static int container_main(void *arg) {
    ContainerArgs *a = (ContainerArgs *)arg;

    /* Redirect stdout+stderr to supervisor pipe FIRST so all errors are captured */
    if (dup2(a->pipe_wr, STDOUT_FILENO) < 0) _exit(1);
    if (dup2(a->pipe_wr, STDERR_FILENO) < 0) _exit(1);
    close(a->pipe_wr);

    /* Set hostname (UTS namespace) */
    if (sethostname(a->name, strlen(a->name)) < 0)
        fprintf(stderr, "[container:%s] sethostname failed: %s\n",
                a->name, strerror(errno));

    /* Mount /proc — dir should already exist in a valid Alpine rootfs */
    char proc_path[512];
    snprintf(proc_path, sizeof(proc_path), "%s/proc", a->rootfs);
    mkdir(proc_path, 0555);   /* no-op if exists */
    if (mount("proc", proc_path, "proc", 0, NULL) < 0)
        fprintf(stderr, "[container:%s] mount proc failed: %s\n",
                a->name, strerror(errno));

    /* chroot */
    if (chroot(a->rootfs) < 0) {
        fprintf(stderr, "[container:%s] chroot('%s') failed: %s\n",
                a->name, a->rootfs, strerror(errno));
        fflush(stderr);
        return 1;
    }
    if (chdir("/") < 0) {
        fprintf(stderr, "[container:%s] chdir / failed: %s\n",
                a->name, strerror(errno));
        return 1;
    }

    /* Build argv and exec */
    char *argv[MAX_CMD_ARGS + 1];
    for (int i = 0; i < a->cmd_argc; i++) argv[i] = a->cmd[i];
    argv[a->cmd_argc] = NULL;

    execv(argv[0], argv);
    fprintf(stderr, "[container:%s] execv('%s') failed: %s\n",
            a->name, argv[0], strerror(errno));
    return 127;
}

/* Stack for clone() */
#define STACK_SIZE (1024 * 1024)
static char clone_stack[STACK_SIZE];

/* ─── SIGCHLD handler ────────────────────────────────────────────────────── */
static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&containers_mu);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].host_pid == pid &&
                containers[i].state == STATE_RUNNING) {
                if (containers[i].stop_requested) {
                    containers[i].state = STATE_STOPPED;
                } else if (WIFSIGNALED(status) &&
                           WTERMSIG(status) == SIGKILL) {
                    containers[i].state = STATE_HARD_LIMIT_KILLED;
                } else {
                    containers[i].state = STATE_KILLED;
                }
                if (WIFEXITED(status))
                    containers[i].exit_status = WEXITSTATUS(status);
                if (WIFSIGNALED(status))
                    containers[i].exit_signal = WTERMSIG(status);
                if (containers[i].log_pipe_rd >= 0) {
                    close(containers[i].log_pipe_rd);
                    containers[i].log_pipe_rd = -1;
                }
                break;
            }
        }
        pthread_mutex_unlock(&containers_mu);
    }
    errno = saved_errno;
}

/* ─── SIGTERM/SIGINT handler ─────────────────────────────────────────────── */
static void sigterm_handler(int sig) {
    (void)sig;
    supervisor_running = 0;
}

/* ─── Kernel monitor integration ─────────────────────────────────────────── */
static void monitor_register(pid_t pid, long soft_mib, long hard_mib) {
    if (monitor_fd < 0) return;
    struct container_info ci;
    ci.pid        = pid;
    ci.soft_limit = (unsigned long)soft_mib * 1024 * 1024;
    ci.hard_limit = (unsigned long)hard_mib * 1024 * 1024;
    if (ioctl(monitor_fd, CONTAINER_MONITOR_REGISTER, &ci) < 0)
        fprintf(stderr, "[supervisor] ioctl register failed: %s\n", strerror(errno));
    else
        printf("[supervisor] registered PID %d with kernel monitor "
               "(soft=%ldMiB hard=%ldMiB)\n", pid, soft_mib, hard_mib);
}

static void monitor_unregister(pid_t pid) {
    if (monitor_fd < 0) return;
    if (ioctl(monitor_fd, CONTAINER_MONITOR_UNREGISTER, &pid) < 0)
        fprintf(stderr, "[supervisor] ioctl unregister failed: %s\n", strerror(errno));
}

/* ─── Launch a container ─────────────────────────────────────────────────── */
static pid_t launch_container(const char *name, const char *rootfs,
                               char *argv[], long soft_mib, long hard_mib) {
    pthread_mutex_lock(&containers_mu);
    if (find_container(name)) {
        pthread_mutex_unlock(&containers_mu);
        fprintf(stderr, "[supervisor] container '%s' already exists\n", name);
        return -1;
    }
    Container *c = alloc_container();
    if (!c) {
        pthread_mutex_unlock(&containers_mu);
        fprintf(stderr, "[supervisor] too many containers\n");
        return -1;
    }

    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) {
        pthread_mutex_unlock(&containers_mu);
        perror("pipe2");
        return -1;
    }

    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, MAX_NAME - 1);
    c->state       = STATE_STARTING;
    c->soft_mib    = soft_mib;
    c->hard_mib    = hard_mib;
    c->start_time  = time(NULL);
    c->log_pipe_rd = pipefd[0];
    c->log_pipe_wr = pipefd[1];
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, name);

    ContainerArgs *args = calloc(1, sizeof(ContainerArgs));
    strncpy(args->rootfs, rootfs, sizeof(args->rootfs) - 1);
    strncpy(args->name, name, sizeof(args->name) - 1);
    /* Pass the write-end fd — NOTE: pipe2 used O_CLOEXEC so we must
       clear the flag on pipefd[1] so the child (clone) inherits it */
    int wr = dup(pipefd[1]);   /* dup without O_CLOEXEC */
    args->pipe_wr = wr;
    int argc = 0;
    while (argv[argc] && argc < MAX_CMD_ARGS) {
        strncpy(args->cmd[argc], argv[argc], 255);
        argc++;
    }
    args->cmd_argc = argc;

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(container_main, clone_stack + STACK_SIZE, flags, args);
    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        close(pipefd[1]);
        close(wr);
        memset(c, 0, sizeof(*c));
        pthread_mutex_unlock(&containers_mu);
        free(args);
        return -1;
    }

    /* Close write ends in supervisor */
    close(pipefd[1]);
    close(wr);
    c->log_pipe_wr = -1;
    c->host_pid = pid;
    c->state    = STATE_RUNNING;
    pthread_mutex_unlock(&containers_mu);
    free(args);

    /* Start pipe-reader producer thread */
    PipeReaderArg *pra = malloc(sizeof(PipeReaderArg));
    pra->pipe_fd = pipefd[0];
    strncpy(pra->log_path, c->log_path, sizeof(pra->log_path) - 1);
    pra->log_path[sizeof(pra->log_path)-1] = '\0';
    pthread_t pt;
    pthread_create(&pt, NULL, pipe_reader, pra);
    pthread_detach(pt);

    if (soft_mib > 0 || hard_mib > 0)
        monitor_register(pid, soft_mib, hard_mib);

    printf("[supervisor] container '%s' started, host PID %d\n", name, pid);
    return pid;
}

/* ─── IPC control channel ────────────────────────────────────────────────── */
#define CMD_BUF 4096

static void handle_client(int cfd) {
    char buf[CMD_BUF];
    ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(cfd); return; }
    buf[n] = '\0';

    char *tokens[32];
    int  tc = 0;
    char *p = strtok(buf, " \t\n");
    while (p && tc < 32) { tokens[tc++] = p; p = strtok(NULL, " \t\n"); }
    if (tc == 0) { close(cfd); return; }

    char reply[CMD_BUF];
    memset(reply, 0, sizeof(reply));

    if (strcmp(tokens[0], "start") == 0 || strcmp(tokens[0], "run") == 0) {
        if (tc < 4) {
            snprintf(reply, sizeof(reply), "ERR usage: start <n> <rootfs> <cmd>\n");
        } else {
            const char *cname  = tokens[1];
            const char *rootfs = tokens[2];
            long soft = 0, hard = 0;
            char *cmdv[MAX_CMD_ARGS];
            int   cmdc = 0;
            for (int i = 3; i < tc; i++) {
                if (strcmp(tokens[i], "--soft-mib") == 0 && i + 1 < tc)
                    soft = atol(tokens[++i]);
                else if (strcmp(tokens[i], "--hard-mib") == 0 && i + 1 < tc)
                    hard = atol(tokens[++i]);
                else
                    cmdv[cmdc++] = tokens[i];
            }
            cmdv[cmdc] = NULL;
            pid_t pid = launch_container(cname, rootfs, cmdv, soft, hard);
            if (pid < 0)
                snprintf(reply, sizeof(reply), "ERR failed to launch container\n");
            else
                snprintf(reply, sizeof(reply), "OK started %s pid=%d\n", cname, pid);

            if (strcmp(tokens[0], "run") == 0 && pid > 0) {
                int status;
                waitpid(pid, &status, 0);
                snprintf(reply, sizeof(reply),
                         "OK container %s exited (status=%d)\n",
                         cname, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            }
        }
    } else if (strcmp(tokens[0], "ps") == 0) {
        char *out = reply;
        int   rem = sizeof(reply);
        int used = snprintf(out, rem,
            "%-16s %-8s %-20s %-22s %-8s %-8s\n",
            "NAME","PID","START","STATE","SOFT_MIB","HARD_MIB");
        out += used; rem -= used;
        pthread_mutex_lock(&containers_mu);
        for (int i = 0; i < MAX_CONTAINERS && rem > 0; i++) {
            Container *c = &containers[i];
            if (c->state == STATE_EMPTY) continue;
            char tbuf[32];
            struct tm *tm = localtime(&c->start_time);
            strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm);
            used = snprintf(out, rem,
                "%-16s %-8d %-20s %-22s %-8ld %-8ld\n",
                c->name, c->host_pid, tbuf, state_str(c->state),
                c->soft_mib, c->hard_mib);
            out += used; rem -= used;
        }
        pthread_mutex_unlock(&containers_mu);
    } else if (strcmp(tokens[0], "logs") == 0) {
        if (tc < 2) {
            snprintf(reply, sizeof(reply), "ERR usage: logs <n>\n");
        } else {
            pthread_mutex_lock(&containers_mu);
            Container *c = find_container(tokens[1]);
            if (!c) {
                snprintf(reply, sizeof(reply), "ERR unknown container %s\n", tokens[1]);
                pthread_mutex_unlock(&containers_mu);
            } else {
                char path[256];
                strncpy(path, c->log_path, sizeof(path) - 1);
                path[sizeof(path)-1] = '\0';
                pthread_mutex_unlock(&containers_mu);
                /* Flush buffer before reading */
                usleep(100000);
                FILE *f = fopen(path, "r");
                if (!f) {
                    snprintf(reply, sizeof(reply),
                             "ERR cannot open log '%s': %s\n", path, strerror(errno));
                } else {
                    int cnt = 0;
                    char line[512];
                    while (fgets(line, sizeof(line), f) && cnt < 200) {
                        strncat(reply, line, sizeof(reply) - strlen(reply) - 2);
                        cnt++;
                    }
                    fclose(f);
                    if (strlen(reply) == 0)
                        snprintf(reply, sizeof(reply), "(log is empty)\n");
                }
            }
        }
    } else if (strcmp(tokens[0], "stop") == 0) {
        if (tc < 2) {
            snprintf(reply, sizeof(reply), "ERR usage: stop <n>\n");
        } else {
            pthread_mutex_lock(&containers_mu);
            Container *c = find_container(tokens[1]);
            if (!c || c->state != STATE_RUNNING) {
                snprintf(reply, sizeof(reply),
                         "ERR container %s not running\n", tokens[1]);
                pthread_mutex_unlock(&containers_mu);
            } else {
                c->stop_requested = 1;
                pid_t pid = c->host_pid;
                pthread_mutex_unlock(&containers_mu);
                monitor_unregister(pid);
                kill(pid, SIGTERM);
                usleep(300000);
                pthread_mutex_lock(&containers_mu);
                if (c->state == STATE_RUNNING)
                    kill(pid, SIGKILL);
                pthread_mutex_unlock(&containers_mu);
                snprintf(reply, sizeof(reply),
                         "OK stop signaled for %s\n", tokens[1]);
            }
        }
    } else {
        snprintf(reply, sizeof(reply), "ERR unknown command: %s\n", tokens[0]);
    }

    send(cfd, reply, strlen(reply), 0);
    close(cfd);
}

/* ─── Supervisor main loop ───────────────────────────────────────────────── */
static void run_supervisor(const char *rootfs) {
    (void)rootfs;
    printf("[supervisor] starting\n");

    mkdir(LOG_DIR, 0755);

    le_shut = 0;
    pthread_create(&logger_thread, NULL, logger_consumer, NULL);

    monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr, "[supervisor] kernel monitor unavailable: %s\n", strerror(errno));
    else
        printf("[supervisor] kernel monitor open at %s\n", MONITOR_DEV);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) die("socket");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(srv, 8) < 0) die("listen");
    chmod(SOCK_PATH, 0777);
    printf("[supervisor] control socket at %s\n", SOCK_PATH);
    printf("[supervisor] ready. Use 'engine start/ps/logs/stop' in another terminal.\n");
    fflush(stdout);

    while (supervisor_running) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (!supervisor_running) break;
            perror("accept");
            continue;
        }
        handle_client(cfd);
    }

    printf("[supervisor] shutting down...\n");
    pthread_mutex_lock(&containers_mu);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].state == STATE_RUNNING) {
            containers[i].stop_requested = 1;
            kill(containers[i].host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&containers_mu);
    sleep(1);

    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {}

    pthread_mutex_lock(&le_mu);
    le_shut = 1;
    pthread_cond_broadcast(&le_ne);
    pthread_mutex_unlock(&le_mu);
    pthread_join(logger_thread, NULL);

    if (monitor_fd >= 0) close(monitor_fd);
    close(srv);
    unlink(SOCK_PATH);
    printf("[supervisor] clean exit. No zombies.\n");
}

/* ─── CLI client ─────────────────────────────────────────────────────────── */
static void run_client(int argc, char *argv[]) {
    char buf[CMD_BUF];
    memset(buf, 0, sizeof(buf));
    for (int i = 1; i < argc; i++) {
        strncat(buf, argv[i], sizeof(buf) - strlen(buf) - 2);
        if (i < argc - 1) strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
    }
    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) die("socket");
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
            "Cannot connect to supervisor at %s: %s\n"
            "Is the supervisor running? Try: sudo ./engine supervisor ../rootfs-base\n",
            SOCK_PATH, strerror(errno));
        close(sock);
        exit(1);
    }
    send(sock, buf, strlen(buf), 0);

    char reply[CMD_BUF * 4];
    ssize_t n;
    int total = 0;
    while ((n = recv(sock, reply + total,
                     sizeof(reply) - total - 1, 0)) > 0)
        total += n;
    reply[total] = '\0';
    if (strncmp(reply, "ERR", 3) == 0) {
        fprintf(stderr, "%s", reply);
        close(sock);
        exit(1);
    }
    printf("%s", reply);
    close(sock);
}

/* ─── main ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <rootfs>\n"
            "  %s start <n> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
            "  %s run   <n> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
            "  %s ps\n"
            "  %s logs  <n>\n"
            "  %s stop  <n>\n",
            argv[0],argv[0],argv[0],argv[0],argv[0],argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "supervisor needs <rootfs>\n");
            return 1;
        }
        run_supervisor(argv[2]);
    } else {
        run_client(argc, argv);
    }
    return 0;
}
