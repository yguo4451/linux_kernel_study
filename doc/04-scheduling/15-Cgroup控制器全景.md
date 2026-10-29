# cgroup v2 控制器全景：/sys/fs/cgroup 下到底能管什么

> 调度子系列第 15 篇。前一篇 [14-CPU隔离与cgroup](./14-CPU隔离与cgroup.md) 只讲了 `cpu.max` 一个文件，本篇铺开整个 cgroup v2 的资源管理地图——哪些文件管 CPU、哪些管内存、哪些管 I/O、哪些管进程数。

## 一、核心认知：目录里的文件分两类

```
/sys/fs/cgroup/test/
 ├─ 通用文件（core files）      ← 任何 cgroup 都有，不依赖控制器
 │    cgroup.procs / cgroup.controllers / cgroup.events / cgroup.freeze ...
 └─ 控制器文件（controller files） ← 只有父级 subtree_control 启用对应控制器才出现
      cpu.max / memory.max / pids.max / cpuset.cpus / io.max ...
```

判断一个文件属于哪类，看它出现在**哪个 cgroup 层级**：通用文件从根组就存在；控制器文件必须由祖先向下"显式授权"（`echo "+xxx" > cgroup.subtree_control`）才会在子组出现。这就是上一篇坑 2 的根源。

## 二、通用文件（每个组都有）

| 文件 | 权限 | 作用 |
|------|------|------|
| `cgroup.procs` | 写 | 把 PID 写进去 → 进程移入本组（我们实验用的就是它）|
| `cgroup.threads` | 写 | 移入单个线程（threaded 模式）|
| `cgroup.controllers` | 读 | 本组**可用**的控制器列表 |
| `cgroup.subtree_control` | 读写 | 本组要**下发给子组**的控制器（`+cpu` / `-cpu`）|
| `cgroup.events` | 读 | `populated`（是否有进程）/ `frozen`（是否冻结）|
| `cgroup.freeze` | 写 | 写 `1` 冻结组内所有进程，`0` 解冻 |
| `cgroup.kill` | 写 | 写 `1` 杀死组内所有进程 |
| `cgroup.type` | 读写 | `domain`（默认）/ `threaded`（线程级控制）|
| `cgroup.stat` | 读 | `nr_descendants` / `nr_dying_descendants` |
| `cgroup.max.depth` | 读写 | 限制子组嵌套深度（委托授权用）|
| `cgroup.max.descendants` | 读写 | 限制后代 cgroup 总数 |

**两个易混概念**：

- `cgroup.controllers` = 这个组**能用的**控制器（由祖先授权下来的）
- `cgroup.subtree_control` = 这个组**再往下传**的控制器（自己不用也能传）

理解"授权链"：root 的 `subtree_control` 写了 `+cpu`，子组 `test` 的 `controllers` 里才有 `cpu`；`test` 想再建 `test/a` 用 cpu，还得在 `test/subtree_control` 里再写 `+cpu`。**授权是一级一级往下传的**。

## 三、控制器文件（按资源分）

### 1. cpu —— 已启用 ✅（本篇重点）

| 文件 | 作用 |
|------|------|
| `cpu.max` | 带宽配额 `"quota period"`（前一篇主角，50% 限制）|
| `cpu.max.burst` | 突发额度（`CONFIG_CFS_BANDWIDTH`）|
| `cpu.weight` | CFS 相对权重（1~10000，默认 100，v1 的 cpu.shares）|
| `cpu.weight.nice` | 用 nice 值（-20~19）表达权重 |
| `cpu.idle` | 标记为 idle 调度类（最低优先级）|
| `cpu.stat` | 使用统计 + **节流证据** |
| `cpu.pressure` | PSI 压力统计（需 `CONFIG_PSI`）|

`cpu.stat` 是验证节流的关键文件（做完上一篇实验后可以 `cat` 看看）：

