/*
 * hello_irq_free.c — request_irq 注册一条空闲中断线（IRQ 12）
 *
 * 验证 request_irq / free_irq 的完整流程：
 *   insmod 后 /proc/interrupts 里 IRQ 12 出现 "hello_irq_free"
 *   rmmod 后消失
 *
 * 注意：这条中断线没有硬件触发，所以计数始终为 0。
 *       本实验验证的是"注册/注销流程"，不是"中断响应"。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>

#define TEST_IRQ 12

static irqreturn_t my_handler(int irq, void *dev_id)
{
    printk(KERN_INFO "hello_irq_free: handler called! irq=%d\n", irq);
    return IRQ_HANDLED;
}

static int __init hello_irq_free_init(void)
{
    int ret = request_irq(TEST_IRQ, my_handler, IRQF_SHARED,
                          "hello_irq_free", &my_handler);
    if (ret) {
        printk(KERN_ERR "hello_irq_free: request_irq(%d) failed: %d\n",
               TEST_IRQ, ret);
        return ret;
    }
    printk(KERN_INFO "hello_irq_free: request_irq(%d) OK\n", TEST_IRQ);
    return 0;
}

static void __exit hello_irq_free_exit(void)
{
    free_irq(TEST_IRQ, &my_handler);
    printk(KERN_INFO "hello_irq_free: free_irq(%d) done\n", TEST_IRQ);
}

module_init(hello_irq_free_init);
module_exit(hello_irq_free_exit);
MODULE_LICENSE("GPL");
