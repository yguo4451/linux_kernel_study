# 05 EEVDF 调度器：eligible 与虚拟截止时间

> 环境：内核 6.6.44（arm64，QEMU `-smp 4`，`CONFIG_SCHED_DEBUG=y`）
> 主题：6.6 起 CFS 从"选 vruntime 最小"进化为 EEVDF（Earliest Eligible Virtual Deadline First）
> 前置：doc/02（vruntime 红黑树）、doc/03（调度周期与抢占）

---

## 0. 一句话

> **老 CFS 问"谁最吃亏"（vruntime 最小），EEVDF 问"谁最紧急、而且还有资格"。**
> eligible 管公平（防超跑插队），deadline 管延迟（防干等）。

---

## 1. 为什么 6.6 要换算法

老 CFS 的规则：红黑树按 `vruntime` 排序，选 vruntime 最小的任务。两个隐患：

1. **睡醒的任务会"插队"**：任务睡很久，vruntime 停住，醒来时 vruntime 很小 → 排到最前疯狂跑。
2. **没有显式延迟保证**：刚唤醒、急需响应的任务可能排在很多人后面干等。

EEVDF 引入两个新概念补上这两个洞。

---

## 2. 两个核心概念

### 概念一：eligible（合格）——防止超跑

引入 **lag（滞后量）**：任务"应得服务"与"实得服务"之差。

```
lag_i = S - s_i = w_i × (V - v_i)
```

其中 `V` = `avg_vruntime`（所有任务 vruntime 的加权平均）。

- `lag >= 0`：任务欠服务 → **eligible（E）**，有资格被调度
- `lag < 0`：任务超拿了 → **not eligible（N）**，暂时没资格

### 概念二：virtual deadline（虚拟截止时间）——保证延迟

每个任务每次被调度，等于"申请一个请求"：

```
deadline = vruntime + slice / 权重
```

`slice` = `sysctl_sched_base_slice`（缩放后 2.25ms，见下）。`deadline` 是这个请求的虚拟截止时间。

---

## 3. 核心公式

```
lag_i = w_i × (V - v_i)            V = avg_vruntime（加权平均 vruntime）

eligible ⟺ lag_i >= 0 ⟺ V >= v_i

deadline:  vd_i = ve_i + r_i / w_i = vruntime + calc_delta_fair(slice, weight)
```

## 4. 选任务规则（源码注释 `fair.c:863`）

```
1) 任务必须 eligible（欠服务）
2) 在 eligible 的任务里，选 deadline 最早的
```

数据结构：**增强红黑树**——树按 vruntime 排序，每个节点额外维护 `min_deadline`（子树最小 deadline），
一棵树同时"按 vruntime 排序 + 按 deadline 当堆用"，O(log n) 查找。

---

## 5. 源码关键点

| 函数/字段 | 位置 | 作用 |
|-----------|------|------|
| `se.deadline` / `se.vlag` / `se.slice` | `include/linux/sched.h:548` | EEVDF 新增的三个字段 |
| `avg_vruntime()` | `fair.c:671` | 加权平均 vruntime |
| `entity_eligible()` | `fair.c:744` | 判断 lag >= 0（输出 E/N 标记）|
| `update_deadline()` | `fair.c:1025` | `deadline = vruntime + calc_delta_fair(slice, se)` |
| `__pick_eevdf()` | `fair.c:881` | 增强红黑树搜索"deadline 最早且 eligible" |
| `pick_eevdf()` | `fair.c:975` | 兜底：找不到就取最左 |

---

## 6. base_slice 实测更正

源码默认 `sysctl_sched_base_slice = 750000ULL`（0.75ms），但**实际生效是 2.25ms**：

```
tunable_scaling = logarithmic → factor = 1 + log₂(4核) = 3
实际 slice = 0.75ms × 3 = 2.25ms
```

实测：`cat /sys/kernel/debug/sched/base_slice_ns` → `2250000`。

---

## 7. 观测方法：6.6 迁移到了 debugfs

