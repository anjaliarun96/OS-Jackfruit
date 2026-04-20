#include <stdio.h>
#include <unistd.h>

int main() {
    printf("CPU hog started\n");

    volatile unsigned long long i = 0;

    while (1) {
        i++;
        if (i % 1000000000ULL == 0) {
            printf("still running...\n");
        }
    }

    return 0;
}