```
usage_usec  ...      ← 组内所有进程消耗的 CPU 总时间（μs）
user_usec / system_usec
nr_periods  100      ← 已经过了 100 个周期
nr_throttled 50      ← 其中 50 个周期被节流
throttled_usec ...   ← 累计被节流的时间
```

`cpu.weight` vs `cpu.max` 是两种完全不同的机制：

| | cpu.weight | cpu.max |
|--|-----------|---------|
| 语义 | **相对份额**（争抢时按权重分）| **绝对上限**（硬性封顶）|
| 不争抢时 | 不用也白不用 | 照样封顶 |
| 类比 | nice 的组级版 | 限速器 |

### 2. cpuset —— 已启用 ✅

| 文件 | 作用 |
|------|------|
| `cpuset.cpus` | 组内任务**允许**跑哪些 CPU（如 `0-2` 或 `3`）|
| `cpuset.cpus.effective` | 实际生效的 CPU（受祖先约束后的交集）|
| `cpuset.mems` | 允许用哪些 NUMA 内存节点 |
| `cpuset.mems.effective` | 实际生效的节点 |
| `cpuset.cpus.partition` | 分区模式（`member` / `root` / `isolated`）|

这是 doc/09 `isolcpus` 的**运行时替代品**——不必改内核启动参数，动态把某组任务钉到指定 CPU：

```sh
echo "3" > /sys/fs/cgroup/test/cpuset.cpus    # 组内任务只能跑 CPU3
```

### 3. memory —— 已启用 ✅

| 文件 | 作用 |
|------|------|
| `memory.current` | 当前内存占用（含 page cache）|
| `memory.min` | **硬保底**：低于此值永不回收 |
| `memory.low` | **软保护**：内存紧张时优先保护 |
| `memory.high` | **软上限**：超过先节流回收，不立即杀 |
| `memory.max` | **硬上限**：超过且回收不掉就 OOM kill |
| `memory.swap.max` | swap 上限 |
| `memory.events` | `low` / `high` / `max` / `oom` / `oom_kill` 事件计数 |
| `memory.stat` | 详细分类（anon / file / slab / kernel 等）|
| `memory.oom.group` | OOM 时杀**整个组**还是只杀单个进程 |
| `memory.reclaim` | 主动触发回收 |
| `memory.pressure` | PSI |

四个水位的关系（**必须记住的递进链**）：

```
min < low < high < max
保底    保护    节流    杀
```

- 超过 `max` 回收不掉 → OOM killer 出手
- 超过 `high` → 先被节流 + 强制回收，给机会自救
- 低于 `low`/`min` → 内存紧张时优先不碰它

### 4. pids —— 已启用 ✅

| 文件 | 作用 |
|------|------|
| `pids.max` | 组内最大进程/线程数（`max` 或数字）|
| `pids.current` | 当前数量 |
| `pids.peak` | 历史峰值 |

防 **fork 炸弹**：写 `pids.max = 100`，组内 fork 超过 100 个就直接 `EAGAIN`。汽车场景：防止一个 ECU 应用失控 fork 拖垮整块 SoC。

### 5. io —— 已编译但未启用 ⚠️

`CONFIG_BLK_CGROUP=y` 已编入，但 `/init` 没 `+io`，所以现在 `test` 目录下没有 io 文件。启用：

```sh
echo "+io" > /sys/fs/cgroup/cgroup.subtree_control
```

| 文件 | 作用 |
|------|------|
| `io.max` | 每设备 I/O 带宽/IOPS 上限，格式 `"8:0 rbps=... wbps=... riops=... wiops=..."` |
| `io.weight` | I/O 相对权重 |
| `io.stat` | 每设备的读写字节/次数统计 |

### 6. 其他已编译但特殊/ v1 专属

