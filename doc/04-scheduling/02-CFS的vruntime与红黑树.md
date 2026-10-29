# 22. CFS 核心：vruntime 与红黑树

> CFS 调度系列第二篇。核心：vruntime（虚拟运行时间）是什么、nice 如何通过权重影响它、红黑树如何排序、6.6 的 EEVDF 进化。源码依据 6.6.44。

## 一、vruntime 是什么

`vruntime`（虚拟运行时间）是 CFS 的**排序键**——不是真实 CPU 时间，而是**加权后**的时间，是"公平"的度量标准。`sched_entity` 里字段 `vruntime`（`sched.h:548`）。

## 二、权重表：nice → weight

`kernel/sched/core.c:11545` 的 40 项权重表，`nice 0` 的权重定为 `1024`（`NICE_0_LOAD`）：

```
nice:  -20   ...   0     1     2    ...  +19
weight: 88761 ... 1024  820   655   ...  15
```

相邻 nice 权重比约 **1.25**（1024/820≈1.249，820/655≈1.252），即每差 1 级权重差约 25%。

## 三、vruntime 的计算（核心公式）

`update_curr`（`fair.c:1156`）每次调度给当前任务记账：

```c
curr->sum_exec_runtime += delta_exec;            // 真实时间累加
curr->vruntime += calc_delta_fair(delta_exec, curr); // vruntime 加权累加
```

`calc_delta_fair`（`fair.c:311`）展开即：

```
vruntime += 真实运行时间 × (1024 / weight)
```

**权重越大，vruntime 增长越慢**：

```
nice 0  (weight=1024) ：跑 10ms → vruntime += 10ms
nice -20(weight=88761)：跑 10ms → vruntime += 0.115ms（几乎不涨）
nice +19(weight=15)   ：跑 10ms → vruntime += 680ms（飞速涨）
```

## 四、红黑树：按 vruntime 排序，最左者先跑

`cfs_rq.tasks_timeline`（`sched.h`）是一棵按 vruntime 排序的红黑树。`__pick_first_entity`（`fair.c:852`）取最左节点：

```c
struct rb_node *left = rb_first_cached(&cfs_rq->tasks_timeline);
```

vruntime 最小者先跑。于是：nice 低的进程 vruntime 涨得慢、总排前面 → 分到更多 CPU；nice 高的涨得快、排后面 → 分到更少。

**"公平"的本质**：不是平均分时间，而是让所有任务的 vruntime 增长速率一致——权重大的任务获得更多真实时间，最终大家都付出相同的 vruntime。

## 五、6.6 的进化：EEVDF

6.6 内核把选人标准从"最小 vruntime"升级为 **EEVDF（Earliest Eligible Virtual Deadline First）**（`fair.c:864` 注释）：

```
1) 任务必须 eligible（欠它的服务未补上）
2) 从中选 virtual deadline 最早的那个
```

每个任务 `deadline = vruntime + slice`（虚拟截止期），红黑树增强为按 deadline 的堆（`se->min_deadline = min(se->deadline, 左右子树 min_deadline)`）。核心 vruntime 概念不变，选人更精细。

## 六、实验记录

内核未开 `CONFIG_SCHED_DEBUG`，`/proc/PID/sched` 不可用，改测 CPU 时间分配：

```sh
yes >/dev/null 2>&1 & P1=$!                 # nice 0
nice -n 19 yes >/dev/null 2>&1 & P2=$!      # nice 19
sleep 3
awk '{print $14+$15}' /proc/$P1/stat        # nice0 的 utime+stime
awk '{print $14+$15}' /proc/$P2/stat        # nice19 的 utime+stime
```

结果：`nice0=98 ticks, nice19=1 ticks`，比例约 98:1，量级吻合理论 `1024:15 ≈ 68:1`——nice 19 的任务几乎饿死。

## 七、观测脚本

`scripts/guest_exec.py` 支持第二个参数指定等待秒数，用于"启动任务 → sleep → 读结果"类实验。
