#include <linux/module.h>

#include <linux/kernel.h>



static int __init hello_init(void) {
    printk(KERN_INFO "hello init...\n");

    return 0;
}

static void hello_exit(void) {
    printk(KERN_INFO "hello exit...\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
