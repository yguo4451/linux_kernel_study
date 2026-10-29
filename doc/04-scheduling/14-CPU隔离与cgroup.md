# CPU 隔离与 cgroup：带宽控制器（cpu.max）原理与实测

> 调度子系列第 14 篇。前一篇 [13-SCHED_DEADLINE](./13-SCHED_DEADLINE调度器.md) 讲的是"给任务一个绝对 deadline"，本篇讲"给一组任务一个 CPU 预算上限"——这是汽车 ECU 多负载分区隔离的核心工具。

## 一、实验现象（先看结果）

4 核系统（QEMU `-smp 4`），跑一个 `while true; do :; done` 死循环，PID 135：

| 阶段 | 操作 | PID 135 的 %CPU | 系统 usr |
|------|------|----------------|----------|
| 未限流 | —— | **24.6%** | 24.7% |
| 限流 50% | `echo "50000 100000" > cpu.max` + 移入组 | **10.1%** | 12.2% |

**24.6% → 10.1%，正好砍半。**

一个关键刻度提醒（这是本次实验最大的"坑"）：busybox `top` 的 `%CPU` 是**占整个系统 CPU 的百分比**，不是占单核。4 核机器上：

```
单核吃满 = 100% / 4 = 25%
cpu.max 限 50%（单核） = 25% × 50% = 12.5%
```

所以死循环未限流时显示 ~25% 而不是 100%，限流后显示 ~12% 而不是 50%。

## 二、cgroup 是什么

**cgroup（control group）** 是内核的"进程分组资源配额"机制：把一组进程放进一个组，对这个组统一设置 CPU、内存、I/O 等资源上限。Linux 现有两代：

| | cgroup v1 | cgroup v2 |
|--|-----------|-----------|
| 挂载点 | 每控制器一个（`/sys/fs/cgroup/cpu`、`/memory`…） | 统一 `/sys/fs/cgroup` |
| 层次结构 | 每个子系统一棵树，互相独立 | 单一树，控制器统一管理 |
| 文件接口 | `cpu.cfs_quota_us` / `cpu.cfs_period_us` | **`cpu.max`**（一行两个值） |
| 现状 | 遗留，逐步废弃 | 主流，本项目用 v2 |

本项目内核只开了 v2（`CONFIG_CPUSETS_V1` 未设置），挂在 `/sys/fs/cgroup`。

## 三、cpu.max 文件格式

```sh
cat /sys/fs/cgroup/test/cpu.max
# max 100000        ← 默认：不限制

echo "50000 100000" > /sys/fs/cgroup/test/cpu.max
# 含义：每 100000μs（100ms）周期内，本组最多用 50000μs（50ms）CPU 时间
```

```
cpu.max = "$QUOTA $PERIOD"
          QUOTA=50000   → 每个 PERIOD 周期内最多 50000μs
          QUOTA=max     → 不限制（RUNTIME_INF）
```

50ms / 100ms = **单核的 50%**。注意这是"总配额"，如果组里有 2 个进程抢，就平分这 50%（各 ~25% 单核，即 top 里的 ~6%）。

## 四、核心数据结构

带宽控制涉及三组对象，都在 `kernel/sched/sched.h`：

```
task_group（任务组，每个 cgroup 对应一个）
 ├─ cfs_bandwidth cfs_bandwidth   ← 组的"预算账本"
 └─ struct cfs_rq *cfs_rq[NR_CPUS] ← 每个 CPU 一个 CFS 运行队列

struct cfs_bandwidth（预算池，组级全局，一份）
 ├─ ktime_t  period           ← 周期（100ms）
 ├─ u64      quota            ← 每周期配额（50ms，或 RUNTIME_INF）
 ├─ s64      runtime          ← 当前池里还剩多少
 ├─ u64      burst            ← 突发额度（cpu.max.burst）
 ├─ struct hrtimer period_timer ← 周期定时器，到期补给
 └─ struct list_head throttled_cfs_rq ← 被节流的运行队列链表

struct cfs_rq（每个 CPU 的运行队列，组在每核上都有一份）
 ├─ int  runtime_enabled    ← 是否启用带宽限制（quota != INF）
 ├─ s64  runtime_remaining  ← 本队列这一小段还剩多少
 └─ int  throttled          ← 是否已被节流（摘除出队）
```

**设计意图**：配额 `quota` 放在**组级** `cfs_bandwidth` 里（全局池），运行队列 `cfs_rq` 从池里"借"一小段（`sched_cfs_bandwidth_slice()`，默认 5ms）到 `runtime_remaining`，用完了再借。这样避免每 tick 都去抢全局锁。

