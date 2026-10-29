# 内存回收：三条水位线与 kswapd

> 内存管理子系列第 6 篇。前五篇铺好了"分配"（Buddy/SLUB/缺页）和"承诺"（overcommit）。本篇讲"回收"的前半段：**空闲内存不够时，谁在什么时候出手、什么时候收手。** 核心是两个东西：三条水位线（min/low/high）和后台回收线程 kswapd。

## 一、现象：三条水位线

在 512MB 的最小系统上：

```sh
# cat /proc/meminfo | grep -E 'MemFree|Cached|Swap'
MemFree:      444224 kB
Cached:        13040 kB
SwapTotal:          0 kB      ← 没配 swap

# cat /proc/zoneinfo | grep -E 'min|low|high' | head -3
        min      678
        low      847
        high     1016
```

单位是**页**（4KB/页）：min≈2.6MB、low≈3.3MB、high≈4.0MB。规律：

```
low  - min  = 169
high - low  = 169        ← 间隔相等，且 = min/4
```

## 二、三条水位线的分工

```
free pages
   ▲
 high ──┐  kswapd 回收到 ≥ high 就停（睡觉）
   │     │
 low  ──┤  kswapd 被唤醒（free < low）
   │     │
 min  ──┘  紧急线：分配进程自己同步回收（direct reclaim）
```

| 区间 | 含义 |
|------|------|
| free ≥ high | 正常，kswapd 睡觉 |
| low ≤ free < high | 可分配但偏紧（fast path 判据是 low）|
| min ≤ free < low | 唤醒 kswapd 后台回收 |
| free < min | 分配者自己动手（direct reclaim），可能触发 OOM |

## 三、源码：水位线怎么算

### 1. min 的源头：`calculate_min_free_kbytes`

`mm/page_alloc.c:6214`：

```c
new_min_free_kbytes = int_sqrt(lowmem_kbytes * 16);   // = 4 × √(内存KB)
min_free_kbytes = clamp(new_min_free_kbytes, 128, 262144);
```

$$\text{min\_free\_kbytes} = 4\sqrt{\text{内存 KB 数}}$$

512MB → `4 × √524288 ≈ 2896 KB ≈ 2.8MB`。**平方根关系**：内存翻 4 倍，水位只翻 2 倍。

### 2. low/high 的间隔：`__setup_per_zone_wmarks`

`mm/page_alloc.c:6103`：

```c
tmp = max_t(u64, tmp >> 2,                             // min/4
            mult_frac(zone_managed_pages(zone),
                      watermark_scale_factor, 10000)); // 或 zone/1000
zone->_watermark[WMARK_LOW]  = min_wmark_pages(zone) + tmp;
zone->_watermark[WMARK_HIGH] = low_wmark_pages(zone) + tmp;
```

`watermark_scale_factor` 默认 10。在中小内存上 `min/4` 更大，所以：

```
low  = min + min/4
high = low + min/4 = min + min/2
```

对应 678/847/1016，吻合。

## 四、kswapd：什么时候醒、什么时候睡

### 唤醒侧（分配路径，`mm/page_alloc.c`）

`__alloc_pages_slowpath` 里，fast path 分配失败时：

```c
if (alloc_flags & ALLOC_KSWAPD)
    wake_all_kswapds(order, gfp_mask, ac);   // 唤醒 kswapd
```

### 回收侧：`balance_pgdat`（`mm/vmscan.c:6877`）

从 `DEF_PRIORITY` 开始循环调用 `pgdat_balanced` 判断是否已平衡。

### 平衡判据：`pgdat_balanced`（`mm/vmscan.c:6704`）

```c
mark = high_wmark_pages(zone);                 // ★ 用 HIGH 判平衡
if (zone_watermark_ok_safe(zone, order, mark, highest_zoneidx))
    return true;                               // free ≥ high → 已平衡
```

### 主循环：`kswapd`（`mm/vmscan.c:7210`）

```c
for ( ; ; ) {
    kswapd_try_to_sleep(...);                  // 先睡，等唤醒
    ...
    reclaim_order = balance_pgdat(pgdat, alloc_order, highest_zoneidx);
}
```

**一句话：kswapd 在 free < low 时被唤醒，回收到 free ≥ high 才回去睡。** high−low 的缓冲带是故意留的，防止"刚睡下又被唤醒"的颠簸。

## 五、`SwapTotal: 0` 的隐患

回收手段只有两类：

| 回收对象 | 需要条件 | 最小系统 |
|---------|---------|---------|
| 页缓存（Cached）| 有缓存可丢 | 只有十几 MB，杯水车薪 |
| 匿名页换出 | **需要 swap** | SwapTotal=0，换不出去 |

当 touch 吃满内存：kswapd 醒来 → 无缓存可回收、无 swap 可换出 → 回收无效 → 只能请 **OOM Killer** 杀进程。这是下一篇的主题。

## 六、实验记录

```sh
cat /proc/sys/vm/min_free_kbytes    # min 源头，约 2896
ps | grep kswapd                    # kswapd0（每个 NUMA node 一个）
cat /proc/zoneinfo | grep '^Node'   # 确认 zone 数量
mem_hog 300 1 &                     # touch 300MB，观察 MemFree 下降、kswapd 被唤醒
cat /proc/meminfo | grep MemFree
```

## 七、陷阱

1. **zoneinfo 里的 `high: 273` 不是水位线**：那是 per-CPU 页缓存池（PCP）的高水位，4 个值对应 4 个核。
2. **水位线单位是页不是 KB**：换算要 ×4KB。
3. **kswapd 判平衡用 high，唤醒判据是 low**：两者不是同一个数，别记混。
4. **`min_free_kbytes` 是平方根关系**：不是内存越大水位线线性越高。
