# 36. 中断上下文与 workqueue：顶半部/底半部的落地

> 阶段五第十六课。用 `hrtimer + workqueue` 组合把"顶半部/底半部"模型落地：中断回调只调度，耗时/可睡眠的工作交给 workqueue。实验中意外撞出 **PREEMPT_RT 下 `in_atomic()` 误判**的经典坑——同一个宏在实时内核和普通内核下行为不同。

## 一、顶半部 / 底半部模型

```
中断/定时器（顶半部）  →  只做最紧急的（几微秒），不能睡眠
        ↓ schedule_work
workqueue（底半部）    →  做耗时/可睡眠的工作（进程上下文）
```

原因：中断回调运行在**原子上下文**，不能睡眠；耗时的活必须挪到**进程上下文**（workqueue 线程）执行。

## 二、实验：hrtimer + workqueue

```c
static struct hrtimer my_hrtimer;
static struct work_struct my_work;

/* ② workqueue 工作函数：进程上下文，可以睡眠 */
static void my_work_func(struct work_struct *work)
{
    printk("... in_task=%d in_serving_softirq=%d in_atomic=%d\n", ...);
    msleep(50);                /* 进程上下文，睡眠合法 */
}

/* ① hrtimer 回调：软中断上下文，只调度 */
static enum hrtimer_restart hrtimer_callback(struct hrtimer *t)
{
    printk("... in_task=%d in_serving_softirq=%d in_atomic=%d\n", ...);
    schedule_work(&my_work);   /* 只扔给 workqueue，立即返回 */
    return HRTIMER_NORESTART;
}

/* init */
INIT_WORK(&my_work, my_work_func);
hrtimer_init(&my_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
my_hrtimer.function = hrtimer_callback;
hrtimer_start(&my_hrtimer, ms_to_ktime(1000), HRTIMER_MODE_REL);

/* exit */
hrtimer_cancel(&my_hrtimer);
cancel_work_sync(&my_work);    /* 等 work 跑完，防止卸载时还在用 */
```

## 三、实测输出：上下文身份对比

```
hello_wq: [hrtimer] in_task=0 in_serving_softirq=256 in_atomic=0   ← 软中断上下文
hello_wq: [work]    in_task=1 in_serving_softirq=0   in_atomic=0   ← 进程上下文
hello_wq: [work] msleep(50) done                                    ← 进程上下文睡眠成功
```

| 上下文 | `in_task()` | `in_serving_softirq()` | `in_atomic()`（RT 下不可靠） |
|---|---|---|---|
| hrtimer 回调 | `0`（不是进程） | `256`（是软中断） | `0` ❌ **误判** |
| workqueue | `1`（是进程） | `0`（不是软中断） | `0` ✅ 碰巧对 |

**结论**：

- 判断"是否进程上下文" → `in_task()`
- 判断"是否软中断" → `in_serving_softirq()`
- **`in_atomic()` 在 PREEMPT_RT 下会误判**（软中断里返回 0）

## 四、PREEMPT_RT 下的 `in_atomic()` 陷阱（本课精华）

源码 `include/linux/preempt.h:110`：

```c
#ifdef CONFIG_PREEMPT_RT
# define softirq_count()	(current->softirq_disable_cnt & SOFTIRQ_MASK)   // RT：从 softirq_disable_cnt 读
# define irq_count()		((preempt_count() & (NMI_MASK | HARDIRQ_MASK)) | softirq_count())
#else
# define softirq_count()	(preempt_count() & SOFTIRQ_MASK)               // 普通：从 preempt_count 读
# define irq_count()		(preempt_count() & (NMI_MASK | HARDIRQ_MASK | SOFTIRQ_MASK))
#endif

#define in_atomic()	(preempt_count() != 0)                                 // 只看 preempt_count
```

**根因**：

```
普通内核：软中断禁用计数 → preempt_count        → in_atomic() 能看到
PREEMPT_RT：软中断禁用计数 → current->softirq_disable_cnt → in_atomic() 看不到
```

PREEMPT_RT 为了"软中断也能被抢占"，把软中断禁用计数从 `preempt_count` 移到了独立字段 `softirq_disable_cnt`。而 `in_atomic()` 仍只看 `preempt_count`，所以**在 RT 上它会漏掉软中断上下文**。

源码注释也明确警告：

```c
 * WARNING: this macro cannot always detect atomic context; ...
 * Do not use in_atomic() in driver code.
```

## 五、宏的演进：旧宏已 deprecated

```c
/* 已弃用 */
#define in_softirq()		(softirq_count())
#define in_interrupt()		(irq_count())

/* 推荐 */
#define in_nmi()		(nmi_count())
#define in_hardirq()		(hardirq_count())
#define in_serving_softirq()	(softirq_count() & SOFTIRQ_OFFSET)   // RT 下正确
#define in_task()		(!(in_nmi() | in_hardirq() | in_serving_softirq()))  // RT 下正确
```

驱动代码判断上下文，用 `in_task()` / `in_serving_softirq()` / `in_hardirq()`。

## 六、要点回顾

```
顶半部/底半部  ──hrtimer 回调 + schedule_work──>  软中断快返回 + 进程上下文干活
软中断上下文   ──in_serving_softirq()=256──>  不能睡眠
进程上下文     ──in_task()=1──>  可以睡眠（msleep 成功）
in_atomic()   ──PREEMPT_RT 下误判──>  驱动里不要用
宏的选择       ──in_task/in_serving_softirq──>  取代 in_softirq/in_interrupt
```
