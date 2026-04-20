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
typedef struct {
    char name[MAX_NAME];
    pid_t host_pid;
    time_t start_time;
    ContainerState state;
} Container;

c->host_pid = pid;
c->state = STATE_RUNNING;
c->start_time = time(NULL);

static Container containers[MAX_CONTAINERS];
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


/* ─── Shared supervisor state ────────────────────────────────────────────── */
static pthread_mutex_t containers_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t    logger_thread;
static int          monitor_fd = -1;
static volatile sig_atomic_t supervisor_running = 1;

/* ─── Helpers ────────────────────────────────────────────────────────────── */
static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
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
static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    int status;
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
void run_supervisor() {
    printf("Supervisor running\n");
    launch_container("alpha", "../rootfs-alpha", ...);
launch_container("beta", "../rootfs-beta", ...);

    while (1) {
        sleep(5);
    }
}
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
/* ─── main ───────────────────────────────────────────────────────────────── */
int main() {
    char *argv[] = {"/bin/sh", NULL};

    ContainerArgs args;
    strcpy(args.rootfs, "../rootfs-alpha");
    strcpy(args.name, "test");

    args.cmd_argc = 1;
    strcpy(args.cmd[0], "/bin/sh");

    args.pipe_wr = STDOUT_FILENO;

    clone(container_main, clone_stack + STACK_SIZE,
          CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
          &args);
    if (strcmp(argv[1], "supervisor") == 0) {
    run_supervisor();
}

    wait(NULL);
}
