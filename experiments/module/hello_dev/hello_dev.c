
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/device.h>


#define DEVICE_NAME "hello_dev"


static dev_t dev_num;  //设备号
// static struct cdev hello_cdev;  // 字符设备

#define HELLO_MAGIC 'H'  
#define HELLO_RESET _IO(HELLO_MAGIC, 0)

static struct class* hello_class; 

struct hello_dev {
    struct cdev cdev;
    char buf[128];
    int id;
};


static struct hello_dev hello_devs[2];

static int hello_open(struct inode* inode, struct file* file) {
    struct hello_dev * dev = container_of(inode->i_cdev, struct hello_dev, cdev);
    file->private_data = dev;
    return 0;
}

static ssize_t hello_read(struct file* file, char __user *buf, size_t len, loff_t* off) {
    struct hello_dev* dev = file->private_data;
    int msg_len = strlen(dev->buf);

    if (*off >= msg_len) {
        return 0;
    }
    if (len > msg_len - *off) {
        len = msg_len - *off;
    }

    if (copy_to_user(buf, dev->buf + *off, len)) {
        return -EFAULT;
    }

    *off += len;
    return len;
}

static ssize_t hello_write(struct file* file, const char __user* buf, size_t len, loff_t *off) {

    struct hello_dev* dev = file->private_data;

    if (len >= sizeof(dev->buf)) {
        return -EINVAL;
    }

    if (copy_from_user(dev->buf, buf, len)) {
        return -EFAULT;
    }

    dev->buf[len] = '\0';
    return len;

}

static long hello_ioctl(struct file* file, unsigned int cmd, unsigned long arg) {
    struct hello_dev *dev = file->private_data;
    switch(cmd) {
        case HELLO_RESET:
            snprintf(dev->buf, sizeof(dev->buf), "Hello from device: %d\n", dev->id);
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

    int i = 0, ret = 0;

    ret = alloc_chrdev_region(&dev_num, 0, 2, DEVICE_NAME);  //申请设备号
    if (ret) {
        return ret;
    }
    printk(KERN_INFO "%s: major = %d minor=%d\n", DEVICE_NAME, MAJOR(dev_num), MINOR(dev_num));

    // cdev_init(&hello_cdev, &hello_fops);
    // cdev_add(&hello_cdev, dev_num, 1);  //注册进内核
    hello_class = class_create("hello_dev");

    if (IS_ERR(hello_class)) {
        ret = PTR_ERR(hello_class);
        goto err_unreg_region;
    }

    for (i = 0; i < 2; ++i) {
        hello_devs[i].id = i;
        snprintf(hello_devs[i].buf, sizeof(hello_devs[i].buf), "Hello from device: %d\n", i);

        cdev_init(&hello_devs[i].cdev, &hello_fops);

        ret =  cdev_add(&hello_devs[i].cdev, dev_num + i, 1);
        if (ret) {
            goto err_del_cdev;
        }
        struct device *d = device_create(hello_class, NULL, dev_num + i, NULL,
                                 "hello_dev%d", i);
        if (IS_ERR(d)) {
            ret = PTR_ERR(d);
            cdev_del(&hello_devs[i].cdev);
            goto err_del_cdev;
        }
    }

    return 0;

err_del_cdev:
    while(i--) {
        device_destroy(hello_class, dev_num + i);
        cdev_del(&hello_devs[i].cdev);
    }

    class_destroy(hello_class);

err_unreg_region:
    unregister_chrdev_region(dev_num, 2);
    return ret;
}

static void __exit hello_dev_exit(void) {

    for (int i = 0; i < 2; ++i) {
        device_destroy(hello_class, dev_num+i);
        cdev_del(&hello_devs[i].cdev);
    }

    class_destroy(hello_class);

    unregister_chrdev_region(dev_num, 2);
}

module_init(hello_dev_init);
module_exit(hello_dev_exit);

MODULE_LICENSE("GPL");