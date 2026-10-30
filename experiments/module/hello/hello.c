#include <linux/module.h>

#include <linux/kernel.h>

#include "../my_add/my_add.h"

// extern int my_add(int a, int b);

static int __init hello_init(void) {
    printk(KERN_INFO "hello init...\n");

    printk(KERN_INFO "10 + 23 = %d\n", my_add(10, 23));

    return 0;
}

static void hello_exit(void) {
    printk(KERN_INFO "hello exit...\n");

}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
