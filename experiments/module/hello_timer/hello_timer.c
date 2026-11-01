#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>


static struct timer_list my_timer;
static int count;


static void timer_callback(struct timer_list* t) {
    count++;
    printk(KERN_INFO "hello_timer: tick #%d (jiffies=%lu)\n", count, jiffies);

    if (count < 10) {
        mod_timer(&my_timer, jiffies + msecs_to_jiffies(1000));  // 每秒一次，共 10 次
    }
}

static int __init hello_timer_init(void) {
    timer_setup(&my_timer, timer_callback, 0);
    mod_timer(&my_timer, jiffies + msecs_to_jiffies(1000));
    printk(KERN_INFO "hello_timer: armed, tick in 1s\n");
    return 0;
}


static void __exit hello_timer_exit(void) {
    del_timer_sync(&my_timer);
    printk(KERN_INFO "hello_timer: bye, total=%d\n", count);
}

module_init(hello_timer_init);
module_exit(hello_timer_exit);
MODULE_LICENSE("GPL");