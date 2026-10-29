# zram 与 zswap 实战：从内核配置到压缩数据观测

> 内存管理子系列第 21 篇。doc 20 讲了理论，本篇是**动手实测**：重编内核开启 zram/zswap，在 QEMU 里亲眼看到压缩内存工作，并意外挖出"零页换出优化"这个隐藏机制。

## 一、内核重编：开启压缩内存

上一轮重编内核时，在 `scripts/build_in_container.sh` 的内核配置段加了：

```bash
"$KERNEL_DIR/scripts/config" --file "$KBUILD_DIR/.config" \
  -e ZRAM -e ZSMALLOC -e ZSWAP -e ZPOOL
```

重编后 `grep` 验证结果：

```
CONFIG_ZRAM=y
CONFIG_ZSMALLOC=y
CONFIG_ZSWAP=y
CONFIG_ZPOOL=y
```

同时开的还有 PSI、DMABUF_HEAPS。但**有两个配置没生效，且都是内核设计使然**（详见 doc 18/20）：

| 配置 | 结果 | 原因 |
|------|------|------|
| `TRANSPARENT_HUGEPAGE` | ❌ 未开 | `mm/Kconfig` 里 `depends on HAVE_ARCH_TRANSPARENT_HUGEPAGE && !PREEMPT_RT`，**THP 与 PREEMPT_RT 互斥**（khugepaged 压缩延迟破坏实时性） |
| `FRONTSWAP` | ❌ 不存在 | **6.12 已彻底移除**（`mm/frontswap.c` 已删），功能由 zswap 取代 |

> 教训：`scripts/config -e` 不会报错，`make olddefconfig` 会静默把满足不了依赖的选项删掉。验证配置要看 `.config`，不能只看脚本有没有写。

## 二、zram 实战：压缩内存盘

### 2.1 创建 zram 设备

```sh
cat /sys/class/zram-control/hot_add
# 1                    ← 返回新设备的编号
# dmesg: zram: Added device: zram1
```

**陷阱 1**：`hot_add` 是"**创建**"不是"查看"，每 `cat` 一次就新建一个设备，编号从 0 递增。这次返回 `1` 说明 zram0 已存在（之前 hot_add 过一次），新建的是 zram1。**不要反复 cat，会造出一堆闲置设备**。

### 2.2 设置容量并格式化为 swap

```sh
echo 64M > /sys/block/zram0/disksize
# dmesg: zram0: detected capacity change from 0 to 131072

mkswap /dev/zram0 && swapon /dev/zram0
# Adding 65532k swap on /dev/zram0.  Priority:-3

cat /proc/swaps
# /mnt/swapfile    file       262140   0   -2
# /dev/zram0       partition  65532    0   -3
```

**知识点**：zram 的本质是一个**压缩块设备**，格式化成 swap 后，换出的页压缩存进 RAM，**零磁盘 I/O**（doc 20 第二节）。

### 2.3 观测压缩效果

```sh
cat /sys/block/zram0/orig_data_size    # 原始数据大小
cat /sys/block/zram0/compr_data_size   # 压缩后大小
cat /sys/block/zram0/mem_used_total    # 实际占用内存
```

## 三、zswap 实战：压缩 swap 缓存

### 3.1 默认关闭，需手动开启

```sh
cat /sys/module/zswap/parameters/enabled
# N                              ← 编译进了，但默认不启用

echo 1 > /sys/module/zswap/parameters/enabled   # 开启
cat /sys/module/zswap/parameters/compressor     # lzo
cat /sys/module/zswap/parameters/zpool          # zsmalloc
```

原因：`.config` 里 `# CONFIG_ZSWAP_DEFAULT_ON is not set`。嵌入式惯例是默认保守、按需开启。

### 3.2 开启后，/proc/meminfo 出现新字段

开启前：

```
SwapCached:  0 kB
SwapTotal:   262140 kB
SwapFree:    262140 kB
```

开启后多出两行：

```
Zswap:       0 kB
Zswapped:    0 kB
```

这两个字段语义（`fs/proc/meminfo.c` 源码）：

```c
show_val_kb(m, "Zswap",    zswap_pool_total_size);           // 压缩后实际占的 RAM
show_val_kb(m, "Zswapped", zswap_stored_pages << PAGE_SHIFT); // 压缩前原始大小
```

### 3.3 用 mem_hog 逼出 swap

```sh
cat /proc/meminfo | grep -E "MemTotal|MemFree|MemAvailable"
# MemTotal: 476224 kB  MemFree: 441624 kB  MemAvailable: 438368 kB

mem_hog 500 1 &        # 分配 500MB 并 touch（数字 1！）
# pid=159 malloc=500MB vaddr=0xffff6616f010 touch=1
# touched all pages

cat /proc/meminfo | grep -iE "swap|zswap"
# SwapCached:  108 kB
# SwapTotal:   262140 kB
# SwapFree:    170028 kB      ← 掉了 92112 kB ≈ 90MB 换出
# Zswap:       6528 kB        ← 压缩后
# Zswapped:    8032 kB        ← 压缩前
```

