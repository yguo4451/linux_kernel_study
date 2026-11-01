
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/platform_device.h>

#include <linux/io.h>
#include <linux/slab.h>
#include <linux/mm.h>


#define DEVICE_NAME "hello_iomem"
#define HELLO_MAGIC 'H'  
#define HELLO_RESET _IO(HELLO_MAGIC, 0)

static dev_t dev_num;  //设备号
static struct class* hello_class; 

struct hello_dev {
    struct cdev cdev;
    char buf[128];
    int id;
    int has_data; 
    wait_queue_head_t wq;
};


static struct hello_dev hello_devs[2];
static void __iomem* io_base;

static int hello_open(struct inode* inode, struct file* file) {
    struct hello_dev * dev = container_of(inode->i_cdev, struct hello_dev, cdev);
    file->private_data = dev;
    return 0;
}

static ssize_t hello_read(struct file* file, char __user *buf, size_t len, loff_t* off) {
    struct hello_dev* dev = file->private_data;

    int msg_len = 0;

    if (wait_event_interruptible(dev->wq, dev->has_data)) {
        return -ERESTARTSYS;
    }
    msg_len = strlen(dev->buf);


    if (len > msg_len ) {
        len = msg_len;
    }

    if (copy_to_user(buf, dev->buf, len)) {
        return -EFAULT;
    }

    dev->has_data = 0;
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

    dev->has_data = 1;
    wake_up_interruptible(&dev->wq);
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

static __poll_t hello_poll(struct file* file, struct poll_table_struct * wait) {
    struct hello_dev* dev = file->private_data;

    __poll_t mask = 0;
    poll_wait(file, &dev->wq, wait);

    if (dev->has_data) {
        mask |= EPOLLIN |EPOLLRDNORM;
    }

    return mask;
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
    .poll = hello_poll,
};



static int hello_probe(struct platform_device* pdev) {
    printk(KERN_INFO "hello_plat: probe called, name=%s\n", pdev->name);
   
    int i = 0, ret = 0;

    struct resource* r;


    r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!r) {
        printk(KERN_ERR "hello_iomem: no MEM resource\n");
        return -ENODEV; 
    }
    io_base = ioremap(r->start, resource_size(r));
    if (!io_base) {
        printk(KERN_ERR "hello_iomem: ioremap failed\n");
        return -ENOMEM;
    }
    printk(KERN_INFO "hello_iomem: phys=0x%llx -> virt=%px\n",
       (unsigned long long)r->start, io_base);

    u32 fr = readl(io_base + 0x18); 
    printk(KERN_INFO "hello_iomem: UART FR=0x%x (TXFE=%d RXFE=%d)\n",
        fr, (fr >> 7) & 1, (fr >> 4) & 1);

        
    ret = alloc_chrdev_region(&dev_num, 0, 2, DEVICE_NAME);  //申请设备号
    if (ret) {
        goto err_iounmap;
    }

    // cdev_init(&hello_cdev, &hello_fops);
    // cdev_add(&hello_cdev, dev_num, 1);  //注册进内核
    hello_class = class_create(DEVICE_NAME);

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
                                 DEVICE_NAME"%d", i);
        if (IS_ERR(d)) {
            ret = PTR_ERR(d);
            cdev_del(&hello_devs[i].cdev);
            goto err_del_cdev;
        }

        init_waitqueue_head(&hello_devs[i].wq);
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
err_iounmap:
    iounmap(io_base);
    return ret;
}

static void hello_remove(struct platform_device* pdev) {
    printk(KERN_INFO "hello_plat: remove called\n");
    for (int i = 0; i < 2; ++i) {
        device_destroy(hello_class, dev_num+i);
        cdev_del(&hello_devs[i].cdev);
    }

    class_destroy(hello_class);

    unregister_chrdev_region(dev_num, 2);
    iounmap(io_base);
    printk(KERN_INFO "hello_iomem: iounmap done\n");
}

static struct platform_driver hello_driver = {
    .probe = hello_probe,
    .remove = hello_remove,
    .driver = {
        .name = "hello_iomem",
        .owner = THIS_MODULE,
    },
};


static struct platform_device *pdev;
static struct resource res[1]; 

static int __init hello_plat_init(void) {



    /* ⭐ ② 虚拟地址 → 物理地址，填进 resource */
    res[0].start = 0x09000000;
    res[0].end = 0x09000fff;
    res[0].flags = IORESOURCE_MEM;
    res[0].name  = "uart0";

    // ① 先注册"设备"
    pdev = platform_device_register_simple("hello_iomem", -1, res, 1);
    if (IS_ERR(pdev)) {
        return PTR_ERR(pdev);
    }


    // ② 再注册"驱动" —— 名字匹配上 → 内核立刻自动调 probe
    return platform_driver_register(&hello_driver);
}

static void __exit hello_plat_exit(void) {
    platform_driver_unregister(&hello_driver);
    platform_device_unregister(pdev);

}

module_init(hello_plat_init);
module_exit(hello_plat_exit);

MODULE_LICENSE("GPL");