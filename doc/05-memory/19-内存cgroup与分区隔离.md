# 内存 cgroup（memcg）：分区内存隔离

> 内存管理子系列第 19 篇（汽车内存方向收尾篇）。前面 18 篇讲的是"全局内存管理"，本篇讲**分区内存管理**：用 cgroup 给一组进程设内存限额，实现故障隔离——这是汽车 ECU 分区、容器、云多租户的共同基石。

## 一、概念：给进程组设内存限额

cgroup 的 memory 控制器（memcg）把一组进程圈起来，设内存上限。超限 → 组内回收 → 还超 → **cgroup OOM 杀掉组内进程**——但不影响其他组。

## 二、为什么（汽车 ECU 分区的核心）

```
ECU 分区 A（自动驾驶）: memory.max = 2GB
ECU 分区 B（娱乐系统）: memory.max = 512MB
ECU 分区 C（仪表盘）  : memory.max = 256MB
```

**故障隔离**：分区 B 内存爆了，只 OOM 杀 B 内的进程，A/C 毫发无损。没有 cgroup，B 会触发全局 OOM，随机杀掉 A/C 的关键进程——车上可能致命。

## 三、cgroup v2 的核心文件

| 文件 | 作用 |
|------|------|
| `memory.max` | 硬限额（超了回收+OOM）|
| `memory.high` | 软限额（节流不杀）|
| `memory.low` | 保护（内存紧张时优先保它）|
| `memory.current` | 当前使用量 |
| `memory.events` | 事件计数（oom/max/high/low）|
| `memory.swap.max` | swap 限额 |

## 四、机制（源码调用链）

```
do_page_fault（缺页，doc 04）
  → __mem_cgroup_charge（memcg 记账：这页算哪个组的？）
    → try_charge_memcg（检查是否超限）
      → 超限！→ mem_cgroup_out_of_memory（cgroup OOM 入口）
        → out_of_memory
          → oom_kill_process（杀组内最"坏"的进程）
```

## 五、实测：64MB 限额逼出 cgroup OOM

```sh
mkdir /sys/fs/cgroup/test
echo 64M > /sys/fs/cgroup/test/memory.max
echo 0   > /sys/fs/cgroup/test/memory.swap.max   # 禁 swap，逼 OOM
echo $$  > /sys/fs/cgroup/test/cgroup.procs
cat /proc/self/cgroup        # 0::/test（已在 test 组）

mem_hog 100 &                # 分配 100MB，超 64MB 限额
# [1]+ Killed  mem_hog 100
```

### 内核日志的关键证据

```
mem_hog invoked oom-killer: gfp_mask=0xcc0(GFP_KERNEL), order=0
  ... 调用栈 ...
  do_page_fault → handle_mm_fault → __handle_mm_fault
    → __mem_cgroup_charge → try_charge_memcg
      → mem_cgroup_out_of_memory → out_of_memory → oom_kill_process

memory: usage 65536kB, limit 65536kB, failcnt 96   ← 正好卡在 64MB 上限
swap:   usage 0kB,     limit 0kB,     failcnt 0    ← swap 被禁

oom-kill:constraint=CONSTRAINT_MEMCG,
         oom_memcg=/test, task_memcg=/test,
         task=mem_hog, pid=149, uid=0
```

### 三个铁证

1. **`constraint=CONSTRAINT_MEMCG`**：OOM 日志里最关键的字段——它明确说"这次 OOM 是 **cgroup 限额** 触发的，不是全局内存耗尽"。全局 OOM 会是 `constraint=CONSTRAINT_NONE`。

2. **调用栈里 `mem_cgroup_out_of_memory`**：这是 memcg 专属的 OOM 入口（`mm/memcontrol.c`），证明走的是 cgroup OOM 路径，而非全局 `out_of_memory` 直接入口。

3. **`memory: usage 65536kB, limit 65536kB`**：使用量精确卡在 64MB（65536kB）上限——因为 swap 被禁（`limit 0kB`），匿名页无处可逃，只能 OOM。

### memory.events 的计数

```
low 0
high 0
max 42      ← 42 次触碰 memory.max 上限（分配被拒绝/触发回收）
oom 1       ← 1 次 OOM 尝试
oom_kill 1  ← 1 次 OOM 杀进程
```

## 六、一个重要的纠正：vmstat 的 oom_kill 也计数

之前我预期"`/proc/vmstat` 的 oom_kill 保持 0"，但实测是 **1**。原因：**cgroup OOM 和全局 OOM 共享同一个 `oom_kill_process()` 代码路径**（调用栈里 cgroup OOM 也走 `out_of_memory → oom_kill_process`），所以全局 vmstat 计数器**两者都算**。

**真正的区分靠 dmesg 的 `constraint=` 字段**：

| constraint | 含义 |
|-----------|------|
| `CONSTRAINT_NONE` | 全局 OOM（系统内存耗尽）|
| `CONSTRAINT_MEMCG` | cgroup 限额 OOM |
| `CONSTRAINT_CPUSET` | cpuset 约束 OOM |

而 `memory.events` 的 `oom_kill` 是**组内专属**的计数（只记这个 cgroup）。

## 七、汽车场景的完整拼图

至此，汽车内存方向四件套齐了：

```
CMA（连续内存）→ DMA-BUF（零拷贝共享）→ Compaction（腾连续块）→ memcg（分区隔离）
```

车载系统分层：

```
硬件：SoC + 摄像头 + GPU/NPU
  ├─ CMA + DMA-BUF：传感器数据零拷贝流水线
  ├─ memcg：ECU 分区故障隔离（自动驾驶/娱乐/仪表互不影响）
  └─ hugetlbfs：AI 推理内存池（减少 TLB miss）
```

## 八、陷阱

1. **cgroup OOM 只杀组内**：这是特性（隔离），但如果限额设太小，组内进程会被反复杀（`oom_kill` 暴涨）。
2. **memory.max 要配 memory.swap.max**：否则超限先换出（性能下降）而不是 OOM，排查时容易误判。
3. **vmstat 的 oom_kill 不区分全局/cgroup**：排查时看 dmesg 的 `constraint=` 字段。
