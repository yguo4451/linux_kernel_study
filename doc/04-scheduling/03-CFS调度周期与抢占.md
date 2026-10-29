# 23. CFS 调度周期与抢占

> CFS 调度系列第三篇（收尾）。时间片怎么算、任务多久被强制让出、TIF_NEED_RESCHED 抢占标志、用户态/内核态抢占。源码依据 6.6.44。

## 一、时间片：EEVDF 的 base_slice

6.6 用 EEVDF，基础时间片是常量（`fair.c:78`）：

```c
unsigned int sysctl_sched_base_slice = 750000ULL;   // 源码默认 0.75ms
```

> **实测更正**：实际生效的 slice 是 **2.25ms**，不是 0.75ms。因为
> `tunable_scaling = logarithmic`，内核按核数缩放：`0.75ms × (1+log₂4) = 0.75 × 3 = 2.25ms`。
> 实测：`cat /sys/kernel/debug/sched/base_slice_ns` → `2250000`（详见 doc/05）。

`update_deadline`（`fair.c:1025`）给每个任务算**虚拟截止期**：

```c
se->slice = sysctl_sched_base_slice;                        // 真实时间片（缩放后 2.25ms）
se->deadline = se->vruntime + calc_delta_fair(se->slice, se); // 虚拟截止期
```

注释公式 `vd_i = ve_i + r_i / w_i`：

```
deadline = vruntime + 2.25ms × (1024 / weight)
```

权重大的任务虚拟截止期近、真实跑得久；权重小的虚拟截止期远、真实跑得短。

## 二、抢占触发：跑到 deadline

任务运行中 vruntime 增长，`vruntime >= deadline`（消耗完 slice）时，`update_deadline` 触发：

```c
if (cfs_rq->nr_running > 1) {
    resched_curr(rq_of(cfs_rq));   // 触发抢占
}
```

`resched_curr`（`core.c:1040`）只设标志位：

```c
set_tsk_need_resched(curr);   // 设置 TIF_NEED_RESCHED
set_preempt_need_resched();
```

## 三、TIF_NEED_RESCHED：抢占的"请柬"

抢占机制是**标志位 + 安全点**，不是立即切换：

```
任务跑满 slice → resched_curr 设 TIF_NEED_RESCHED（只是"建议换人"）
→ 安全时机检查到标志 → 真正调用 __schedule() 切换
```

`__schedule()` 才做真正的上下文切换（保存寄存器、换栈、切页表）。标志位让"请求抢占"和"执行切换"解耦。

## 四、抢占时机：用户态 vs 内核态

| 场景 | 抢占时机 | 依赖 |
|------|---------|------|
| 用户态代码 | 返回用户态前检查标志 | 永远可以 |
| 内核态代码 | 只有 CONFIG_PREEMPT 开启才能抢占 | 内核配置 |

实验确认本内核：`CONFIG_PREEMPT=y`（完整抢占）、`CONFIG_HZ=250`（调度时钟 250Hz）。

**通往 PREEMPT_RT**：普通 PREEMPT 内核在内核态抢占点有限（自旋锁持有期间不能抢占）；PREEMPT_RT 把几乎所有自旋锁变成可睡眠的 rt_mutex，让内核态几乎处处可抢占，实时性大幅提升——汽车实时 Linux 的核心。

## 五、完整闭环

```
nice → weight → vruntime 增速（doc/22）
        ↓
跑满 slice（2.25ms）→ deadline 到
        ↓
resched_curr 设 TIF_NEED_RESCHED（请柬）
        ↓
安全点检查标志 → __schedule() 切换
        ↓
红黑树选 vruntime 最小（或 deadline 最早）的下一个
```

## 六、相关 sysctl

`/proc/sys/kernel/` 下：`sched_rr_timeslice_ms`、`sched_rt_period_us`、`sched_rt_runtime_us` 等。

> 注意：`sched_base_slice` **不在** `/proc/sys/kernel/` 下，而是在 debugfs：
> `/sys/kernel/debug/sched/base_slice_ns`（6.6 起调度调试信息迁移到 debugfs，详见 doc/05）。
