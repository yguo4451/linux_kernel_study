# /proc/vmstat 字段详解：内存事件账本

> 内存管理子系列第 14 篇（工具篇，doc 13 的姊妹篇）。`/proc/meminfo` 是"当前快照"，`/proc/vmstat` 是**开机以来的累计计数器**——每发生一次内存事件就 +1。想回放系统"发生过什么"，查它。

## 一、与 meminfo 的分工

| | `/proc/meminfo` | `/proc/vmstat` |
|---|---|---|
| 性质 | 快照（当前值）| 累计计数（历史事件次数）|
| 单位 | kB | 页（×4KB）或次数 |
| 场景 | "现在内存怎么分布" | "系统发生过多少次缺页/换页/回收" |

## 二、页状态计数器（当前值，对应 meminfo）

| 字段 | 含义 |
|------|------|
| `nr_free_pages` | 空闲页数（×4KB = MemFree）|
| `nr_zone_inactive_anon` / `nr_zone_active_anon` | 不活跃 / 活跃匿名页 |
| `nr_zone_inactive_file` / `nr_zone_active_file` | 不活跃 / 活跃文件页 |
| `nr_zone_unevictable` / `nr_mlock` | 不可回收页 / mlock 页 |
| `nr_free_cma` | CMA 空闲页（×4KB = CmaFree）|
| `nr_anon_pages` / `nr_mapped` / `nr_file_pages` / `nr_shmem` | 匿名页 / mmap 页 / 文件页 / tmpfs |
| `nr_dirty` / `nr_writeback` / `nr_writeback_temp` | 脏页 / 回写中 / 临时回写 |
| `nr_slab_reclaimable` / `nr_slab_unreclaimable` | 可回收 / 不可回收 slab |
| `nr_kernel_stack` / `nr_page_table_pages` | 内核栈 / 页表 |

## 三、NUMA

| 字段 | 含义 |
|------|------|
| `numa_hit` / `numa_miss` | 分配落在本地 / 远端节点 |
| `numa_local` / `numa_other` | 本地 / 跨节点分配 |
| `numa_foreign` / `numa_interleave` | 外节点页面 / 交错分配 |

> 单 NUMA 节点（如 QEMU virt）时 `numa_miss = 0`，全部本地。

## 四、页分配与释放（Buddy，doc 01）

| 字段 | 含义 |
|------|------|
| `pgalloc_dma` / `pgalloc_normal` / `pgalloc_movable` | 从各 zone 分配的页 |
| `pgfree` | 释放的页 |
| `allocstall_normal` / `allocstall_movable` | **分配停滞**（Buddy 分不出，触发直接回收）|
| `pgactivate` / `pgdeactivate` | 页在 LRU 活跃/不活跃间迁移 |
| `pgreuse` / `pgrefill` | 复用 / 补充 |

> `allocstall_*` 高 = 内存紧张，分配经常被卡住。

## 五、缺页（doc 04）

| 字段 | 含义 |
|------|------|
| `pgfault` | 总缺页次数（minor + major）|
| `pgmajfault` | **主缺页**次数（读磁盘/swap 的缺页）|

> `pgmajfault / pgfault` 比例高 = 大量访问被换出/未缓存的数据，性能差。

## 六、回收扫描（doc 06，核心）

| 字段 | 含义 |
|------|------|
| `pgscan_kswapd` / `pgsteal_kswapd` | kswapd 扫描 / 回收的页数 |
| `pgscan_direct` / `pgsteal_direct` | 直接回收扫描 / 回收的页数 |
| `pgscan_anon` / `pgscan_file` | 扫描的匿名页 / 文件页 |
| `pgsteal_anon` / `pgsteal_file` | 回收的匿名页 / 文件页 |
| `pageoutrun` | kswapd 运行次数 |
| `pgrotated` | 页在 LRU 末尾旋转次数 |
| `kswapd_low_wmark_hit_quickly` | kswapd 因低于 low 水位被唤醒 |
| `oom_kill` | OOM 杀进程次数 |
| `drop_pagecache` / `drop_slab` | 手动 `drop_caches` 次数 |

> **steal / scan 比值** = 回收效率。`pgsteal_anon > pgsteal_file` 说明匿名页被回收更多（有 swap 时）。

## 七、写回（doc 12）

