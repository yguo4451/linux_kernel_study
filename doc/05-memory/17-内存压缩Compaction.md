# 内存压缩 Compaction：碎片整理器

> 内存管理子系列第 17 篇。doc 01 讲了 Buddy 分配器，但它有个天生缺陷：长期运行后**外部碎片化**——空闲页很多，却都是零散的 4KB 小页，凑不出大块连续页。Compaction 就是解决这个问题的"碎片整理器"。

## 一、问题：外部碎片（External Fragmentation）

Buddy 按 order（2^order 页）分配块。系统跑久了，空闲页被"在用页"隔开：

```
碎片化: [用A][空][用B][空][用C][空][用D]...
   ↑ 空闲页 4 个，但彼此不相邻，凑不出一个 order 2（16KB）的块
```

总空闲够，但"连续"的不够——这就是外部碎片。需要大块连续内存的调用方（CMA、THP、DMA）会因此分配失败。

## 二、Compaction 干什么：搬走可移动页，合并空闲页

Compaction 把**可移动页**迁移到别处，让空闲页相邻、合并成高阶块：

```
碎片化: [用A][空][用B][空][用C][空]
   ↓ 迁移可移动页 A/B/C
压缩后: [空][空][空][用A][用B][用C]
          ↑ 3 个空闲页连续 = 一个 order 1（8KB）块
```

**核心限制**：只能搬**可移动页**（匿名页、页缓存 = `MIGRATE_MOVABLE`）。不可移动页（内核结构、DMA buffer = `MIGRATE_UNMOVABLE`）像"钉子"钉在原地——这就是 doc 15 里 CMA 区域严格限制为 MOVABLE 的原因。

## 三、触发条件

| 触发源 | 场景 | 计数器 |
|--------|------|--------|
| 高阶分配失败 | `alloc_pages(order>0)` 失败 → 直接压缩 | `compact_stall` |
| kcompactd 后台线程 | 定期整理碎片 | `compact_daemon_wake` |
| CMA 分配 | 要连续内存，先腾块（doc 15）| `cma_alloc_*` |
| THP 分配 | 要 2MB 连续页（doc 18）| — |
| **手动触发** | `echo 1 > /proc/sys/vm/compact_memory` | 本实验 |

## 四、源码

| 模块 | 作用 |
|------|------|
| `mm/compaction.c` | `compact_zone`：压缩一个 zone 的核心 |
| `mm/migrate.c` | `migrate_pages`：页迁移执行 |
| `mm/page_alloc.c` | `__alloc_pages_direct_compact`：分配失败→直接压缩 |

## 五、实测：手动触发前后对比

```sh
# ① 压缩前
cat /proc/buddyinfo
# Node 0, zone DMA: 271 353 361 85 66 43 41 30 23 16 85

# ② 手动触发全量压缩
echo 1 > /proc/sys/vm/compact_memory

# ③ 压缩后
cat /proc/buddyinfo
# Node 0, zone DMA: 235 247 237 95 74 48 44 33 22 16 85
```

### buddyinfo 前后对比（order 0~10 的空闲块数）

| order | 块大小 | 前 | 后 | 变化 |
|-------|--------|-----|-----|------|
| 0 | 4KB | 271 | 235 | **−36** |
| 1 | 8KB | 353 | 247 | **−106** |
| 2 | 16KB | 361 | 237 | **−124** |
| 3 | 32KB | 85 | 95 | +10 |
| 4 | 64KB | 66 | 74 | +8 |
| 5 | 128KB | 43 | 48 | +5 |
| 6 | 256KB | 41 | 44 | +3 |
| 7 | 512KB | 30 | 33 | +3 |
| 8~10 | 1~4MB | 不变 | 不变 | 0 |

### 三个铁证

1. **小页减少、大页增多**：order 0/1/2 共减少 266 块，order 3~7 共增加 29 块。零散的 4KB/8KB/16KB 小页被**合并成了 32KB~512KB 的大块**——这正是 Compaction 的搬移合并。

2. **总空闲页几乎不变**：压缩前 113117 页，压缩后 113061 页（差 56 页是 `echo` 命令自身分配）。**Compaction 不创造也不销毁内存，只"重新排列"**。

3. **vmstat 计数器跳变**（`grep compact_ /proc/vmstat`）：

```
compact_migrate_scanned  40228 → 206116  （+16.6 万页，扫描量暴增）
pgmigrate_success        3458  → 4425    （+967 页，实际迁移成功）
```

注意 `compact_stall` 和 `compact_success` 都是 0：那是"分配时触发的直接压缩"专用计数器，手动 `echo 1` 不算 stall（不阻塞分配），所以不动。而 `compact_daemon_*` 是 kcompactd 后台线程的独立账本，手动压缩也不影响它。

## 六、与 CMA / THP 的连接

```
Compaction（搬页、腾连续块）
   ├─► CMA 分配连续内存时调用（doc 15）
   └─► THP 分配 2MB 巨页时调用（doc 18）
```

Compaction 是"腾大块"的机制，CMA 和 THP 是"用大块"的两个消费者。

## 七、陷阱

1. **不可移动页是死穴**：迁移类型里 `Unmovable` 的页搬不动，它们卡在中间就会让压缩失败（`compact_fail` 增长）。
2. **压缩有成本**：迁移页要改页表、刷 TLB，频繁压缩影响性能（`compact_migrate_scanned` 大 = 压缩在频繁干活）。
3. **压缩 ≠ 释放内存**：它只整理碎片，不减少内存占用。