## 五、写入路径：echo 一行文件发生了什么

```
echo "50000 100000" > cpu.max
  │  kernfs 写回调
  ▼
cpu_max_write()                          kernel/sched/core.c:9955
  → cpu_period_quota_parse(buf,&period,&quota)   // 解析 "50000 100000"
  → tg_set_cfs_bandwidth(tg, period, quota, burst)   core.c:9359
      ├─ cfs_b->period = ns_to_ktime(period)     // 100ms
      ├─ cfs_b->quota  = quota                    // 50ms
      ├─ __refill_cfs_bandwidth_runtime(cfs_b)    // fair.c:5803 首次充值
      ├─ start_cfs_bandwidth(cfs_b)               // 启动 100ms 周期定时器
      └─ for_each_online_cpu(i)                   // 逐核初始化
            cfs_rq->runtime_enabled = 1
            cfs_rq->runtime_remaining = 0
```

`tg_set_cfs_bandwidth` 里还有几道**防御校验**（core.c:9370 起）：

```c
if (tg == &root_task_group) return -EINVAL;   // 根组不许限制
if (quota < min_cfs_quota_period || period < min_cfs_quota_period) return -EINVAL; // 下限 1ms
if (period > max_cfs_quota_period) return -EINVAL; // 上限 1s
if (quota != RUNTIME_INF && quota > max_cfs_runtime) return -EINVAL;
ret = __cfs_schedulable(tg, period, quota);   // 分层带宽可行性检查
```

## 六、节流路径：配额怎么被"花掉"

**扣钱**发生在每次时钟 tick 更新当前实体时：

```
update_curr(cfs_rq)                          // scheduler_tick → task_tick_fair
  → account_cfs_rq_runtime(cfs_rq, delta_exec)   fair.c:5891
    → __account_cfs_rq_runtime(cfs_rq, delta_exec)  fair.c:5867
        cfs_rq->runtime_remaining -= delta_exec     // 先扣本队列余额
        if (runtime_remaining <= 0) {
            assign_cfs_rq_runtime(cfs_rq)           // 向组池再借一段
              → __assign_cfs_rq_runtime             fair.c:5827
                  cfs_b->runtime -= amount          // 组池扣钱
                  扣不到（池已空）→ return 0
            if (!assign(...)) resched_curr(rq)      // 没配额了，发起重新调度
        }
```

**执行节流**发生在 `put_prev_entity`（调度器要把当前实体换下来时）：

```
check_cfs_rq_runtime(cfs_rq)                 fair.c:6529
  if (runtime_enabled && runtime_remaining <= 0)
    → throttle_cfs_rq(cfs_rq)                fair.c:5970
        ├─ __assign_cfs_rq_runtime(...,1)    // 最后再确认一次（防竞争）
        ├─ dequeue_entity(qcfs_rq, se, ...)  // 把组的调度实体摘出红黑树
        ├─ cfs_rq->throttled = 1
        └─ sub_nr_running(rq, queued_delta)  // 运行数减一 → 别的任务上 CPU
```

**关键点**：被节流不是"降频"，而是**彻底摘出运行队列**——这个组的任务在这段周期内完全不参与调度，把 CPU 让给别的组。所以你在 `top` 里看到 %CPU 是"一段一段"地下降，而不是平滑减半。

## 七、补给路径：下个周期"复活"

```
sched_cfs_period_timer(hrtimer)   // 每 100ms 到期一次   fair.c:6580
  → do_sched_cfs_period_timer(cfs_b)                     fair.c:6341
      ├─ __refill_cfs_bandwidth_runtime(cfs_b)           // runtime += quota（回到 50ms）
      │      cfs_b->runtime = min(runtime, quota + burst)
      └─ distribute_cfs_runtime(cfs_b)                   // 分给被节流的队列
            → unthrottle_cfs_rq(cfs_rq)                  fair.c:6074
                → enqueue_entity(...)                    // 重新入队红黑树
```

于是死循环呈现**"跑 50ms → 停 50ms"**的周期节律。周期越小（如 period 设 20ms），节流越平滑但调度开销越大；周期越大，突刺越明显。默认 100ms 是平衡点。

## 八、本次踩的两个坑（为什么之前 cpu.max 不存在）

### 坑 1：`cat cpu.max` 报 "No such file"

`cpu.max` 这个文件**只有在内核编入 `CONFIG_CFS_BANDWIDTH` 时才注册**。看 `cpu_files[]` 表（core.c:9997）：

