# 21. 进程调度：task_struct 与优先级体系

> CFS 调度系列第一篇。涵盖：调度器总览、task_struct 数据结构、内核线程 vs 用户进程、/proc/PID/stat 解析、优先级换算、调度类链。源码依据 6.6.44。

## 一、调度器解决什么问题

单核 CPU 上"同时"跑几十个进程，靠**分时复用**——CPU 在进程间快速切换。调度器回答两个问题：

1. 下一个让谁跑？（`pick_next_task`）
2. 让它跑多久？（时间片 slice）

## 二、task_struct：进程的内核表示（数据优先）

一个进程 = 一个 `task_struct`（`include/linux/sched.h:743`）。调度相关字段：

```c
struct task_struct {
    unsigned int __state;                  // 进程状态
    int prio;                              // 动态优先级（实际生效）
    int static_prio;                       // 静态优先级（nice 换算）
    int normal_prio;                       // 普通优先级（无提升时应有值）
    unsigned int rt_priority;              // 实时优先级 0-99

    struct sched_entity  se;               // CFS 调度实体
    struct sched_rt_entity rt;             // 实时实体
    struct sched_dl_entity dl;             // deadline 实体
    const struct sched_class *sched_class; // 调度类指针（sched.h:797）
};
```

`struct sched_entity`（`sched.h:548`）是 CFS 的排序单位：

```c
struct sched_entity {
    struct load_weight load;   // 负载权重（nice 换算）
    struct rb_node run_node;   // 红黑树节点 ← CFS 核心
    u64 vruntime;              // 虚拟运行时间 ← 排序键
    u64 exec_start;            // 本次开始运行时间
    u64 sum_exec_runtime;      // 累计 CPU 时间
    u64 slice;                 // 时间片
};
```

**CFS 调度单位是 `sched_entity` 而非 `task_struct`**——cgroup 可把一组进程当整体调度。

## 三、内核线程 vs 用户进程

`ps` 里 `[方括号]` 的是内核线程。本质判据是 `PF_KTHREAD` 标志（`sched.h:1748`）：

```c
#define PF_KTHREAD 0x00200000
```

`ps` 读 `/proc/PID/stat` 第 9 字段 `flags`，检测到该位就渲染方括号。**内核线程也有 task_struct，和用户进程在调度器里一视同仁**。

过滤用户态进程（判断本质标志）：

```sh
for p in /proc/[0-9]*; do
  flags=$(awk '{print $9}' "$p/stat" 2>/dev/null)
  [ $((flags & 2097152)) -eq 0 ] && echo "$(basename $p): $(cat $p/comm)"
done
```

## 四、/proc/PID/stat 的 52 字段

由 `do_task_stat()`（`fs/proc/array.c:467`）每次读时现场生成，非磁盘存储。关键字段：

| # | 字段 | 来源 |
|---|------|------|
| 3 | state | `get_task_state(task)` 转字符 |
| 9 | flags | `task->flags`（含 PF_KTHREAD）|
| 14/15 | utime/stime | 用户态/内核态 CPU 时间 |
| 18 | priority | `task_prio(task)` = `prio - 100` |
| 19 | nice | `task_nice(task)` = `static_prio - 120` |
| 40 | rt_priority | `task->rt_priority` |
| 41 | policy | `task->policy`（0=NORMAL,1=FIFO,2=RR）|

`task_prio`（`core.c:7315`）：`return p->prio - MAX_RT_PRIO;`
`task_nice`（`sched.h:1915`）：`return PRIO_TO_NICE(p->static_prio);`

⚠️ comm 字段（第 2 个）是括号包裹、可能含空格，awk 按空格解析不可靠，应 `sed 's/ (.*) / /'` 先剥掉。

## 五、优先级坐标系（`include/linux/sched/prio.h`）

```
内核 prio：0 ─────── 99 ─────────── 139
            │ 实时区  │  普通区       │
            │ 0~99    │  100~139     │
            │ 越小越高 │ = 120 + nice │
            └─────────┴──────────────┘
              MAX_RT_PRIO=100   MAX_PRIO=140

rt_priority（用户视角）：0~99，越大越优先
nice（用户视角）：-20 ~ +19，越小越优先
```

宏定义：`MAX_RT_PRIO=100`、`MAX_PRIO=140`、`DEFAULT_PRIO=120`、`NICE_TO_PRIO(n)=n+120`、`PRIO_TO_NICE(p)=p-120`。