**压缩率 = 8032 / 6528 ≈ 1.23x**，这些页被压缩进了 RAM，**没有写磁盘**。

### 3.4 陷阱 2：mem_hog 的 touch 参数必须是数字

```sh
mem_hog 400 touch &   # ❌ "touch" 是字符串！
# pid=154 malloc=400MB vaddr=... touch=0
```

`mem_hog.c` 里 `touch = atoi(argv[2])`，`atoi("touch")` 返回 **0**，于是跳过了 touch 循环——**400MB 只是 `malloc` 出的虚拟地址（VMA），一个物理页都没分配**，自然不会有 swap。这恰好复现了 `malloc 不触发缺页` 的原理（doc 04）。正确写法：`mem_hog 500 1`。

## 四、意外收获：零页换出优化

上面的数据有个矛盾：**SwapFree 掉了 90MB，但 Zswapped 只有 8MB**。剩下 82MB 去哪了？

看 `mem_hog.c` 的 touch 循环：

```c
for (i = 0; i < size; i += 4096)
    p[i] = (char)i;    /* i 永远是 4096 的倍数 */
```

`4096 % 256 == 0`，所以 `(char)i` 恒等于 **0**。匿名页 `malloc` 出来本来就是全零，于是 **500MB 全是全零页**。

全零页换出时，内核走**零页优化**：不在 swap 里存真实数据，只在 swap entry 里打个"这是零页"的标记；换入时直接映射到共享零页。**一字节都不实际存储**（doc 04 提过零页优化，这里亲眼看到）。

所以真实数据流是：

```
500MB 全零页 touch
  ├─ 82MB → 零页优化：标记 swap entry 为零页，不写 zswap/磁盘
  └─ 8MB  → 真正的非零页（内核/进程数据）→ zswap 压缩 8032→6528 kB
```

**结论**：这次实验一箭双雕——既验证了 zswap 压缩，又复现了零页换出优化。但也因此**没能展示 zswap 的真正压缩威力**（lzo 压可压缩数据能到 2~5 倍，全零页则被零页优化拦截了）。要测真实压缩率，需让 mem_hog 写**非零可压缩数据**（如全 `0xAB` 或递增字节）。

## 五、换入时：压缩数据要解压才能用

zswap 里存的是压缩数据。进程再次访问被换出的页时：

```
进程访问 *p → MMU 发现 PTE 是 swap entry
  → 缺页异常（主缺页）→ do_swap_page()
    → 查 zswap：zswap_load() 命中？
       ├─ 命中 → crypto_comp_decompress() 解压到新物理页（RAM + CPU，微秒级）
       └─ 未命中 → 从磁盘 swap 读回（毫秒级）
  → PTE 重新指向物理页 → 进程恢复，全程无感
```

源码落点：`mm/zswap.c::zswap_load()` → `crypto_comp_decompress()`（我们内核是 lzo）。

解压虽要 CPU，但远比磁盘 I/O 快一个数量级——**用 CPU 压缩/解压时间，换磁盘 I/O 大延迟**，这就是 zswap 值钱的地方。

## 六、陷阱总结

1. **`scripts/config -e` 静默失败**：依赖不满足的选项会被 `olddefconfig` 删掉，验证要看 `.config`。
2. **`hot_add` 是创建不是查看**：每 `cat` 一次新建一个 zram 设备，别反复 cat。
3. **zswap 默认关闭**：`CONFIG_ZSWAP_DEFAULT_ON` 未设，需 `echo 1 > .../enabled`，或改配置重编。
4. **zram 与 zswap 二选一**：zram 是无磁盘方案，zswap 是有磁盘方案的优化层，别同时开（实验时 `swapoff /dev/zram0` 隔离路径）。
5. **全零页不走 zswap**：零页优化会拦截，测 zswap 压缩率要用非零数据。
6. **解压有成本**：换入 zswap 页要 Buddy 分配新页 + 解压，实时系统里的延迟来源（doc 08 mlock 锁页）。

## 七、车载视角

车载 SoC 常无大磁盘或要保护 eMMC/UFS 寿命：

- **zram**：无磁盘场景的"虚拟 swap"，不写 flash、不磨损存储（Android/AAOS 标配）。
- **zswap**：有真实 swap 时的压缩缓存层，减少实际落盘 I/O。

两者都体现同一个思想：**用 CPU 压缩算力，换物理内存或磁盘 I/O 的节省**。
