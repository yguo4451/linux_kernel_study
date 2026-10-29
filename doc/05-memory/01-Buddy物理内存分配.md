# Buddy System：物理内存的伙伴分配器

> 内存管理子系列第 1 篇。这是整条「内存管理主线」的地基：Buddy（物理页）→ slab（小对象）→ 页表（虚拟映射）→ 缺页（按需分配）→ 回收（kswapd/OOM）。

## 一、内存在内核里的层次（先看全景）

```
用户态 malloc/free
      │
      ▼
glibc 堆管理（brk / mmap）
      │
      ▼
内核：slab/slub 分配器   ← 分配"小对象"（task_struct、inode、sk_buff…）
      │  需要页时
      ▼
Buddy System            ← 分配"物理页"（4KB 的整数倍，2 的幂）  ★ 本篇
      │
      ▼
物理内存（node → zone → page）
```

**一句话定位**：Buddy 管的是**物理页帧**的分配与回收，是所有内存分配的最终底层。slab 在上面，页表映射在旁边，缺页异常触发分配。

## 二、核心数据结构（数据优先）

### 1. 三级层次：node → zone → page

```
NUMA node（节点，本机 arm64 通常只有 1 个 node 0）
 └─ zone（区域，按用途分 DMA / DMA32 / Normal / Movable）
     └─ page（页帧，每页 4KB，用 struct page 描述）

include/linux/mmzone.h
```

arm64 QEMU 上我们的内核一般只有 **Normal** 一个 zone（内存连续，不需要 DMA32 分段）。

### 2. struct free_area —— 伙伴系统的核心

```c
// include/linux/mmzone.h:117
struct free_area {
	struct list_head	free_list[MIGRATE_TYPES];  // 每种迁移类型一条链表
	unsigned long		nr_free;                  // 本 order 空闲页总数
};
```

每个 **order**（阶）有一个 `free_area`，挂在 `struct zone` 里：

```c
// include/linux/mmzone.h:938
struct zone {
    ...
    struct free_area	free_area[NR_PAGE_ORDERS];  // 0 ~ 10 共 11 阶
};
```

### 3. order 与块大小

```c
// include/linux/mmzone.h:29
#ifndef CONFIG_ARCH_FORCE_MAX_ORDER
#define MAX_PAGE_ORDER 10        // 最大阶 = 10
#endif
#define NR_PAGE_ORDERS (MAX_PAGE_ORDER + 1)   // 11 个阶：0~10
```

| order | 页数 | 大小（4KB 页）|
|-------|------|--------------|
| 0 | 1 | 4 KB |
| 1 | 2 | 8 KB |
| 2 | 4 | 16 KB |
| 3 | 8 | 32 KB |
| 9 | 512 | 2 MB |
| **10** | 1024 | **4 MB（最大块）**|

> `MAX_PAGE_ORDER` 在 arm64 上由 `CONFIG_ARCH_FORCE_MAX_ORDER` 决定，默认 10。注意它表示的是"阶"，对应 4KB 页最大 4MB 块；x86 上有的配置是 11（8MB）。

## 三、伙伴系统的核心思想：2 的幂 + XOR 找伙伴

**为什么块大小必须是 2 的幂？** 因为这样"找伙伴"只需要一次异或，O(1)：

```c
// mm/internal.h:607
static inline unsigned long
__find_buddy_pfn(unsigned long page_pfn, unsigned int order)
{
	return page_pfn ^ (1 << order);   // 伙伴 = 自己的 pfn 异或 2^order
}
```

例子（pfn 表示第几页）：

```
pfn 8 的 order-1 伙伴 = 8 ^ 2 = 10
   页 8-9  和 页 10-11 是一对伙伴，合并成 页 12-15（order 2）

pfn 12 的 order-2 伙伴 = 12 ^ 4 = 8
   页 12-15 和 页 8-11 是一对伙伴，合并成 页 8-15（order 3）
```

**两个核心操作**：

```
分配：需要 order=k 的块，但只有更大的 order=m 块
      → 把 m 阶块劈成两半，一半给出去，另一半挂回 m-1 阶链表
      → 递归直到凑出 k 阶   （函数 expand，向下分裂）

释放：释放一个 order=k 的块
      → 用 XOR 找它的伙伴，如果伙伴也空闲
      → 两半合并成 order=k+1 的块，再继续往上找
      → 直到伙伴不空闲或到 MAX_PAGE_ORDER   （函数 __free_one_page，向上合并）
```

## 四、分配路径调用链

```
用户态/内核要页
  → alloc_pages(gfp_mask, order)              mm/page_alloc.c
    → __alloc_pages
      → get_page_from_freelist                mm/page_alloc.c:3407
          ├─ 遍历 zonelist（按优先级选 zone）
          ├─ zone_watermark_fast()            检查水位线（低于水位进慢路径回收）
          └─ rmqueue(zone)
              → __rmqueue_smallest             mm/page_alloc.c:1628
                  从 order 往上找第一个非空链表
                  → page_del_and_expand        mm/page_alloc.c:1463
                      摘下来 → expand 把多余部分分裂回低阶链表
```

`__rmqueue_smallest` 关键代码（page_alloc.c:1628）：

```c
for (current_order = order; current_order < NR_PAGE_ORDERS; ++current_order) {
    area = &(zone->free_area[current_order]);
    page = get_page_from_free_area(area, migratetype);
    if (!page) continue;
    page_del_and_expand(zone, page, order, current_order, migratetype);
    return page;
}
```

