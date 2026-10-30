#include <linux/module.h>

#include <linux/kernel.h>

#include "my_add.h"

static int base = 0;

module_param(base, int, 0644);

static int __init add_init(void) {
    printk(KERN_INFO "add module init.base= %d..\n", base);

    return 0;
}

static void add_exit(void) {
    printk(KERN_INFO "add module exit...\n");
}

int my_add(int a, int b);

int my_add(int a, int b) {
    return a + b;
}



module_init(add_init);
module_exit(add_exit);

EXPORT_SYMBOL(my_add);

MODULE_LICENSE("GPL");