| 控制器 | 我们内核 | 说明 |
|--------|---------|------|
| freezer | `CONFIG_CGROUP_FREEZER=y` | v2 中表现为 `cgroup.freeze` 文件，不算独立控制器 |
| perf_event | `CONFIG_CGROUP_PERF=y` | 只挂 root，不参与 subtree_control |
| bpf | `CONFIG_CGROUP_BPF=y` | 用于挂 eBPF 程序做设备访问/网络过滤 |
| device | `CONFIG_CGROUP_DEVICE=y` | **v1 专属**，v2 中设备访问控制改用 eBPF |
| net_cls / net_prio | `CONFIG_NET_CLS=y` | **v1 专属**，v2 中网络用 BPF/tc |
| cpuacct | `CONFIG_CGROUP_CPUACCT=y` | **v1 专属**，v2 已并入 cpu 控制器 |

## 四、我们内核的实际清单

```sh
cat /sys/fs/cgroup/cgroup.controllers
# cpu cpuset memory pids      ← /init 已授权的四个
```

编译情况汇总：

| 控制器 | 配置项 | 值 | 状态 |
|--------|--------|-----|------|
| cpu | `CONFIG_CGROUP_SCHED` + `CONFIG_FAIR_GROUP_SCHED` | y | ✅ 启用 |
| cpuset | `CONFIG_CPUSETS` | y | ✅ 启用 |
| memory | `CONFIG_MEMCG` | y | ✅ 启用 |
| pids | `CONFIG_CGROUP_PIDS` | y | ✅ 启用 |
| io | `CONFIG_BLK_CGROUP` | y | 已编译，未 `+io` |
| freezer | `CONFIG_CGROUP_FREEZER` | y | `cgroup.freeze` 可用 |
| RT 组带宽 | `CONFIG_RT_GROUP_SCHED` | **not set** | ❌ 无 `cpu.rt.max`（RT 节流不在组级）|
| misc | `CONFIG_CGROUP_MISC` | not set | ❌ 无 misc 控制器 |
| rdma / hugetlb | —— | 未编译 | ❌ |

## 五、汽车实时 Linux 相关性

cgroup 是 **ECU 分区隔离** 的标准工具，四类控制器各管一个维度：

```
一个域控 SoC 上的多负载分区
  /sys/fs/cgroup/adas/
    cpu.max=30000 100000      ← 感知任务最多 30% CPU
    cpuset.cpus=2-3           ← 钉在高性能核
    memory.min=512M           ← 保底内存，关键时刻不被抢
    pids.max=500              ← 防线程泄漏拖垮系统
  /sys/fs/cgroup/infotain/
    cpu.max=20000 100000      ← 娱乐系统封顶 20%
    io.weight=50              ← I/O 让路给 adas
```

- **cpu + cpuset**：算力隔离（配合 doc/09 isolcpus、doc/13 deadline）
- **memory**：内存隔离，防止一个负载吃光内存触发全局 OOM
- **pids**：进程数封顶，防 fork 炸弹
- **io**：存储带宽隔离（OTA 刷写时不干扰实时负载）

> 注意：`cpu.max` 只管 CFS 任务，`memory.max` 只管内存，`pids.max` 只管进程数——**它们是正交的**，一个 ECU 应用通常要同时设多道闸。

## 六、快速自查命令

```sh
# 看本机有哪些控制器可用
cat /sys/fs/cgroup/cgroup.controllers

# 看当前已下发到子组的
cat /sys/fs/cgroup/cgroup.subtree_control

# 看 test 组里实际有哪些文件（哪些控制器生效了）
ls /sys/fs/cgroup/test/
```

## 关键源码索引

| 控制器 | 源码位置 |
|--------|---------|
| cpu（带宽/权重）| `kernel/sched/core.c`、`kernel/sched/fair.c` |
| cpuset | `kernel/cgroup/cpuset.c` |
| memory | `mm/memcontrol.c` |
| pids | `kernel/cgroup/pids.c` |
| io（blk）| `block/blk-cgroup.c` |
| freezer | `kernel/cgroup/freezer.c` |
| cgroup v2 通用文件 | `kernel/cgroup/cgroup.c`（`cgroup_base_files[]` / `cgroup_psi_files[]`）|