**为什么从低往高找**：要 order=2（16KB），如果 order=2 链表空，就去 order=3 找一块 32KB，劈成两个 16KB，给一个、留一个。宁可分裂，不轻易跨 zone。

## 五、释放路径调用链（含 PCP）

```
free_pages(page, order)
  → free_unref_page                           mm/page_alloc.c:2714
      ├─ order ≤ 某阈值 → 先放进 PCP（每 CPU 页缓存）不立刻还给 Buddy
      │      free_unref_page_commit           mm/page_alloc.c:2663
      └─ order 较大 → 直接 __free_pages_ok
              → __free_one_page               mm/page_alloc.c:809
                  while (order < MAX_PAGE_ORDER) {
                      buddy = find_buddy_page_pfn(page, pfn, order, &buddy_pfn);
                      if (!buddy) break;          // 伙伴不空闲，停止
                      把伙伴从链表摘下，order++ 继续合并
                  }
                  挂回 free_area[order] 链表
```

**PCP（Per-CPU Pages）设计意图**：频繁分配/释放 order=0 的页（比如网卡收包），每次都锁 zone 的 freelist 会有争用。所以每个 CPU 先缓存一小批单页在 `per_cpu_pages`，够用就不用碰全局锁，快路径极快。这是"热点优化"的经典例子。

## 六、迁移类型：为什么把空闲页分成三类

```c
// include/linux/mmzone.h:45
enum migratetype {
	MIGRATE_UNMOVABLE,     // 不可移动：内核态（slab、页表、内核栈）
	MIGRATE_MOVABLE,       // 可移动：用户进程的匿名页
	MIGRATE_RECLAIMABLE,   // 可回收：页缓存
	...
};
```

`struct free_area.free_list[MIGRATE_TYPES]` 就是按这个分类的链表。

**目的：抗碎片化（anti-fragmentation）**。如果"可移动页"和"不可移动页"混在一起，长时间运行后大块连续内存被不可移动页钉死，再也凑不出 4MB 大页。分类型存放后：

1. 分配时优先从对应类型的链表拿
2. 内存紧张时，把 **MOVABLE 页迁移走**，腾出连续大块给需要连续内存的分配（如 CMA、大页、DMA）

`fallbacks` 数组（page_alloc.c:1649）定义了本类型耗尽时向谁借：

```c
static int fallbacks[MIGRATE_PCPTYPES][...] = {
	[MIGRATE_UNMOVABLE]   = { MIGRATE_RECLAIMABLE, MIGRATE_MOVABLE   },
	[MIGRATE_MOVABLE]     = { MIGRATE_RECLAIMABLE, MIGRATE_UNMOVABLE },
	[MIGRATE_RECLAIMABLE] = { MIGRATE_UNMOVABLE,   MIGRATE_MOVABLE   },
};
```

## 七、动手实验（QEMU 内）

### 实验 1：看每个 zone 每个 order 的空闲块

```sh
cat /proc/buddyinfo
# Node 0, zone   Normal     16   8   5   3   2   2   1   0   1   1   0
#                          └ order0─┘ └order1┘ ...            └order10
```

每一列是该 order 的空闲块**数量**（不是页数）。order0 的 16 表示有 16 个空闲单页。

### 实验 2：看迁移类型分布

```sh
cat /proc/pagetypeinfo
# 每行一种迁移类型，每列一个 pageblock 的块数
```

### 实验 3：看 zone 整体水位与统计

```sh
cat /proc/zoneinfo | head -40
# 看 pages free / min / low / high（水位线，下一篇回收的核心）
```

### 实验 4：压内存，观察 buddyinfo 变化

```sh
# 分配一大块内存并驻留，再看 buddyinfo 高阶块变少
dd if=/dev/zero of=/dev/null bs=1M count=200   # 短暂吃内存
cat /proc/buddyinfo                             # 对比前后 order 分布
```

## 八、陷阱与常见误区

1. **`/proc/buddyinfo` 是块数不是页数**：order2 的 "3" 是 3 个 16KB 块（48KB），不是 3 页。
2. **`MAX_PAGE_ORDER` 是"阶"不是"页数"**：默认 10 表示最大块 2^10 页 = 4MB，别和"最大分配 10 页"搞混。
3. **Buddy 只负责物理页**：它不知道"这个页给哪个进程、映射到哪个虚拟地址"——那是页表和缺页异常的事（后续模块）。
4. **释放不等于立即可用**：order=0 的页先进 PCP 缓存，`/proc/buddyinfo` 里可能看不到立即增加，要等 PCP 批量回流。

## 关键源码索引

| 功能 | 位置 |
|------|------|
| `MAX_PAGE_ORDER` / `NR_PAGE_ORDERS` | `include/linux/mmzone.h:29` |
| `struct free_area` | `include/linux/mmzone.h:117` |
| `enum migratetype` | `include/linux/mmzone.h:45` |
| `__find_buddy_pfn`（XOR 找伙伴）| `mm/internal.h:607` |
| `find_buddy_page_pfn`（校验伙伴）| `mm/internal.h:626` |
| `__free_one_page`（释放合并）| `mm/page_alloc.c:809` |
| `expand`（分配分裂）| `mm/page_alloc.c:1435` |
| `page_del_and_expand` | `mm/page_alloc.c:1463` |
| `__rmqueue_smallest`（从链表取）| `mm/page_alloc.c:1628` |
| `fallbacks`（迁移类型回退）| `mm/page_alloc.c:1649` |
| `free_unref_page`（PCP 释放入口）| `mm/page_alloc.c:2714` |
| `get_page_from_freelist`（遍历 zone）| `mm/page_alloc.c:3407` |
