#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>


static struct hrtimer my_hrtimer;
static int count;
static ktime_t last_time;

static enum hrtimer_restart hrtimer_callback(struct hrtimer* t) {
    ktime_t now = ktime_get();
    s64 delta_ns = ktime_to_ns(ktime_sub(now, last_time));
    last_time = now;

    count++;
    printk(KERN_INFO "hello_hrtimer: tick #%d, interval=%lld ns (%lld.%03lld ms)\n",
           count, delta_ns, delta_ns / 1000000, (delta_ns % 1000000) / 1000);

    if (count < 10) {
        hrtimer_forward_now(t, ms_to_ktime(1000));
        return HRTIMER_RESTART;
    }

    return HRTIMER_NORESTART;
}

static int __init hello_hrtimer_init(void) {
    hrtimer_init(&my_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    my_hrtimer.function = hrtimer_callback; 
    last_time = ktime_get();
    hrtimer_start(&my_hrtimer, ms_to_ktime(1000), HRTIMER_MODE_REL);
    printk(KERN_INFO "hello_hrtimer: armed, tick in 1s\n");

    return 0;
}

static void __exit hello_hrtimer_exit(void)
{
    hrtimer_cancel(&my_hrtimer);
    printk(KERN_INFO "hello_hrtimer: bye, total=%d\n", count);
}

module_init(hello_hrtimer_init);
module_exit(hello_hrtimer_exit);
MODULE_LICENSE("GPL");