#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#define MONITOR_IOCTL_MAGIC 'M'

#define MONITOR_REGISTER   _IOW(MONITOR_IOCTL_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_IOCTL_MAGIC, 2, struct monitor_request)

struct monitor_request {
    pid_t pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
};

#endif
