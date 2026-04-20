/*
 * io_pulse.c — I/O-bound workload for scheduling experiments (Task 5)
 *
 * Usage: ./io_pulse [seconds]   (default: 30 seconds)
 *
 * Repeatedly writes and reads a temp file to generate I/O load.
 * Mostly sleeps while waiting for I/O, leaving the CPU idle.
 * Useful for demonstrating how the Linux CFS scheduler treats I/O-bound
 * vs CPU-bound processes differently.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#define BUF_SIZE (4 * 1024)    /* 4 KB */

static volatile int running = 1;
static void handle_sig(int s) { (void)s; running = 0; }

int main(int argc, char *argv[]) {
    int seconds = 30;
    if (argc > 1) seconds = atoi(argv[1]);

    signal(SIGTERM, handle_sig);
    signal(SIGINT,  handle_sig);

    printf("[io_pulse] PID=%d, running for %ds\n", (int)getpid(), seconds);
    fflush(stdout);

    char buf[BUF_SIZE];
    memset(buf, 'X', sizeof(buf));

    time_t start = time(NULL);
    long   ops   = 0;

    while (running) {
        time_t elapsed = time(NULL) - start;
        if (elapsed >= seconds) break;

        /* Write to a temp file */
        FILE *f = fopen("/tmp/io_pulse_tmp", "w");
        if (f) {
            fwrite(buf, 1, sizeof(buf), f);
            fflush(f);
            fclose(f);
        }
        /* Read it back */
        f = fopen("/tmp/io_pulse_tmp", "r");
        if (f) {
            fread(buf, 1, sizeof(buf), f);
            fclose(f);
        }
        ops++;

        if (ops % 100 == 0) {
            printf("[io_pulse] elapsed=%lds ops=%ld\n", elapsed, ops);
            fflush(stdout);
        }
        /* Small sleep to model I/O wait */
        usleep(5000);
    }

    time_t total = time(NULL) - start;
    printf("[io_pulse] done. total_ops=%ld elapsed=%lds\n", ops, total);
    return 0;
}

