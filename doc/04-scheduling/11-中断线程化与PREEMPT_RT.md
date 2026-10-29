# 11 中断线程化与 PREEMPT_RT

## 目标

理解中断处理的两段式（上半部/下半部），以及 PREEMPT_RT 如何通过"强制线程化"把中断从"关中断的硬中断上下文"变成"可调度的 SCHED_FIFO 线程"，从而降低中断对实时任务的延迟影响。

## 为什么中断要分两段

硬中断期间 CPU 关中断（或关本地中断），handler 必须快。但有些中断处理天然慢（网卡搬数据、I2C 读寄存器），于是拆成两段：

```
设备发中断
   ↓
【上半部 hardirq】关中断，必须快：确认来源 + 屏蔽本线 + 唤醒下半部
   ↓
【下半部 bottom half】可以慢：softirq / tasklet / workqueue / threaded IRQ
```

## 下半部的四种机制

| 机制 | 运行上下文 | 能否睡眠 | 特点 |
|------|-----------|---------|------|
| softirq | 软中断上下文 | ❌ | 最快，TIMER_SOFTIRQ / NET_RX_SOFTIRQ |
| tasklet | 软中断上下文 | ❌ | 基于 softirq，串行化 |
| workqueue | 内核线程 | ✅ | 通用但慢 |
| threaded IRQ | 每 IRQ 独立线程 | ✅ | 可调度、可设优先级 |

## PREEMPT_RT 的杀手锏：强制线程化

`force_irqthreads()` 在 `CONFIG_PREEMPT_RT` 下恒返回 true，所有非 per-CPU 中断强制线程化。

### 源码证据（kernel/irq/manage.c）

**1. 中断线程是 FIFO 实时线程**（`irq_thread`，约 1300 行）：

```c
static int irq_thread(void *data)
{
    ...
    irq_thread_set_ready(desc, action);
    sched_set_fifo(current);      // ← 设成 SCHED_FIFO，默认 rt_priority=50
    ...
    while (!irq_wait_for_interrupt(desc, action)) {
        action_ret = handler_fn(desc, action);
        ...
    }
}
```

**2. RT 下线程里不关中断**（`irq_forced_thread_fn`，约 1191 行）：

```c
local_bh_disable();
if (!IS_ENABLED(CONFIG_PREEMPT_RT))   // ← 非 RT 才关本地中断
    local_irq_disable();
ret = action->thread_fn(action->irq, action->dev_id);
...
if (!IS_ENABLED(CONFIG_PREEMPT_RT))
    local_irq_enable();
local_bh_enable();
```

**3. 线程命名**（`setup_irq_thread`）：`kthread_create(irq_thread, new, "irq/%d-%s", ...)` → `[irq/13-uart-pl0]`。

**4. 哪些中断不线程化**（约 1372 行）：

```c
if (new->flags & (IRQF_NO_THREAD | IRQF_PERCPU | IRQF_ONESHOT))
    // 跳过线程创建
```

per-CPU 中断（arch_timer、arm-pmu）因高频 + 本 CPU 绑定，保持硬中断。

## 实验验证（QEMU 实测）

### ps 看到三类线程

```
[irq/13-uart-pl0]  [irq/16-virtio0-] ...    ← 中断线程
[ksoftirqd/N]                                ← 软中断守护线程
[rcuc/N] [rcub/N]                            ← RCU 线程
```

### /proc/interrupts 对照

| IRQ | 设备 | 计数 | 有线程 |
|-----|------|------|--------|
| 11 | arch_timer | 123782 | ❌ 硬中断 |
| 13 | uart-pl011 | 73 | ✅ 线程化 |
| 17/18 | virtio | 2 | ✅ 线程化 |

### 中断线程调度策略（/proc/92/sched）

```
policy : 1   → SCHED_FIFO
prio   : 49  → 内核优先级
```

## 优先级三层换算

```
用户态 sched_priority = 50   （sched_set_fifo 默认 MAX_RT_PRIO/2）
        ↓
内核 rt_priority = 50
        ↓
task->prio = 99 - 50 = 49    （/proc/PID/sched 的 prio 字段）
        ↓
/proc/PID/stat 第18字段 = 49 - 100 = -51
```

与 doc/04-01 的公式 `/proc/PID/stat 第18字段 = task->prio - 100` 完全自洽。

## 核心结论

| | 普通内核 | PREEMPT_RT |
|---|---------|-----------|
| 中断 handler 环境 | 硬中断（关中断）| SCHED_FIFO 线程 |
| 高优先级任务能否打断中断 | ❌ | ✅ |
| 中断延迟本质 | 不可控的硬中断时长 | 可调度的优先级问题 |

PREEMPT_RT 把中断延迟从"关中断的不可控时长"变成"可计算、可调度的线程优先级问题"。
