#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_IOC_MAGIC  'M'

/* Information passed from user-space to kernel when registering a container */
struct container_info {
    pid_t         pid;
    unsigned long soft_limit;   /* bytes */
    unsigned long hard_limit;   /* bytes */
};

/* ioctl commands */
#define CONTAINER_MONITOR_REGISTER   _IOW(MONITOR_IOC_MAGIC, 1, struct container_info)
#define CONTAINER_MONITOR_UNREGISTER _IOW(MONITOR_IOC_MAGIC, 2, pid_t)

#endif /* MONITOR_IOCTL_H */