| 字段 | 含义 |
|------|------|
| `nr_dirtied` | 累计弄脏的页 |
| `nr_written` | 累计写回的页 |
| `nr_throttled_written` | 被节流后写的页 |
| `nr_vmscan_write` | 回收时写出的页（含换出）|
| `nr_vmscan_immediate_reclaim` | 立即回收次数 |

## 八、Swap（doc 11）+ 零页优化

| 字段 | 含义 |
|------|------|
| `pswpin` / `pswpout` | 换入 / 换出的页（**真实 I/O**）|
| `swap_ra` / `swap_ra_hit` | swap 预读 / 预读命中 |
| `nr_swapcached` | swap cache 中的页 |

### ★ 零页优化：`swpin_zero` / `swpout_zero`

换出时若检测到**整页全零**，内核不写 swap、只记"零页"标记；换入时也直接填充零，不读盘。

```
pswpout（真实写盘）+ swpout_zero（零页跳过）= 总换出页数
```

实例：`mem_hog` 写 `p[i]=(char)i` 且 i 恒为 4096 的倍数 → 全零页 → 换出 431MB 里几乎全走 `swpout_zero`，真实 `pswpout` 只有 7.6MB。

## 九、压缩与迁移（Compaction，未学）

| 字段 | 含义 |
|------|------|
| `compact_migrate_scanned` / `compact_free_scanned` | 压缩迁移 / 空闲扫描页数 |
| `compact_success` / `compact_fail` / `compact_stall` | 压缩成功 / 失败 / 停滞 |
| `compact_daemon_wake` | kcompactd 唤醒次数 |
| `pgmigrate_success` / `pgmigrate_fail` | 页迁移成功 / 失败 |

> CMA 要连续物理内存，分配不到时触发压缩（`compact_*` 增长）。

## 十、CMA（未学，汽车方向）

| 字段 | 含义 |
|------|------|
| `cma_alloc_success` / `cma_alloc_fail` | CMA 分配成功 / 失败次数 |

> `cma_alloc_success=3` 对应 vmallocinfo 里 3 个 `dma-coherent` 映射。

## 十一、巨页（未学）

| 字段 | 含义 |
|------|------|
| `nr_anon_transparent_hugepages` | 匿名 THP 巨页数 |
| `nr_shmem_hugepages` / `nr_file_hugepages` | tmpfs / 文件巨页 |
| `nr_shmem_pmdmapped` / `nr_file_pmdmapped` | 被 PMD 映射的巨页 |
| `htlb_buddy_alloc_success` / `htlb_buddy_alloc_fail` | hugetlb 从 Buddy 分配 |

## 十二、其他

| 字段 | 含义 |
|------|------|
| `balloon_inflate` / `balloon_deflate` | 虚拟化气球内存膨胀 / 收缩 |
| `ksm_swpin_copy` / `cow_ksm` | KSM 去重相关 |
| `nr_unstable` | NFS 未落盘页 |
| `nr_foll_pin_acquired` / `nr_foll_pin_released` | pin 用户页（RDMA 等）|

## 十三、workingset（工作集）

| 字段 | 含义 |
|------|------|
| `workingset_refault_anon` / `workingset_refault_file` | 被回收后又被访问（refault）的匿名页/文件页 |
| `workingset_activate_*` / `workingset_restore_*` | 激活 / 恢复的页 |
| `workingset_nodereclaim` | 节点级回收次数 |

> `workingset_refault_anon` 爆表 = 大量页刚被换出就被访问回来 = **换页抖动 thrashing** 的直接证据（swap 实验留下的）。

## 十四、排查速查

| 想查什么 | 看哪个计数器 |
|---------|-------------|
| 有没有 OOM | `oom_kill` |
| 缺页多不多 | `pgfault` / `pgmajfault` |
| 回收压力 | `pgscan_kswapd` + `pgsteal_kswapd` |
| 换页活动 | `pswpin` / `pswpout` / `swpin_zero` / `swpout_zero` |
| 是否抖动 | `workingset_refault_anon` |
| 分配是否停滞 | `allocstall_*` |
| 写回压力 | `nr_dirtied` / `nr_written` |
| CMA 用没用 | `cma_alloc_success` |
| 压缩活动 | `compact_*` |

> 实时观测增量用 `vmstat 1`（每秒差值）；回放历史用 `cat /proc/vmstat`（累计值）。
