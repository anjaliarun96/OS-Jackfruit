/*
 * cpu_hog.c — CPU-bound workload for scheduling experiments (Task 5)
 *
 * Usage: ./cpu_hog [seconds]   (default: 30 seconds)
 *
 * Continuously computes a busy loop and periodically prints its progress.
 * Used to measure CPU share when run alongside other cpu_hog instances
 * with different nice values or CPU affinities.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>

static volatile int running = 1;
static void handle_sig(int s) { (void)s; running = 0; }

int main(int argc, char *argv[]) {
    int seconds = 30;
    if (argc > 1) seconds = atoi(argv[1]);

    signal(SIGTERM, handle_sig);
    signal(SIGINT,  handle_sig);

    printf("[cpu_hog] PID=%d, running for %ds\n", (int)getpid(), seconds);
    fflush(stdout);

    time_t start = time(NULL);
    long long count = 0;

    while (running) {
        /* Busy computation — prevent optimizer from eliminating loop */
        for (volatile long i = 0; i < 100000L; i++);
        count++;

        time_t elapsed = time(NULL) - start;
        if (elapsed >= seconds) break;

        if (count % 500 == 0) {
            printf("[cpu_hog] elapsed=%lds iterations=%lld\n",
                   elapsed, count);
            fflush(stdout);
        }
    }

    time_t total = time(NULL) - start;
    printf("[cpu_hog] done. total_iterations=%lld elapsed=%lds\n",
           count, total);
    return 0;
}