### nice 与 prio 是一体两面

`nice` 和 `prio` 不是两个独立概念，而是**同一个优先级的两种表示**——线性映射：

```
nice:  -20  -10   0   +10   +19
        │    │   │    │     │
        ▼    ▼   ▼    ▼     ▼
prio:  100  110  120  130  139   (内核)
显示:   0   10   20   30   39    (/proc，= nice + 20)
```

- `nice`：POSIX 接口，用户视角的**偏移量**（相对默认 0）
- `prio`：内核**绝对坐标**，和实时进程共用 0~139 数轴

为什么用两个：接口分层（用户用 nice，内核用 prio）+ 统一数轴（0~99 留给实时进程，nice 映射到 100~139）。实验验证：`renice -n 10` 后 `nice=10 prio=30`，改 nice 即改 prio。

实战建议：

| 场景 | 设置 |
|------|------|
| 前台默认 | nice=0，别动 |
| 后台重活（编译/备份/日志）| nice +10~+19 |
| 需要优先 | nice -5~-10（需 root）|
| 实时进程 | 用 `chrt`，nice 无效 |

经验法则：nice 每差 1，CFS 权重约差 25%（相邻权重比约 1.25，详见 doc/22）。实时进程要谨慎——死循环 RT 任务会饿死整个系统。

### 进阶：nice 与 rt_priority 可以同时有值，但调度只认一个

没有 syscall 直接设置 `prio`——用户能调的旋钮只有两个：`nice`（改 `static_prio`）和 `rt_priority`（改实时优先级）。`prio` 是内核派生的。

但 `static_prio` 和 `rt_priority` 是两个独立字段，可同时有值。调度器按 `policy` 只认一个：

- `SCHED_NORMAL` → 认 nice，rt_priority 无效
- `SCHED_FIFO/RR` → 认 rt_priority，**nice 被无视**

实验：`chrt -f 50` + `renice -n 10` → `policy=1 rt_priority=50 nice=10 prio=-51`，nice 改了但 prio 恒为 -51（仍由 rt_priority 决定）。"同时设置"在数据层允许，调度层只有一个生效，不打架不叠加。

## 六、调度策略详解（`include/uapi/linux/sched.h:114`）

| 宏值 | 策略 | 调度类 | 时间片 | 同优先级行为 |
|------|------|--------|--------|-------------|
| 0 | SCHED_NORMAL | fair（CFS）| 动态 | 公平共享（vruntime）|
| 1 | SCHED_FIFO | rt | **无** | 先到先服务 |
| 2 | SCHED_RR | rt | 固定 100ms | 轮转 |
| 3 | SCHED_BATCH | fair | 动态 | 减少唤醒抢占 |
| 5 | SCHED_IDLE | idle | — | 最低，空闲才跑 |
| 6 | SCHED_DEADLINE | dl | 预算制 | EDF 按 deadline |

- **SCHED_NORMAL**：默认，公平瓜分 CPU，nice 调权重。
- **SCHED_FIFO**：无时间片，跑到主动让出或被更高优先级抢占；同优先级先到先服务；死循环会饿死系统。
- **SCHED_RR**：唯一区别是有时间片（默认 100ms，`rt.h:57` `RR_TIMESLICE=100*HZ/1000`），时间片用完排到队尾轮转。
- **SCHED_DEADLINE**：EDF 最早截止期优先 + CBS 带宽控制（`deadline.c:5`），优先级 -1（最高），需 `sched_setattr` 设置 runtime/deadline/period。

### FIFO 死循环为何饿死系统（三要素叠加）

"死循环"指 `while(1)` 这类永不主动让出 CPU 的循环。饿死系统的机制是：

```
while(1) 永不主动让出
  + FIFO 无时间片（task_tick_rt 对 FIFO 直接 return，不强制踢）
  + rt_priority 高于所有普通进程（100~139 远低于 0~99，没人能抢占）
  = 永久霸占 CPU
```

内核线程（kworker、ksoftirqd）几乎都是 SCHED_NORMAL，优先级低于实时进程，所以也被饿死——连处理中断下半部的 ksoftirqd 都抢不到 CPU。

对比：普通进程 `while(1)` 只是"CPU 100%"不卡死（CFS 有 vruntime 强制轮转）；FIFO `while(1)` 才是"系统饿死"（无强制让出）。

