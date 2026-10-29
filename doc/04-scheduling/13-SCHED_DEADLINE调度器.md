# 13 SCHED_DEADLINE 调度器（EDF + CBS）

## 目标

理解 Linux 第三种实时调度策略 SCHED_DEADLINE：截止时间驱动的 EDF 算法 + CBS 带宽服务器准入控制，以及它如何贴合汽车 AUTOSAR 的"周期+截止时间"任务模型。

## 三种实时调度策略定位

| | SCHED_FIFO/RR | SCHED_DEADLINE |
|---|---|---|
| 模型 | 固定优先级（1-99）| 截止时间驱动 |
| 谁先跑 | 优先级高的 | deadline 近的 |
| 带宽保证 | ❌ 无 | ✅ CBS 准入控制 |
| 贴合场景 | 简单实时任务 | 周期+截止时间任务（AUTOSAR）|

## 三个核心参数

```
runtime  = 每周期最多运行时间（CPU 预算）
deadline = 每周期必须完成的相对截止时间
period   = 任务周期
约束：runtime ≤ deadline ≤ period
```

## EDF 算法：谁 deadline 近谁先跑

DL 任务按绝对 deadline 排序（红黑树），不是固定优先级：

```c
struct sched_dl_entity {
	struct rb_node		rb_node;      // 按 deadline 排序的红黑树
	u64			dl_runtime;   // 每实例最大运行时间
	u64			dl_deadline;  // 相对截止时间
	u64			dl_period;    // 周期
	...
	s64			runtime;      // 当前实例剩余 runtime
	u64			deadline;     // 当前实例绝对 deadline
	struct hrtimer		dl_timer;     // 带宽补充定时器
};
```

## CBS 带宽服务器：准入控制

- 每任务带宽 = runtime / period
- 准入控制：总带宽超限则拒绝新任务
- 预算控制：runtime 用完挂起（dl_throttled），下周期补充

## 调度类优先级层次

```
dl_sched_class   : prio = -1    （最高，SCHED_DEADLINE）
rt_sched_class   : prio = 0~99  （SCHED_FIFO/RR）
fair_sched_class : prio = 100+  （CFS）
```

DL 任务 prio = -1，比任何 FIFO 任务都高。

## 实验（dl_demo）

`experiments/dl_demo/dl_demo.c`：用 sched_setattr 设置 SCHED_DEADLINE，模拟周期任务。

```sh
dl_demo 5000 10000 10000 3000   # runtime=5ms deadline=10ms period=10ms
```

观察 `/proc/PID/sched`：

```
policy : 6    # SCHED_DEADLINE
prio   : -1   # DL 类优先级
```

## 准入控制实验与 EPERM 源码发现

两个 60% 带宽任务不绑 CPU 都成功（分散到不同 CPU，每 CPU 60% < 95%）。

taskset 绑同一 CPU 后第二个任务被拒，但报错是 **EPERM** 而非 EBUSY。读 `sched_dl_overflow`（`kernel/sched/deadline.c:3484`）发现：

```c
int sched_dl_overflow(struct task_struct *p, int policy, const struct sched_attr *attr)
{
    ...
    int cpus, err = -1, cpu = task_cpu(p);   // err 默认 -1 = -EPERM
    ...
    if (不超限) err = 0;
    else if (不超限) err = 0;
    else if (退出 DL) err = 0;
    // 超限时三个分支都不进，err 保持 -1
    return err;   // -1 = -EPERM
}
```

**同一个"带宽超限"，不同内核路径不同 errno**：
- `sched_setattr` 设置新任务 → EPERM（err 默认 -1）
- 任务迁移到满载 CPU → EBUSY（`dl_task_can_attach`）

## 已知问题（源码 XXX 注释）

`dl_bw_capacity` 上方的注释承认：

```c
/*
 * XXX Fix: If 'rq->rd == def_root_domain' perform AC against capacity
 * of the CPU the task is running on rather rd's \Sum CPU capacity.
 */
```

准入控制应按单 CPU 容量算，实际按整个 root domain 容量算——与 isolcpus 交互时可能误判。这是读源码才能发现的已知未修复问题。

## 汽车相关性

AUTOSAR 车载任务天然是"周期+截止时间"（10ms 控制、20ms 感知、100ms 诊断）。SCHED_DEADLINE 直接声明 runtime/deadline/period，内核保证准入控制通过后每个任务都能在 deadline 前完成——这是安全关键系统的根基。
