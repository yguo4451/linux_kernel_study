
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>


#define DEVICE_NAME "hello_dev"


static dev_t dev_num;  //设备号
static struct cdev hello_cdev;  // 字符设备
static char msg[] = "Hello from kernel!\n";

static int hello_open(struct inode* inode, struct file* file) {
    return 0;
}

static ssize_t hello_read(struct file* file, char __user *buf, size_t len, loff_t* off) {
    int msg_len = strlen(msg);

    if (*off >= msg_len) {
        return 0;
    }
    if (len > msg_len - *off) {
        len = msg_len - *off;
    }

    if (copy_to_user(buf, msg + *off, len)) {
        return -EFAULT;
    }

    *off += len;
    return len;
}

static int hello_release(struct inode* inode, struct file* file) {
    return 0;
}

static struct file_operations hello_fops = {
    .owner = THIS_MODULE,
    .open = hello_open,
    .read = hello_read,
    .release = hello_release,
};


static int __init hello_dev_init(void) {
    alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);  //申请设备号
    printk(KERN_INFO "%s: major = %d minor=%d\n", DEVICE_NAME, MAJOR(dev_num), MINOR(dev_num));

    cdev_init(&hello_cdev, &hello_fops);
    cdev_add(&hello_cdev, dev_num, 1);  //注册进内核

    return 0;
}

static void __exit hello_dev_exit(void) {
    cdev_del(&hello_cdev);
    unregister_chrdev_region(dev_num, 1);
}

module_init(hello_dev_init);
module_exit(hello_dev_exit);

MODULE_LICENSE("GPL");