### RT throttling 保命机制

**是什么**：限制实时任务在固定周期内最多用多少 CPU，强制留一部分给普通任务，防止饿死。核心结构 `struct rt_bandwidth`（`sched.h:280`）含 `rt_period`（周期）、`rt_runtime`（配额）、`rt_period_timer`（周期定时器）。

**参数**：`sched_rt_period_us=1000000`（1s 周期）、`sched_rt_runtime_us=950000`（每周期 RT 最多 950ms）。特殊值：`runtime=-1`（RUNTIME_INF，禁用）、`runtime>=period`（不节流）、`runtime=0`（RT 完全不跑）。

**触发**：`update_curr_rt`（`rt.c:1060`）累加 `rt_rq->rt_time`，`sched_rt_runtime_exceeded`（`rt.c:1002`）判断 `rt_time > runtime` 就置 `rt_throttled=1` 并 `sched_rt_rq_dequeue` 移出运行队列，同时 `printk_deferred_once("sched: RT throttling activated")`。

**恢复**：period 定时器到期，`do_sched_rt_period_timer`（`rt.c:905`）扣减已过周期时间，`rt_time < runtime` 则解除节流重新入队。

**时序**：RT 任务每 1 秒最多连续跑 950ms，剩下 50ms 强制让给普通进程（init/shell/ksoftirqd 得以存活）。

**实验**：`echo 100000 > /proc/sys/kernel/sched_rt_runtime_us` 缩小配额 → `chrt -f 50 yes` 死循环 → `dmesg` 出现 `sched: RT throttling activated`。设 `runtime=-1` 后死循环才会真正饿死系统。

FIFO/RR 的核心源码区别在 `task_tick_rt`（`rt.c:2647`）：

```c
if (p->policy != SCHED_RR)
    return;                    // FIFO 直接返回，无时间片
if (--p->rt.time_slice)        // RR 递减时间片
    return;
p->rt.time_slice = sched_rr_timeslice;  // 用完重置 100ms
requeue_task_rt(rq, p, 0);              // 排到队尾
resched_curr(rq);                        // 重新调度
```

实验：`chrt -o 0/-f 50/-r 50` 分别得 `policy=0/1/2`；`/proc/sys/kernel/sched_rr_timeslice_ms` = 100。

## 七、三个 prio 与 normal_prio 三分支

`kernel/sched/core.c:2140`：

```c
static inline int __normal_prio(int policy, int rt_prio, int nice)
{
    if (dl_policy(policy))   prio = MAX_DL_PRIO - 1;        // deadline: -1
    else if (rt_policy(policy)) prio = MAX_RT_PRIO - 1 - rt_prio; // RT: 99-rt_prio
    else                     prio = NICE_TO_PRIO(nice);     // 普通: 120+nice
    return prio;
}
```

`effective_prio`（`core.c:2173`）决定实际 `prio`：非实时任务用 `normal_prio`，实时任务保持（可被 RT 继承临时拔高）。

## 八、调度类链：链接脚本排序

`kernel/sched/sched.h:2320`：

```c
#define DEFINE_SCHED_CLASS(name) \
const struct sched_class name##_sched_class \
    __section("__" #name "_sched_class")   // 每个类放独立链接段

#define for_each_class(class) \
    for_class_range(class, __sched_class_highest, __sched_class_lowest)
```

调度类按优先级排成数组（编译期由链接脚本排好）：

```
stop → dl → rt → fair → idle
              ↑ 有任务，fair 就轮不到
```

`pick_next_task`（`core.c:6021`）从最高类遍历，`rt` 有任务则 `fair`（CFS）永远轮不到——这就是"实时压普通"的零开销机制。

## 九、实验记录

```sh
# 普通进程 vs 实时进程（chrt -f 50）
普通：policy=0  rt_priority=0   prio=20    # 内核 120 = 120 + nice0
实时：policy=1  rt_priority=50  prio=-51   # 内核 49  = 99 - 50
```

**prio 显示负数 = 跨进实时区**（内核 prio < 100，即 `task_prio` 减 100 后为负）。普通进程显示 0~39，实时进程显示 0~-99。

## 十、配套脚本

- `scripts/guest_exec.py`：通过宿主机 2323 端口（telnet）执行 guest 命令，与 QEMU 终端分工使用。
