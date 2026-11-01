

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>


/* PL031 RTC 寄存器偏移 */
#define RTC_DR   0x000   /* 数据寄存器：读当前时间 */
#define RTC_MR   0x004   /* 匹配寄存器：设闹钟时间 */
#define RTC_IMSC 0x010   /* 中断屏蔽：bit0 使能匹配中断 */
#define RTC_RIS  0x014   /* 原始中断状态：bit0 匹配已发生 */
#define RTC_ICR  0x01C   /* 中断清除：写 1 清 */

static void __iomem* rtc_base;
static int irq;


static irqreturn_t rtc_handler(int irq, void* dev_id) {
    u32 ris = readl(rtc_base + RTC_RIS);
    if (ris & 0x1) {
        printk(KERN_INFO "hello_irq: alarm fired! time=%u\n",
               readl(rtc_base + RTC_DR));
        writel(1, rtc_base + RTC_ICR);   // 清中断（电平触发不清会一直触发）
        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static int __init hello_irq_init(void) {

    struct device_node* np = of_find_node_by_path("/pl031@9010000");
    if (!np) {
        return -ENODEV;
    }

    struct resource res;
    if (of_address_to_resource(np, 0, &res)) {
        return -EINVAL;
    }

    irq = of_irq_get(np, 0);
    if (irq < 0) {
        return irq;
    }

    rtc_base = ioremap(res.start, resource_size(&res));
    if (!rtc_base) {
        return -ENOMEM;
    }

    //设置闹钟
    u32 now = readl(rtc_base + RTC_DR);
    writel(now+5, rtc_base + RTC_MR);

    //是能匹配中断
    writel(1, rtc_base + RTC_IMSC);

    int ret = request_irq(irq, rtc_handler, IRQF_SHARED, "hello_irq", &irq);
    if (ret) {
        iounmap(rtc_base);
        return ret;
    }

    printk(KERN_INFO "hello_irq: armed, alarm in 5s (irq=%d)\n", irq);

    return 0;
}


static void __exit hello_irq_exit(void) {
    free_irq(irq, &irq);
    iounmap(rtc_base);
    printk(KERN_INFO "hello_irq: bye\n");
}

module_init(hello_irq_init);
module_exit(hello_irq_exit);
MODULE_LICENSE("GPL");