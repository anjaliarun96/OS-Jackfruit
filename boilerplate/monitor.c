/*
 * monitor.c — Kernel Module: Container Memory Monitor
 *
 * Creates /dev/container_monitor.
 * Accepts ioctl commands from the user-space supervisor to register/unregister
 * container PIDs with soft and hard memory limits.
 * A periodic kernel timer checks RSS of each registered process:
 *   - Soft limit: logs a warning to dmesg on first breach.
 *   - Hard limit: sends SIGKILL to the process.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/pid.h>

#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit");
MODULE_DESCRIPTION("Container memory monitor LKM");
MODULE_VERSION("1.0");

/* ─── Internal tracked-process entry ───────────────────────────────────── */
struct tracked_proc {
    pid_t         pid;
    unsigned long soft_limit;   /* bytes */
    unsigned long hard_limit;   /* bytes */
    int           soft_warned;  /* 1 after first soft-limit warning */
    struct list_head list;
};

static LIST_HEAD(proc_list);
static DEFINE_MUTEX(proc_list_mu);

/* ─── Character device globals ──────────────────────────────────────────── */
#define DEVICE_NAME "container_monitor"
#define CLASS_NAME  "container"

static int            major_num;
static struct class  *monitor_class;
static struct cdev    monitor_cdev;
static dev_t          dev_num;

/* ─── Periodic timer ────────────────────────────────────────────────────── */
#define CHECK_INTERVAL_MS  2000   /* check every 2 seconds */
static struct timer_list monitor_timer;

/* Read RSS of a process in bytes. Returns 0 if process gone. */
static unsigned long get_rss_bytes(pid_t pid) {
    struct task_struct *task;
    unsigned long rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm) {
        rss = get_mm_rss(task->mm) << PAGE_SHIFT;
    }
    rcu_read_unlock();
    return rss;
}

static void check_limits(struct timer_list *t) {
    struct tracked_proc *entry, *tmp;

    mutex_lock(&proc_list_mu);
    list_for_each_entry_safe(entry, tmp, &proc_list, list) {
        unsigned long rss = get_rss_bytes(entry->pid);

        if (rss == 0) {
            /* Process no longer exists — remove stale entry */
            printk(KERN_INFO "container_monitor: PID %d gone, removing\n",
                   entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Hard limit check */
        if (entry->hard_limit > 0 && rss > entry->hard_limit) {
            printk(KERN_WARNING
                   "container_monitor: PID %d HARD LIMIT exceeded "
                   "(rss=%lu kB, limit=%lu kB) — sending SIGKILL\n",
                   entry->pid, rss >> 10, entry->hard_limit >> 10);
            struct pid *p = find_get_pid(entry->pid);
            if (p) {
                kill_pid(p, SIGKILL, 1);
                put_pid(p);
            }
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Soft limit check (warn once) */
        if (!entry->soft_warned && entry->soft_limit > 0 &&
            rss > entry->soft_limit) {
            printk(KERN_WARNING
                   "container_monitor: PID %d SOFT LIMIT exceeded "
                   "(rss=%lu kB, limit=%lu kB) — warning\n",
                   entry->pid, rss >> 10, entry->soft_limit >> 10);
            entry->soft_warned = 1;
        }
    }
    mutex_unlock(&proc_list_mu);

    /* Re-arm timer */
    mod_timer(&monitor_timer,
              jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
}

/* ─── ioctl handler ─────────────────────────────────────────────────────── */
static long monitor_ioctl(struct file *file, unsigned int cmd,
                          unsigned long user_arg) {
    switch (cmd) {

    case CONTAINER_MONITOR_REGISTER: {
        struct container_info ci;
        if (copy_from_user(&ci, (void __user *)user_arg, sizeof(ci)))
            return -EFAULT;
        if (ci.pid <= 0) return -EINVAL;

        struct tracked_proc *entry = kzalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) return -ENOMEM;
        entry->pid        = ci.pid;
        entry->soft_limit = ci.soft_limit;
        entry->hard_limit = ci.hard_limit;
        entry->soft_warned = 0;
        INIT_LIST_HEAD(&entry->list);

        mutex_lock(&proc_list_mu);
        list_add_tail(&entry->list, &proc_list);
        mutex_unlock(&proc_list_mu);

        printk(KERN_INFO
               "container_monitor: registered PID %d "
               "(soft=%lu kB, hard=%lu kB)\n",
               ci.pid, ci.soft_limit >> 10, ci.hard_limit >> 10);
        return 0;
    }

    case CONTAINER_MONITOR_UNREGISTER: {
        pid_t pid;
        if (copy_from_user(&pid, (void __user *)user_arg, sizeof(pid)))
            return -EFAULT;
        mutex_lock(&proc_list_mu);
        struct tracked_proc *entry, *tmp;
        list_for_each_entry_safe(entry, tmp, &proc_list, list) {
            if (entry->pid == pid) {
                list_del(&entry->list);
                kfree(entry);
                printk(KERN_INFO "container_monitor: unregistered PID %d\n", pid);
                break;
            }
        }
        mutex_unlock(&proc_list_mu);
        return 0;
    }

    default:
        return -ENOTTY;
    }
}

/* ─── File operations ───────────────────────────────────────────────────── */
static int monitor_open(struct inode *inode, struct file *file) {
    return 0;
}

static int monitor_release(struct inode *inode, struct file *file) {
    return 0;
}

static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .open           = monitor_open,
    .release        = monitor_release,
    .unlocked_ioctl = monitor_ioctl,
};

/* ─── Module init / exit ────────────────────────────────────────────────── */
static int __init monitor_init(void) {
    /* Allocate device number */
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0) {
        printk(KERN_ERR "container_monitor: alloc_chrdev_region failed\n");
        return -1;
    }
    major_num = MAJOR(dev_num);

    /* Create class */
    monitor_class = class_create(CLASS_NAME);
    if (IS_ERR(monitor_class)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(monitor_class);
    }

    /* Register cdev */
    cdev_init(&monitor_cdev, &monitor_fops);
    monitor_cdev.owner = THIS_MODULE;
    if (cdev_add(&monitor_cdev, dev_num, 1) < 0) {
        class_destroy(monitor_class);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    /* Create device node /dev/container_monitor */
    if (IS_ERR(device_create(monitor_class, NULL, dev_num, NULL, DEVICE_NAME))) {
        cdev_del(&monitor_cdev);
        class_destroy(monitor_class);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    /* Start periodic timer */
    timer_setup(&monitor_timer, check_limits, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));

    printk(KERN_INFO "container_monitor: Module loaded, major=%d\n", major_num);
    return 0;
}

static void __exit monitor_exit(void) {
    del_timer_sync(&monitor_timer);

    /* Free all tracked entries */
    mutex_lock(&proc_list_mu);
    struct tracked_proc *entry, *tmp;
    list_for_each_entry_safe(entry, tmp, &proc_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&proc_list_mu);

    device_destroy(monitor_class, dev_num);
    cdev_del(&monitor_cdev);
    class_destroy(monitor_class);
    unregister_chrdev_region(dev_num, 1);
    printk(KERN_INFO "container_monitor: Module unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
