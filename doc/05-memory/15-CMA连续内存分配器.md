# CMA 连续内存分配器：DMA 的连续内存池

> 内存管理子系列第 15 篇（汽车方向第一篇）。前面讲的 Buddy 只能分配"零散"的物理页，但摄像头/ISP/编解码做 DMA 时需要**物理连续**的大块内存。CMA 就是解决这个矛盾的机制，是车载 SoC 内存管理的基石。

## 一、问题：DMA 要连续，Buddy 给不了

DMA 控制器按**物理地址**直接读写内存（不经过 MMU），所以要求内存物理连续。但 Buddy 系统长期运行后**外部碎片化**：4KB 小页到处都是，却凑不出几 MB 的连续块。

```
Buddy 空闲页（碎片化后）：
[4K][4K][4K][4K]...[4K]   ← 一堆零散小页，凑不出 32MB 连续块
```

## 二、CMA 的思路：预留 + 借出 + 迁移

```
启动时预留 32MB 连续物理区（CMA 池）
   ├─ 平时：借给普通进程（只放 MIGRATE_MOVABLE 可移动页）
   └─ DMA 要连续块时：把这些可移动页【迁移走】→ 腾出整块 → 给 DMA
```

| 动作 | 说明 |
|------|------|
| 预留 | `dma_contiguous_reserve` 启动时划出（arm64 默认 32MB，`cma=` 可调）|
| 借出 | CMA 页平时当普通页用，但**只能放可移动页**（匿名页/页缓存）|
| 迁移 | `alloc_contig_range` 把借出的页搬走，腾出连续块 |

**关键**：CMA 页必须是 `MIGRATE_MOVABLE` 可移动页——因为"迁移"是 CMA 的命根子，页移不动就腾不出连续块。

## 三、源码链路

| 模块 | 函数 | 作用 |
|------|------|------|
| `mm/cma.c` | `cma_alloc` / `cma_release` | CMA 池的分配/释放 |
| `kernel/dma/contiguous.c` | `dma_alloc_contiguous` | DMA 框架从 CMA 取连续内存 |
| `mm/page_alloc.c` | `alloc_contig_range` | 迁移可移动页、腾连续块 |
| `mm/compaction.c` | `compaction` | 迁移的执行机制（doc 14 的 `compact_*` 计数器）|

## 四、观测接口（三个文件互相印证）

| 文件 | 字段 | 含义 |
|------|------|------|
| `/proc/meminfo` | `CmaTotal` / `CmaFree` | CMA 总量 / 空闲 |
| `/proc/vmstat` | `nr_free_cma` / `cma_alloc_success` / `cma_alloc_fail` | 空闲页 / 分配成功 / 分配失败 |
| `/proc/pagetypeinfo` | `type CMA` 行 | CMA 空闲页按 order 的分布 |
| `/proc/iomem` | `reserved` 块 | CMA 藏在预留区里 |
| `dmesg` | `cma: Reserved 32 MiB at ...` | CMA 的**精确物理地址**（最权威）|

## 五、实测数据（QEMU virt，32MB CMA）

```sh
grep -i cma /proc/meminfo
# CmaTotal: 32768 kB   预留 32MB
# CmaFree:  32292 kB   空闲 31.5MB（只被用了 476KB）

grep cma /proc/vmstat
# nr_free_cma 8073       8073 页 × 4KB = 32292 kB = CmaFree ✓
# cma_alloc_success 3    成功分配 3 次（virtio 的 dma-coherent）
# cma_alloc_fail 0       从未失败

grep -i cma /proc/pagetypeinfo
# Node 0, zone DMA, type CMA  5 4 1 3 4 5 4 5 5 5 3
```

### pagetypeinfo CMA 行的读法

那 11 个数字是 **order 0~10 的空闲块数**（order N 的块 = 2^N 页）：

| order | 块大小 | 空闲块数 | 合计页 |
|-------|--------|---------|--------|
| 0 | 4KB | 5 | 5 |
| 1 | 8KB | 4 | 8 |
| 2 | 16KB | 1 | 4 |
| 3 | 32KB | 3 | 24 |
| 4 | 64KB | 4 | 64 |
| 5 | 128KB | 5 | 160 |
| 6 | 256KB | 4 | 256 |
| 7 | 512KB | 5 | 640 |
| 8 | 1MB | 5 | 1280 |
| 9 | 2MB | 5 | 2560 |
| 10 | 4MB | **3** | **3072** |

**总和 = 8073 页 = 32292 kB = CmaFree**（严丝合缝）。

**关键结论**：`order 10` 还有 **3 个 4MB 的连续块**（共 12MB），说明 CMA 池还很不碎——DMA 随时能拿到大块连续内存。如果这些大 order 的块都变成 0，只剩 order 0/1 的碎片，CMA 就会 `cma_alloc_fail`。

## 六、为什么 CMA 平时"借出"不浪费

CMA 最妙的地方：预留的 32MB 平时不闲着，而是借给系统当普通内存（放可移动页）。所以：

- 不借出 → 32MB 白预留，浪费
- 借出 → 物尽其用，DMA 需要时再迁移腾出

这就是"**平时当普通内存用，需要时变连续内存**"的设计哲学。

## 七、汽车相关性

车载 SoC 摄像头管线重度依赖 CMA：

```
摄像头 sensor → ISP 处理 → 视频编解码 → 显示
     │            │             │
     └── 都需要几 MB~几十 MB 物理连续内存做 DMA ──► CMA 池
```

- 摄像头分辨率越高，需要的连续 buffer 越大（4K@60fps 单帧 ~30MB）
- CMA 池太小 → `cma_alloc_fail` 增长 → 花屏/丢帧/编解码失败
- 所以车载 BSP 里 CMA 大小是重点调优项（通常几百 MB）

## 八、陷阱

1. **CMA 大小要权衡**：太大浪费（借出也有限制），太小 DMA 分配失败。车载常见 64MB~512MB。
2. **迁移有成本**：DMA 每次要连续块，都要把借出的页搬走（`compact_*` 计数器会涨），频繁迁移影响性能。
3. **不可移动页占着 CMA**：如果 CMA 里混入不可移动页（内核结构），就腾不出来了——所以 CMA 页严格限制为 MIGRATE_MOVABLE。
