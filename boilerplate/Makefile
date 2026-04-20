# Makefile for OS-Jackfruit: Multi-Container Runtime
#
# Usage:
#   make              — build everything
#   make engine       — build user-space runtime only
#   make monitor      — build kernel module only
#   make workloads    — build test workload binaries
#   make clean        — remove all build artifacts

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

# User-space flags
CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -pthread -D_GNU_SOURCE
LDFLAGS  = -pthread

# Kernel module sources
obj-m := monitor.o

.PHONY: all engine monitor workloads clean

all: engine monitor workloads

# ── User-space engine ──────────────────────────────────────────────────────
engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine engine.c $(LDFLAGS)

# ── Kernel module ──────────────────────────────────────────────────────────
monitor: monitor.c monitor_ioctl.h
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules
	@echo "Kernel module built: monitor.ko"

# ── Workload binaries ─────────────────────────────────────────────────────
workloads: cpu_hog io_pulse memory_hog

cpu_hog: cpu_hog.c
	$(CC) $(CFLAGS) -o cpu_hog cpu_hog.c

io_pulse: io_pulse.c
	$(CC) $(CFLAGS) -o io_pulse io_pulse.c

memory_hog: memory_hog.c
	$(CC) $(CFLAGS) -o memory_hog memory_hog.c

# ── Clean ─────────────────────────────────────────────────────────────────
clean:
	rm -f engine cpu_hog io_pulse memory_hog
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean 2>/dev/null || true
	rm -f *.o *.ko *.mod *.mod.c *.symvers *.order .*.cmd
	rm -rf .tmp_versions

