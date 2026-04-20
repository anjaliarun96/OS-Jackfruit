/*
 * memory_hog.c — Memory-consuming workload for kernel monitor testing (Task 4)
 *
 * Usage: ./memory_hog [target_mib] [seconds]   (default: 64 MiB for 20s)
 *
 * Gradually allocates memory to test soft and hard memory limits enforced
 * by the kernel monitor. Touches each page to actually count toward RSS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

static volatile int running = 1;
static void handle_sig(int s) { (void)s; running = 0; }

int main(int argc, char *argv[]) {
    long target_mib = 64;
    int  seconds    = 20;
    if (argc > 1) target_mib = atol(argv[1]);
    if (argc > 2) seconds    = atoi(argv[2]);

    signal(SIGTERM, handle_sig);
    signal(SIGINT,  handle_sig);

    printf("[memory_hog] PID=%d, target=%ldMiB over %ds\n",
           (int)getpid(), target_mib, seconds);
    fflush(stdout);

    long page = 4096;
    long total_bytes = target_mib * 1024 * 1024;
    long chunk       = 4 * 1024 * 1024;   /* allocate 4 MiB at a time */
    long allocated   = 0;

    time_t start = time(NULL);

    while (running && allocated < total_bytes) {
        char *p = malloc(chunk);
        if (!p) {
            fprintf(stderr, "[memory_hog] malloc failed at %ld MiB\n",
                    allocated >> 20);
            break;
        }
        /* Touch every page to force RSS */
        for (long i = 0; i < chunk; i += page)
            p[i] = 1;
        allocated += chunk;

        printf("[memory_hog] allocated=%ld MiB\n", allocated >> 20);
        fflush(stdout);
        sleep(1);

        if ((time(NULL) - start) >= seconds) break;
    }

    printf("[memory_hog] holding %ld MiB, sleeping...\n", allocated >> 20);
    fflush(stdout);

    /* Hold memory until killed or timeout */
    time_t hold_start = time(NULL);
    while (running && (time(NULL) - hold_start) < seconds)
        sleep(1);

    printf("[memory_hog] exiting normally\n");
    return 0;
}

