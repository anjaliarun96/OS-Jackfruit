#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/delay.h>
#include <linux/kthread.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"

MODULE_LICENSE("GPL");

struct monitor_entry {
    struct list_head list;
    pid_t pid;
    unsigned long soft_limit;
    unsigned long hard_limit;
    int warned;
};

static LIST_HEAD(entry_list);
static DEFINE_MUTEX(entry_lock);

static int major;
static struct task_struct *monitor_thread;

/* ───────────── RSS HELPER ───────────── */

static unsigned long get_rss(struct task_struct *task)
{
    struct mm_struct *mm = get_task_mm(task);
    unsigned long rss = 0;

    if (mm) {
        rss = get_mm_rss(mm) << PAGE_SHIFT;
        mmput(mm);
    }

    return rss;
}

/* ───────────── MONITOR THREAD ───────────── */

static int monitor_fn(void *data)
{
    while (!kthread_should_stop()) {

        mutex_lock(&entry_lock);

        struct monitor_entry *e, *tmp;

        list_for_each_entry_safe(e, tmp, &entry_list, list) {

            struct task_struct *task = pid_task(find_vpid(e->pid), PIDTYPE_PID);

            if (!task) {
                printk(KERN_INFO "[monitor] removing dead PID %d\n", e->pid);
                list_del(&e->list);
                kfree(e);
                continue;
            }

            unsigned long rss = get_rss(task);

            if (rss > e->hard_limit) {
                printk(KERN_WARNING "[monitor] PID %d exceeded HARD limit (%lu > %lu), killing\n",
                       e->pid, rss, e->hard_limit);

                send_sig(SIGKILL, task, 0);

                list_del(&e->list);
                kfree(e);
                continue;
            }

            if (rss > e->soft_limit && !e->warned) {
                printk(KERN_WARNING "[monitor] PID %d exceeded SOFT limit (%lu > %lu)\n",
                       e->pid, rss, e->soft_limit);
                e->warned = 1;
            }
        }

        mutex_unlock(&entry_lock);

        msleep(1000); // check every 1 second
    }

    return 0;
}

/* ───────────── IOCTL HANDLER ───────────── */

static long monitor_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {

        struct monitor_entry *e = kmalloc(sizeof(*e), GFP_KERNEL);
        if (!e) return -ENOMEM;

        e->pid = req.pid;
        e->soft_limit = req.soft_limit_bytes;
        e->hard_limit = req.hard_limit_bytes;
        e->warned = 0;

        mutex_lock(&entry_lock);
        list_add(&e->list, &entry_list);
        mutex_unlock(&entry_lock);

        printk(KERN_INFO "[monitor] registered PID %d (soft=%lu hard=%lu)\n",
               e->pid, e->soft_limit, e->hard_limit);

        return 0;
    }

    else if (cmd == MONITOR_UNREGISTER) {

        struct monitor_entry *e, *tmp;

        mutex_lock(&entry_lock);

        list_for_each_entry_safe(e, tmp, &entry_list, list) {
            if (e->pid == req.pid) {
                list_del(&e->list);
                kfree(e);
                printk(KERN_INFO "[monitor] unregistered PID %d\n", req.pid);
                break;
            }
        }

        mutex_unlock(&entry_lock);
        return 0;
    }

    return -EINVAL;
}

/* ───────────── FILE OPS ───────────── */

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ───────────── INIT / EXIT ───────────── */

static int __init monitor_init(void)
{
    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) {
        printk(KERN_ERR "[monitor] failed to register device\n");
        return major;
    }

    printk(KERN_INFO "[monitor] loaded (major=%d)\n", major);

    monitor_thread = kthread_run(monitor_fn, NULL, "container_monitor");
    if (IS_ERR(monitor_thread)) {
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(monitor_thread);
    }

    return 0;
}

static void __exit monitor_exit(void)
{
    struct monitor_entry *e, *tmp;

    if (monitor_thread)
        kthread_stop(monitor_thread);

    mutex_lock(&entry_lock);

    list_for_each_entry_safe(e, tmp, &entry_list, list) {
        list_del(&e->list);
        kfree(e);
    }

    mutex_unlock(&entry_lock);

    unregister_chrdev(major, DEVICE_NAME);

    printk(KERN_INFO "[monitor] unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