```c
static struct cftype cpu_files[] = {
    ...
#ifdef CONFIG_CFS_BANDWIDTH
    { .name = "max", .flags = CFTYPE_NOT_ON_ROOT,
      .seq_show = cpu_max_show, .write = cpu_max_write },
    { .name = "max.burst", ... },
#endif
```

`#ifdef CONFIG_CFS_BANDWIDTH` 一关，`max` 文件整个消失。解决：`scripts/config -e CFS_BANDWIDTH` 重编内核。这印证了一个老教训——**改了 `.config` 一定要 `grep .config` 确认生效**，因为 `olddefconfig` 会静默回退不满足依赖的选项。

### 坑 2：写 cpu.max 报 "Permission denied"

cgroup v2 的控制器**不会自动启用**，父级必须先向 `cgroup.subtree_control` 声明要下发哪些控制器：

```sh
echo "+cpu" > /sys/fs/cgroup/cgroup.subtree_control
```

本项目已把它固化进 `/init`（启动时自动执行 `echo "+cpu +cpuset +memory +pids"`），所以重启后子组直接就有 `cpu.max`。

## 九、汽车实时 Linux 相关性

cgroup 带宽控制是 **ECU 多负载分区**的基石：

```
一块 SoC 上跑多个 ECU 应用（域控/舱驾融合）
  /sys/fs/cgroup/adas    → cpu.max 配额 30%  自动驾驶感知
  /sys/fs/cgroup/infotain → cpu.max 配额 20%  信息娱乐
  /sys/fs/cgroup/rt_ctrl  → 不限额（用 RT 调度类 + CPU 隔离）
```

- **隔离性**：一个负载死循环也抢不走别的负载的 CPU 份额（本文实验正是这个场景）
- **可动态调**：`echo` 一行就能运行时改配额，不用重启进程（对应 OTA 场景在线调优）
- **与 CPU 隔离四件套配合**（doc/09）：`isolcpus` 是"物理隔离"（整核独享），`cpu.max` 是"预算隔离"（共享核上按份额切），两者是互补的两层

> 注意：`cpu.max` 只约束 **CFS（SCHED_NORMAL）** 任务。`SCHED_FIFO`/`SCHED_RR`/`SCHED_DEADLINE` 的实时任务不走这条带宽路径（它们有各自的 `sched_rt_runtime_us` 节流和 CBS 准入），所以实时负载要用 doc/09 的 isolcpus + doc/13 的 deadline 来管。

## 十、动手复现清单

```sh
# 0. 确认控制器就绪
cat /sys/fs/cgroup/cgroup.controllers     # 含 cpu
mkdir /sys/fs/cgroup/test
cat /sys/fs/cgroup/test/cpu.max           # max 100000

# 1. 死循环 + 抓 PID（$! 紧跟在 & 后）
while true; do :; done &
BUSY=$!
echo "BUSY=$BUSY"

# 2. 未限流：top 里看 ~25%
top

# 3. 限流 50%：top 里掉到 ~12%
echo "50000 100000" > /sys/fs/cgroup/test/cpu.max
echo $BUSY > /sys/fs/cgroup/test/cgroup.procs
top

# 4. 实时解除限制，看跳变
echo "max 100000" > /sys/fs/cgroup/test/cpu.max   # 跳回 ~25%
echo "50000 100000" > /sys/fs/cgroup/test/cpu.max # 再掉回 ~12%
```

## 关键源码索引

| 功能 | 位置 |
|------|------|
| `cpu_max_write` 写入口 | `kernel/sched/core.c:9955` |
| `tg_set_cfs_bandwidth` 设置配额 | `kernel/sched/core.c:9359` |
| `cftype cpu_files[]` 注册 max 文件 | `kernel/sched/core.c:9997`（`#ifdef CONFIG_CFS_BANDWIDTH`）|
| `__refill_cfs_bandwidth_runtime` 补给 | `kernel/sched/fair.c:5803` |
| `__assign_cfs_rq_runtime` 借配额 | `kernel/sched/fair.c:5827` |
| `__account_cfs_rq_runtime` 扣配额 | `kernel/sched/fair.c:5867` |
| `throttle_cfs_rq` 执行节流 | `kernel/sched/fair.c:5970` |
| `unthrottle_cfs_rq` 解除节流 | `kernel/sched/fair.c:6074` |
| `do_sched_cfs_period_timer` 周期补给 | `kernel/sched/fair.c:6341` |
| `check_cfs_rq_runtime` 节流检查 | `kernel/sched/fair.c:6529` |
| `sched_cfs_period_timer` 定时器回调 | `kernel/sched/fair.c:6580` |
| `struct cfs_bandwidth` 定义 | `kernel/sched/sched.h` |
