# Writeback 写回：脏页的"落盘之路"

> 内存管理子系列第 12 篇（内存主线收尾篇）。doc 10 留了个谜：那 4MB 脏页躲过 `drop_caches` 后，最终是谁、什么时候写回磁盘的？答案就是本篇的 writeback 机制。讲完它，"分配 → 回收"的完整图景闭环。

## 一、概念：write 的另一半

`write()` 只把数据拷进页缓存、标记 `PG_dirty`，**不落盘**。真正写回磁盘的是独立的 writeback 机制：

```
write() → 数据进页缓存（标脏）    ← 快，写 RAM
   ↓
writeback → 脏页写回磁盘（落盘）  ← 慢，写盘，异步发生
```

`/proc/meminfo` 三个字段就是这段过程的快照：

| 字段 | 含义 |
|------|------|
| `Dirty` | 脏页（已改、还没写回）|
| `Writeback` | 正在写回磁盘的页 |
| `WritebackTmp` | 内存紧张时临时回收用的回写页 |

## 二、谁写回：per-bdi flusher

每个后备设备（bdi = backing device）有自己的 writeback 结构，由工作线程执行（`fs/fs-writeback.c` 的 `wb_workfn`）。脏 inode 挂在 `b_dirty` 链表上，flusher 逐个扫描、回写它们的脏页。

## 三、什么时候写回：五个触发条件

| 触发条件 | sysctl | 默认值 | 作用 |
|---------|--------|--------|------|
| **周期唤醒** | `dirty_writeback_centisecs` | 500（5 秒）| flusher 定期醒来看看有没有活 |
| **过期写回** | `dirty_expire_centisecs` | 3000（30 秒）| 脏页活过 30 秒，无论如何都写 |
| **后台阈值** | `dirty_background_ratio` | 10% | 脏页 > 阈值，flusher 主动后台写回 |
| **强阈值（节流）** | `dirty_ratio` | 20% | 脏页 > 阈值，**写进程自己停下来写** |
| **显式同步** | — | — | `sync` / `fsync` / `fdatasync` |

## 四、核心发现：脏页阈值是"动态"的

`dirty_ratio=20` 不是"总内存的 20%"。内核按**可脏化内存**（`global_dirtyable_memory`，`mm/page-writeback.c`）= 空闲页 + 可回收文件页 来计算，**匿名页不算**。

实测证据（`/proc/vmstat`）：

```
nr_dirty_background_threshold  1715 页  = 1715×4KB ≈ 6.8MB   （10% 阈值）
nr_dirty_threshold             3434 页  = 3434×4KB ≈ 13.7MB   （20% 阈值）
```

两个 `mem_hog`（各 300MB/200MB 匿名页）驻留后，可脏化内存只剩 ~67MB，于是 20%×67MB ≈ 13.7MB。**内存越紧张，脏页阈值缩得越小，内核越早写回**——防止脏页把本就紧张的内存占满。

## 五、源码关键点

- **节流点** `balance_dirty_pages_ratelimited`（`mm/page-writeback.c`）：写路径每轮都问"脏页超阈值了吗？超了我自己写回"。超过 `dirty_ratio` 时写进程被节流，这就是"写满磁盘/写太猛时机器变卡"的根源。
- **实际回写** `wb_writeback → writeback_sb_inodes`（`fs/fs-writeback.c`）：扫描脏 inode，把 `PAGECACHE_TAG_DIRTY` 标记的页写盘，改成 `PAGECACHE_TAG_WRITEBACK`。

## 六、实验验证

### 实验 1：默认阈值，Dirty 涨不起来

```sh
dd if=/dev/zero of=/mnt/big2 bs=1M count=100
cat /proc/meminfo | grep -E "^Dirty|^Writeback"
# Dirty: 24 kB    ← 脏页刚过 6.8MB 后台阈值就被刷掉，100MB 写完只剩 24kB
```

### 实验 2：调高阈值，Dirty 憋住堆积

```sh
echo 90 > /proc/sys/vm/dirty_background_ratio
echo 90 > /proc/sys/vm/dirty_ratio
dd if=/dev/zero of=/mnt/big3 bs=1M count=100
cat /proc/meminfo | grep -E "^Dirty|^Writeback"
# Dirty: 28672 kB  ← 阈值 90% 才写回，脏页堆积到 28MB
sync
cat /proc/meminfo | grep -E "^Dirty|^Writeback"
# Dirty: 0 kB      ← sync 强制刷盘
echo 10 > /proc/sys/vm/dirty_background_ratio   # 恢复默认
echo 20 > /proc/sys/vm/dirty_ratio
```

### 对照结论

| 场景 | Dirty 峰值 | 原因 |
|------|-----------|------|
| 默认阈值（10/20）| 24 kB | 脏页 > 6.8MB 就刷 |
| 调高阈值（90/90）| **28672 kB** | 脏页憋到 ~90% 才刷 |
| `sync` | 0 | 显式强制刷盘 |

## 七、陷阱

1. **`write()` 返回 ≠ 数据安全**：数据还在 RAM，断电就丢。数据库每次事务 `fsync` 才敢说"已提交"。
2. **`sync` 全局刷盘，`fsync(fd)` 只刷一个文件**——后者才是数据库的正确姿势。
3. **`dirty_ratio` 是"刹车"不是"目标"**：超过 20% 写进程被节流，系统变慢。
4. **阈值动态收缩**：内存紧张时脏页阈值变小，写回更频繁——这是保护机制，不是 bug。

## 八、内存主线闭环总结

至此，Linux 内存管理"分配 → 回收"的完整图景拼齐了：

```
分配：Buddy(01) → SLUB(02) → 页表(03) → 缺页(04)
      ├─ 匿名页：malloc → 缺页 → 匿名页
      └─ 文件页：read/write → 页缓存(10)
回收：水位线/kswapd(06) → 文件页：干净丢弃 / 脏页写回(12)
                         匿名页：swap 换出(11) → 换入(11)
      OOM(07) 兜底
实时：mlock 锁页(08) 阻止换出/回收，cyclictest(09) 测量
```

一张图收口：

```
                       ┌── 干净文件页 ── 直接丢弃（白捡）
        内存压力 ──►  ├── 脏文件页 ──── Writeback 写回原文件
   （kswapd 醒来）    ├── 匿名页 ────── rmap 清 PTE → swap 换出
                       └── 都救不了 ─── OOM Killer 杀进程
```
