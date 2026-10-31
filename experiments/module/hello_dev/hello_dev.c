
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/device.h>


#define DEVICE_NAME "hello_dev"


static dev_t dev_num;  //设备号
static struct cdev hello_cdev;  // 字符设备

#define HELLO_MAGIC 'H'  
#define HELLO_RESET _IO(HELLO_MAGIC, 0)

static char kernel_buf[128] = "Hello from kernel!\n";  // 内核里的缓冲区

static struct class* hello_class; 

static int hello_open(struct inode* inode, struct file* file) {
    return 0;
}

static ssize_t hello_read(struct file* file, char __user *buf, size_t len, loff_t* off) {
    int msg_len = strlen(kernel_buf);

    if (*off >= msg_len) {
        return 0;
    }
    if (len > msg_len - *off) {
        len = msg_len - *off;
    }

    if (copy_to_user(buf, kernel_buf + *off, len)) {
        return -EFAULT;
    }

    *off += len;
    return len;
}

static ssize_t hello_write(struct file* file, const char __user* buf, size_t len, loff_t *off) {
    if (len >= sizeof(kernel_buf)) {
        return -EINVAL;
    }

    if (copy_from_user(kernel_buf, buf, len)) {
        return -EFAULT;
    }

    kernel_buf[len] = '\0';
    return len;

}

static long hello_ioctl(struct file* file, unsigned int cmd, unsigned long arg) {
    switch(cmd) {
        case HELLO_RESET:
            strncpy(kernel_buf, "Hello from kernel!\n", sizeof(kernel_buf) - 1);
            return 0;
        default:
            return -ENOTTY;
    }
}

static int hello_release(struct inode* inode, struct file* file) {
    return 0;
}

static struct file_operations hello_fops = {
    .owner = THIS_MODULE,
    .open = hello_open,
    .read = hello_read,
    .write = hello_write,
    .release = hello_release,
    .unlocked_ioctl = hello_ioctl,
};


static int __init hello_dev_init(void) {
    alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);  //申请设备号
    printk(KERN_INFO "%s: major = %d minor=%d\n", DEVICE_NAME, MAJOR(dev_num), MINOR(dev_num));

    cdev_init(&hello_cdev, &hello_fops);
    cdev_add(&hello_cdev, dev_num, 1);  //注册进内核

    hello_class = class_create("hello_dev");
    device_create(hello_class, NULL, dev_num, NULL, "hello_dev");


    return 0;
}

static void __exit hello_dev_exit(void) {
    device_destroy(hello_class, dev_num);
    class_destroy(hello_class);
    
    cdev_del(&hello_cdev);
    unregister_chrdev_region(dev_num, 1);


}

module_init(hello_dev_init);
module_exit(hello_dev_exit);

MODULE_LICENSE("GPL");