**重大变化**：6.6 起 `/proc/sched_debug` **不存在了**，调度调试信息迁移到 debugfs：

```
/sys/kernel/debug/sched/
├── debug              ← 原来的 /proc/sched_debug（任务列表 + 每核 rq）
├── base_slice_ns      ← EEVDF 时间片
├── nr_migrate         ← 负载均衡单次最多搬几个
├── features           ← 调度特性开关
└── tunable_scaling    ← 时间片缩放模式
```

需要先挂载 debugfs（本项目已固化进 `/init`）：

```sh
mkdir -p /sys/kernel/debug
mount -t debugfs debugfs /sys/kernel/debug
```

单任务视角还有 `/proc/PID/sched`（有 `se.vruntime`、`se.load.weight`、切换计数，但**没有 deadline/vlag**）。

`/sys/kernel/debug/sched/debug` 的任务列表字段：

| 列 | 含义 |
|----|------|
| $1 | `>R`=正在运行，` R`=就绪排队 |
| $3 | PID |
| $4 | tree-key = vruntime（红黑树排序键）|
| $5 | **E/N（eligible 标记）** |
| $6 | deadline（虚拟截止时间）|
| $7 | slice（请求时间片）|

---

## 8. 实验记录

### 实验 1：两个任务不同核，验证公式

```sh
yes >/dev/null 2>&1 & P1=$!              # nice 0
nice -n 19 yes >/dev/null 2>&1 & P2=$!   # nice 19
for i in 1 2 3 4 5; do
  echo "--- t$i ---"
  grep " yes " /sys/kernel/debug/sched/debug | awk '{print $1,$2,$3,$4,$5,$6,$7}'
  sleep 0.3
done
kill $P1 $P2
```

观察（两个都 `>R`，被分到不同核）：

1. **vruntime 涨速比 ≈ 68 倍** = 1024/15，精确验证权重反比；
2. **deadline − vruntime 恒定**：P1 = 2.25，P2 = 153.6，精确验证 `calc_delta_fair`。

### 实验 2：锁同核，验证 EEVDF 决策

```sh
taskset -c 0 yes >/dev/null 2>&1 & P1=$!              # nice 0, 锁 CPU0
taskset -c 0 nice -n 19 yes >/dev/null 2>&1 & P2=$!   # nice 19, 锁 CPU0
for i in 1 2 3 4 5 6 7 8 9 10; do
  echo "--- t$i ---"
  grep " yes " /sys/kernel/debug/sched/debug | awk '{print $1,$2,$3,$4,$5,$6,$7}'
  sleep 0.2
done
kill $P1 $P2
```

观察（核心证据）：

| 任务 | 状态 | eligible | 结果 |
|------|------|----------|------|
| P1 nice0 | `>R` 全程 | `E` 全程 | 一直运行，独占 ~98% CPU |
| P2 nice19 | ` R` 全程 | **`N` 全程** | 一直排队，只拿 ~2% CPU |

**P2 为什么一直 N**：nice 19 权重 15，vruntime 涨速 ×68，跑 1ms 真实时间 vruntime 暴涨 68ms
→ 瞬间超服务（lag<0）→ `entity_eligible()` 返回 false → 失去资格 → 等 P1 的 vruntime 追上、avg_vruntime 超过它，才短暂变 E 再跑。

**`N` 标记 = `entity_eligible()` 返回 false 的现场输出**。

---

## 9. 关键结论

1. EEVDF = **eligible（公平门槛）+ virtual deadline（延迟保证）**，选"deadline 最早且 eligible"的任务。
2. eligible 堵住老 CFS"睡醒任务 vruntime 小、疯狂插队"的洞——超服务任务（N）没资格上桌。
3. 实际 base_slice = 2.25ms（0.75ms × 核数缩放），不是源码默认的 0.75ms。
4. 6.6 起调度调试信息在 **debugfs**（`/sys/kernel/debug/sched/`），不是 `/proc/sched_debug`。
5. 观测 EEVDF 的最简手段：`grep " yes " /sys/kernel/debug/sched/debug` 看 E/N 标记与 deadline。
