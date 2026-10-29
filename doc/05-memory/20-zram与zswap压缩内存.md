# zram 与 zswap：压缩内存

> 内存管理子系列第 20 篇。前面 doc 11 讲的 swap 是把匿名页写到**磁盘**（慢）。本篇讲两种**用压缩换内存**的技术：把页压缩后存在 RAM 里，用 CPU 压缩/解压的开销，换物理内存的节省——嵌入式/车载/Android 的标配。

## 一、共同思想：用 CPU 换内存

```
传统 swap：匿名页 → 写磁盘（慢，但有 TB 级空间）
压缩技术：匿名页 → 压缩（CPU 干活）→ 存 RAM（快，省内存）
```

压缩比通常 2~4 倍：100MB 数据压成 25~50MB。代价是压缩/解压要 CPU，但比磁盘 I/O 快一个数量级。

## 二、zram：压缩的块设备（本身就是 swap）

zram 在 RAM 里创建一个**压缩的块设备** `/dev/zram0`，把它格式化成 swap：

```
匿名页要换出
  → 压缩（如 lz4/zstd）
    → 写进 zram（RAM 里的压缩区）
      → 物理内存省出来了
```

**特点**：

- **完全没有磁盘 I/O**：换出=压缩写 RAM，换入=解压读 RAM
- 适合**无磁盘**的设备（嵌入式、车载、Android 手机/车机）
- Android 从 4.4 起标配 zram，车载 AAOS 也用它

```
/dev/zram0（压缩块设备）
  ├─ 写入 100MB 数据 → 压缩后只占 30MB 物理内存
  └─ 换来"多出"70MB 可用内存
```

## 三、zswap：压缩的 swap 缓存（拦截层）

zswap 是 RAM 里的一个**压缩缓存池**，挡在匿名页和**真实 swap** 之间：

```
匿名页要换出
  → 先压进 zswap 池（RAM 里的压缩缓存）
    → 池满了 → 才写真实 swap（磁盘）
```

**特点**：

- **有真实 swap 后端**（磁盘/swapfile），zswap 是"压缩缓存层"
- 减少实际 swap I/O：热页命中 zswap（快），冷页才落盘（慢）
- 适合**有磁盘**的服务器/桌面

## 四、关键区别（一张表）

| | zram | zswap |
|---|---|---|
| 本质 | 压缩的**块设备**（就是 swap）| 压缩的 **swap 缓存**（拦截层）|
| 后端 | 无（全在 RAM）| 有真实 swap 分区 |
| 数据流 | 匿名页 → 压缩 → zram | 匿名页 → 压缩 → zswap 池 → 满则写磁盘 |
| 磁盘 I/O | **零** | 有（池满时）|
| 适用 | 无磁盘的嵌入式/车载/Android | 有磁盘的服务器/桌面 |

## 五、数据流对比

```
zram（无磁盘）:
  匿名页 ──压缩──► zram(RAM) ──► 没了（全在内存里）

zswap（有磁盘）:
  匿名页 ──压缩──► zswap 池(RAM) ──池满──► 真实 swap(磁盘)
```

## 六、车载场景

车载 SoC 常**没有大容量磁盘**（eMMC/UFS 有限且要留寿命），zram 是标配：

```
车载 Android（AAOS）：
  └─ zram 提供"虚拟 swap"，不写存储、不磨损 flash
     └─ 内存不足时，靠压缩撑住，而不是 OOM 杀关键进程
```

- **省内存**：压缩比 2~4 倍，512MB 内存能"当 1GB 用"
- **保护 flash**：不写 eMMC/UFS，延长存储寿命
- **实时性注意**：压缩/解压有 CPU 开销和延迟，实时任务数据不要放在 zram 里（配合 doc 08 mlock）

## 七、观测接口

### zram（启用后）

```sh
cat /sys/block/zram0/comp_algorithm   # 压缩算法（lz4/zstd/...）
cat /sys/block/zram0/orig_data_size   # 原始数据大小
cat /sys/block/zram0/compr_data_size  # 压缩后大小
cat /sys/block/zram0/mem_used_total   # 实际占用的内存
```

### zswap（启用后）

```sh
cat /sys/kernel/debug/zswap/pool_total_size   # 池大小
cat /sys/kernel/debug/zswap/stored_pages      # 已存页数
grep zswap /proc/vmstat                        # zswap 相关计数器
```

## 八、配置与实验（需重编内核）

当前内核 `CONFIG_ZSWAP` 和 `CONFIG_ZRAM` 都未开启。启用需要：

```
CONFIG_ZRAM=y           # zram 块设备
CONFIG_ZSMALLOC=y       # 压缩内存分配器（zram 依赖）
CONFIG_ZSWAP=y          # zswap 缓存
CONFIG_ZPOOL=y          # 压缩池（zswap 依赖）
CONFIG_FRONTSWAP=y      # 前端 swap 钩子（可选）
```

已记入待办，与 PSI/THP/DMABUF_HEAPS 一起，下次重编内核时补上。

启用后的实验：

```sh
# zram 实验
modprobe zram
echo lz4 > /sys/block/zram0/comp_algorithm
echo 128M > /sys/block/zram0/disksize   # 128MB 压缩 swap
mkswap /dev/zram0
swapon /dev/zram0
free   # 看到多了一个 128MB swap（但它在 RAM 里！）
```

## 九、陷阱

1. **CPU 开销**：压缩/解压要 CPU，实时任务数据别放 zram（doc 08 mlock 锁住）。
2. **压缩比非固定**：全是 0 的数据压得极狠，随机数据压不动。
3. **zram 和 zswap 二选一**：zram 是无磁盘方案，zswap 是有磁盘方案的优化层，不会同时用。
