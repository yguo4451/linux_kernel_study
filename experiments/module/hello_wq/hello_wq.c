/*
 * hello_wq.c — hrtimer + workqueue 组合实验
 *
 * 演示"顶半部/底半部"模型：
 *   hrtimer 回调（软中断上下文，in_atomic=1）只调度 workqueue → 立即返回
 *   workqueue 工作函数（进程上下文，in_atomic=0）做耗时/可睡眠的活
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/delay.h>       /* msleep */

static struct hrtimer my_hrtimer;
static struct work_struct my_work;

/* ② workqueue 工作函数：进程上下文，可以睡眠 */
static void my_work_func(struct work_struct *work)
{
    printk(KERN_INFO "hello_wq: [work] in_task=%d in_serving_softirq=%d in_atomic=%d\n",
           (int)in_task(), (int)in_serving_softirq(), (int)in_atomic());

    msleep(50);                /* 进程上下文，睡眠合法 */

    printk(KERN_INFO "hello_wq: [work] msleep(50) done\n");
}

/* ① hrtimer 回调：软中断上下文，只调度，不干活 */
static enum hrtimer_restart hrtimer_callback(struct hrtimer *t)
{
    printk(KERN_INFO "hello_wq: [hrtimer] in_task=%d in_serving_softirq=%d in_atomic=%d\n",
           (int)in_task(), (int)in_serving_softirq(), (int)in_atomic());

    schedule_work(&my_work);   /* 只扔给 workqueue，立即返回 */
    return HRTIMER_NORESTART;
}

static int __init hello_wq_init(void)
{
    INIT_WORK(&my_work, my_work_func);
    hrtimer_init(&my_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    my_hrtimer.function = hrtimer_callback;
    hrtimer_start(&my_hrtimer, ms_to_ktime(1000), HRTIMER_MODE_REL);
    printk(KERN_INFO "hello_wq: armed, fire in 1s\n");
    return 0;
}

static void __exit hello_wq_exit(void)
{
    hrtimer_cancel(&my_hrtimer);
    cancel_work_sync(&my_work);   /* 等待工作完成，防止卸载时还在跑 */
    printk(KERN_INFO "hello_wq: bye\n");
}

module_init(hello_wq_init);
module_exit(hello_wq_exit);
MODULE_LICENSE("GPL");
