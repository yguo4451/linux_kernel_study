# 巨页：hugetlbfs 与 THP

> 内存管理子系列第 18 篇。doc 03 讲过 TLB，本篇讲如何用**巨页**减少 TLB miss——这是高性能计算（数据库、AI 推理）的关键优化，也是实时系统的一个著名权衡点。

## 一、问题：TLB miss

TLB 是 MMU 里缓存"虚拟→物理"翻译的硬件，**条目有限**：

```
4KB 普通页：TLB 100 条 → 覆盖 100×4KB = 400KB
2MB 巨页  ：TLB 100 条 → 覆盖 100×2MB = 200MB（512 倍！）
```

大内存应用（数据库、AI、大模型推理）用 4KB 页会频繁 TLB miss，每次 miss 都要查多级页表。用 2MB 巨页，同样 TLB 条目覆盖 512 倍内存 → TLB miss 暴降。

## 二、两种巨页

| | hugetlbfs（显式）| THP（透明巨页）|
|---|---|---|
| 谁决定 | **程序员**（手动 mmap）| **内核**（自动）|
| 改代码 | 要（`MAP_HUGETLB`）| 不要（透明）|
| 配置 | `CONFIG_HUGETLBFS` | `CONFIG_TRANSPARENT_HUGEPAGE` |
| 实时友好 | ✅ 可精确控制 | ❌ khugepaged 延迟尖峰 |

## 三、hugetlbfs 机制（本实验）

1. **预留**：`echo 8 > /proc/sys/vm/nr_hugepages` 从 Buddy 划出 8 个 2MB 巨页
2. **分配**：`mmap(..., MAP_HUGETLB | MAP_ANONYMOUS, ...)` 直接拿巨页
3. **观测**：`/proc/meminfo` 的 `HugePages_Total/Free/Rsvd/Surp`

## 四、实测数据

```sh
echo 8 > /proc/sys/vm/nr_hugepages     # 预留 8 个 2MB 巨页
grep HugePages_Free /proc/meminfo      # 8

huge_mmap 4 &                          # 用 MAP_HUGETLB 分配 4MB = 2 个巨页
grep HugePages_Free /proc/meminfo      # 6  ← 用了 2 个！

mem_hog 100 &                          # 普通 malloc 100MB
grep HugePages_Free /proc/meminfo      # 6  ← 不变！malloc 走 4KB 页

huge_mmap 20                           # 要 10 个巨页，池里只剩 6 个
# mmap MAP_HUGETLB: Cannot allocate memory  ← 失败

kill %1                                # 杀 huge_mmap
grep HugePages_Free /proc/meminfo      # 8  ← 巨页归还池子
```

### 三个关键结论

1. **巨页是"另开的池子"**：`malloc` 走 Buddy 的 4KB 页，`MAP_HUGETLB` 走巨页池，两者井水不犯河水——所以 `mem_hog 100` 分毫不动 `HugePages_Free`。
2. **预留决定上限**：8 个巨页 = 16MB，要 20MB（10 个）必然失败。这就是 `HugePages_Total` 的意义——巨页不能超配。
3. **释放即归还**：进程退出，巨页回到池子（`HugePages_Free` 回升）。

## 五、THP 为什么没开（实时性的著名权衡）

你的内核 `CONFIG_TRANSPARENT_HUGEPAGE` 未开启。THP 靠 **khugepaged** 后台线程做 Compaction（doc 17）来凑 2MB 连续页，但 Compaction 要迁移页 → 改页表 → 刷 TLB，引入**不可预测的延迟尖峰**。

对实时系统（PREEMPT_RT），这个延迟是灾难，所以**实时系统通常禁用 THP**——和 doc 09 cyclictest 的教训同源：

```
THP 的 khugepaged 压缩延迟  ↔  doc 09 的 swap-in 162ms 停顿
        ↑ 都是"不可预测延迟"，实时任务都怕
```

**工程权衡**：THP 提升吞吐（TLB miss 少），但牺牲确定性（khugepaged 延迟）。实时系统要确定性，所以禁 THP、改用 hugetlbfs（可精确控制）。

## 六、观测接口

| 接口 | 内容 |
|------|------|
| `/proc/sys/vm/nr_hugepages` | 预留的巨页数（可写）|
| `/proc/meminfo` 的 `HugePages_*` | 巨页池状态 |
| `/proc/meminfo` 的 `Hugepagesize` | 巨页大小（arm64 = 2MB）|
| `/sys/kernel/mm/transparent_hugepage/enabled` | THP 模式（always/madvise/never）|

## 七、陷阱

1. **巨页不能超配**：`nr_hugepages` 就是硬上限，预留多少才能用多少。
2. **巨页有内部碎片**：分配 2MB 巨页但只用 1KB，浪费 2047KB。
3. **预留巨页 = 锁定内存**：`nr_hugepages` 预留给巨页池，普通内存分配用不了，预留太多会挤占常规内存。
4. **THP 与实时冲突**：实时系统禁 THP，用 hugetlbfs 精确控制。
