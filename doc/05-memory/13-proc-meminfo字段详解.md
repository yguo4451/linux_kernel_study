# /proc/meminfo 字段详解：内存体检报告

> 内存管理子系列第 13 篇（工具篇）。前 12 篇讲清了"分配→回收"的每个机制，本篇把 `/proc/meminfo` 的所有字段逐一解释，作为排查内存问题的"体检报告"速查表。字段按主题分组，与前面各篇一一对应。

## 一、总量与空闲（判断"够不够"）

| 字段 | 含义 | 对应 |
|------|------|------|
| `MemTotal` | 物理内存总量（去掉内核预留后的可用量）| — |
| `MemFree` | 完全未分配的页 | — |
| `MemAvailable` | **实际可用** = free + 可回收缓存 − 水位线预留 | doc 06/12 |

> **判断内存够不够只看 `MemAvailable`**，不看 `MemFree`（后者不含可回收缓存，会误判）。

## 二、页缓存与缓冲（doc 10）

| 字段 | 含义 |
|------|------|
| `Buffers` | 块设备元数据缓冲（超级块/inode 表/位图），通常几 MB |
| `Cached` | 页缓存 = 文件内容页 + `Shmem`（tmpfs）|
| `SwapCached` | 换出后又读回的共享页，暂时两边都留（doc 11）|

## 三、LRU 活跃度（doc 06 回收）

| 字段 | 含义 |
|------|------|
| `Active` / `Inactive` | 活跃 / 不活跃页总数 |
| `Active(anon)` / `Inactive(anon)` | 匿名页的活跃 / 不活跃 |
| `Active(file)` / `Inactive(file)` | 文件页的活跃 / 不活跃 |
| `Unevictable` | 不可回收页（= `Mlocked` 等）|
| `Mlocked` | `mlock` 锁定的页（doc 08）|

## 四、Swap（doc 11）

| 字段 | 含义 |
|------|------|
| `SwapTotal` / `SwapFree` | swap 总量 / 空闲（Used = Total − Free）|

## 五、脏页与写回（doc 12）

| 字段 | 含义 |
|------|------|
| `Dirty` | 脏页（已改未写回）|
| `Writeback` | 正在写回磁盘 |
| `WritebackTmp` | 内存紧张时临时回写页 |
| `NFS_Unstable` | NFS 客户端未落盘的页 |

## 六、进程内存

| 字段 | 含义 |
|------|------|
| `AnonPages` | 匿名页（进程堆/栈，无文件背页）|
| `Mapped` | 被 `mmap` 映射的文件页 |
| `Shmem` | tmpfs 共享内存（= `free` 的 shared 列，**不可回收**）|

## 七、Slab（doc 02）

| 字段 | 含义 |
|------|------|
| `Slab` | slab 分配器总内存（= SReclaimable + SUnreclaim）|
| `SReclaimable` | 可回收 slab（dentry/inode 缓存）|
| `SUnreclaim` | 不可回收 slab（内核核心结构）|
| `KReclaimable` | 可回收的内核内存（≈ SReclaimable + 其他）|

## 八、内核自身

| 字段 | 含义 |
|------|------|
| `KernelStack` | 内核栈（每个线程两块页）|
| `PageTables` | 页表占用的内存 |
| `SecPageTables` | 安全页表（arm64 KVM 第二阶段页表）|
| `Percpu` | per-CPU 变量 |

## 九、虚拟内存区

| 字段 | 含义 |
|------|------|
| `VmallocTotal` | vmalloc 虚拟地址区总大小 |
| `VmallocUsed` | vmalloc 已用 |
| `VmallocChunk` | vmalloc 最大连续空闲块 |

## 十、内存承诺（doc 05 overcommit）

| 字段 | 含义 |
|------|------|
| `CommitLimit` | 可承诺上限 = 内存 + swap × overcommit_ratio |
| `Committed_AS` | 当前已承诺总量（所有进程的虚拟内存之和）|

> `Committed_AS > CommitLimit` 且 overcommit_memory=2 时，新分配会被拒绝。

## 十一、CMA 连续内存（汽车方向，未学）

| 字段 | 含义 |
|------|------|
| `CmaTotal` | CMA 预留的连续物理内存总量 |
| `CmaFree` | CMA 当前空闲量 |

> 车载摄像头/ISP/编解码需要**连续物理内存**做 DMA，CMA 就是为此预留的池子。arm64 defconfig 默认预留 32MB。

## 十二、巨页（未学）

| 字段 | 含义 |
|------|------|
| `HugePages_Total/Free/Rsvd/Surp` | 显式巨页池：总/空/已预留/超额 |
| `Hugepagesize` | 巨页大小（arm64 为 2048 kB = 2MB）|
| `Hugetlb` | 巨页占用的内存 |

## 十三、其他

| 字段 | 含义 |
|------|------|
| `Bounce` | 反弹缓冲（老硬件 32 位 DMA 用，现代为 0）|
| `HardwareCorrupted` | ECC 检测到的硬件损坏内存 |

## 实战：读一份真实 meminfo

```sh
cat /proc/meminfo
```

关键读法（结合本系列知识）：

1. **`Cached == Shmem`**：`drop_caches` 后页缓存只剩 tmpfs 共享内存——文件页被清空，Shmem 清不掉（doc 10 的实证）。
2. **`Buffers` 极小（40 kB）**：元数据缓冲本来就小，大头在 `Cached`。
3. **`Active(file) == 0`**：drop 后没有活跃文件页了。
4. **`CmaTotal` 32MB**：内核预留了 32MB 连续内存（下一篇 CMA 的主角）。
5. **`Committed_AS (13MB) << CommitLimit (500MB)`**：承诺远低于上限，没有超卖风险。
