# 反向映射 rmap 与 swap 换出

> 内存管理子系列第 11 篇。上一篇讲了页缓存（文件页有"家"），本篇讲**匿名页（没家）怎么被 swap 换出**——以及换出时最关键的一步：内核如何用 **rmap（反向映射）** 找到所有引用这一页的页表项，把它们逐个摘掉。

## 一、核心概念：正向映射 vs 反向映射

| | 方向 | 视角 | 数据结构 |
|---|------|------|---------|
| **正向映射** | 虚拟地址 → 物理页 | 进程访问内存（CPU 走页表）| 页表（PTE）|
| **反向映射 rmap** | 物理页 → 所有引用它的 VMA/PTE | 内核回收/迁移（要摘掉所有映射）| `anon_vma` / `i_mmap` |

正向映射只回答"这个虚拟地址对应哪一页"。但内核要**换出/迁移某一页**时，问的是反过来的问题：**"这一页被谁映射着？我要去清掉谁的 PTE？"**——这就是 rmap 存在的意义。没有它，内核根本不知道一页被几个进程、几个 VMA 共享。

## 二、rmap 的数据结构（匿名页）

匿名页的 rmap 核心是 `struct anon_vma`（`include/linux/rmap.h`）：

```c
struct anon_vma {
    struct anon_vma *root;
    struct rw_semaphore rwsem;
    atomic_t refcount;
    unsigned long degree;
    struct anon_vma *parent;
    struct rb_root_cached rb_root;   // anon_vma_chain 红黑树，按 VMA 区间排序
};
```

关键识别位：`page->mapping` 这个字段**一石二鸟**：

- 指向 `struct address_space` → 这是**文件页**（页缓存，上一篇）
- 指向 `struct anon_vma`（低 2 位带 `PAGE_MAPPING_ANON` 标志）→ 这是**匿名页**

于是内核拿到任意一页，先看 `page->mapping` 就知道它是"有家的文件页"还是"没家的匿名页"，再决定走哪条回收路。

## 三、swap 换出的三步（源码链路）

内存压力下，kswapd 醒来后扫描匿名页 LRU，对"不活跃"的匿名页执行（`mm/vmscan.c`）：

```
balance_pgdat → shrink_node → shrink_lruvec
  → shrink_inactive_list → shrink_folio_list(folio)
      │
      ├─ ① add_to_swap(folio)          // 分配一个 swap 条目（swap 类型+偏移）
      ├─ ② try_to_unmap(folio)         // ★ rmap：清掉所有引用它的 PTE
      ├─ ③ pageout(folio)              // 把页内容写到 swap 区域
      └─ ④ folio 还给 Buddy 分配器      // 物理页释放
```

### ① 分配 swap 条目

`add_to_swap` → `get_swap_page` 在 swap 区域里找一个空闲槽位，生成 `swp_entry_t`（`include/linux/swapops.h`）——一个 64 位值，编码了"哪个 swap 设备 + 设备内哪个偏移"。**这个条目之后被写进 PTE，是换回时的"寻宝图"**。

### ② rmap 清映射（核心，`mm/rmap.c`）

```c
try_to_unmap(folio)
  → rmap_walk(folio) → rmap_walk_anon(folio)
      → anon_vma_interval_tree_foreach(...)      // 遍历所有映射该页的 VMA
          → try_to_unmap_one(vma, folio)          // 逐个清 PTE
              → 清掉 PTE，把 swap entry 写进 PTE 的位置
```

想象一个匿名页被 3 个进程共享（如 fork 后的 COW 页），换出时必须**把这 3 个进程的页表里指向它的 PTE 全部改成 swap entry**，否则某个进程访问时还以为页在内存里。`try_to_unmap_one` 就是干这个的。

### ③④ 写 swap + 释放物理页

`pageout` → `swap_writepage`（`mm/page_io.c`）把页数据写入 swap 设备；成功后物理页还给 Buddy。此后该页在内存里消失，只在 swap 里留一份，PTE 里存着 swap entry。

## 四、实验验证

环境：512MB 内存 + 256MB swapfile（`swapon /mnt/swapfile`），`echo 100 > /proc/sys/vm/swappiness`（积极换出）。

```sh
mem_hog 300 &     # pid 141：分配 300MB 匿名页并 touch
free              # Swap used 还是 0
mem_hog 200 &     # pid 143：再分配 200MB（共 500MB > 476MB 物理内存）
free              # Swap used 涨到 83MB
cat /proc/141/status | grep VmSwap   # 第一个 hog：VmSwap 73036 kB
cat /proc/143/status | grep VmSwap   # 第二个 hog：VmSwap 0 kB
```

### 实测数据

| 观测点 | 第二次 mem_hog 前 | 第二次 mem_hog 后 | 解读 |
|--------|------------------|------------------|------|
| `free` used | 327076 | 458304 | 多用了 ~128MB |
| `free` buff/cache | 143312 | **7048** | 干净文件页被先回收 |
| `free` Swap used | 0 | **83336** | 匿名页被换出 |
| pid 141 `VmSwap` | — | **73036** | 第一个 hog 被换出 71MB |
| pid 143 `VmSwap` | — | **0** | 第二个 hog 分毫未换 |
| `SwapCached` | — | 1132 | 少量共享页换出后回读 |

### 三个关键结论

1. **回收顺序印证了"成本排序"**：第二次 mem_hog 要 200MB，内核**先回收干净文件页**（`buff/cache` 从 143MB 掉到 7MB——上一篇 dd 写 swapfile 留下的缓存），**再换出匿名页**（Swap used 涨 83MB）。顺序正是：`干净文件页（白捡）→ 匿名页（写 swap）`。

2. **LRU 受害者是"最久没被碰"的页**：pid 141 先 touch、驻留最久，它的页在匿名 LRU 链表里"最冷"；pid 143 刚 touch 完、是"热"页。所以被换出的是 **pid 141（VmSwap 73MB），pid 143 分毫未换（0）**。这就是匿名页 LRU 的"冷热"判定。

3. **VmSwap 是进程级的换出证据**：`VmSwap` 字段（`/proc/PID/status`）精确统计了每个进程有多少页躺在 swap 里。

## 五、换入（swap-in）：缺页的另一半

被换出的页，PTE 里存的是 swap entry。下次进程访问该地址：

```
访问 PTE=swap entry → 缺页异常 → do_swap_page (mm/memory.c)
  → swap_read_folio (mm/page_io.c)     // 按 swap entry 从 swap 读回
  → 重新建立 PTE，页回到内存（Swap used 下降）
```

这是**主缺页（major fault）**，要读 swap 设备，比普通缺页慢得多——这就是 doc 08 说的实时任务必须 `mlock` 锁页的原因：**防止热数据被换出，避免运行时再来一次主缺页**。

### 实测：换入耗时 + 换页抖动

用带 `retouch` 模式的 mem_hog（`mem_hog 300 1 20`：touch 后睡 20 秒再重读全部页）：

```sh
mem_hog 300 1 20 &   # pid 140：300MB，touch 后睡 20s 再重读
mem_hog 200 &        # pid 141：逼内核换出 pid140
free                 # Swap used 86424（pid140 被换出 ~84MB）
# 20 秒后 pid140 醒来：
# re-read done: 300MB in 162.68 ms   ← 重读 300MB 耗时 162ms
cat /proc/140/status | grep VmSwap  # VmSwap: 8 kB（页回来了）
cat /proc/141/status | grep VmSwap  # VmSwap: 71376 kB（pid141 被换出去了！）
free                 # Swap used 134640（反而涨了）
```

**两个关键结论**：

1. **换入是昂贵的**：重读 300MB 花了 **162.68 ms**。其中大部分是主缺页从 swap 读回 ~84MB 的 I/O 成本。对比首次 touch（分配零页 + 写，快得多），换入要"读 swap 设备"。这 162ms 的不可预测延迟，就是实时任务必须 `mlock` 锁页的直接证据。

2. **换页抖动（thrashing）**：pid140 的 84MB 回来了，但代价是 pid141 的 71MB 被换出去（`VmSwap` 0→71376），总 `Swap used` 反而从 86MB 涨到 134MB。两个进程各要 300MB + 200MB = 500MB，超出 476MB 物理内存，**谁活跃谁换回、谁冷谁换出**，来回倒腾。这就是系统"卡死"的微观现场——内存不够时，内核在 swap 上疲于奔命。

## 六、陷阱

1. **swap 不是"内存的延伸"那么简单**：换出/换入都要读写磁盘，频繁换入换出会导致系统卡死（**thrashing 抖动**）。swappiness 默认 60 是折中。
2. **SwapCached 的坑**：共享页换出后被一个进程读回，会同时留在 swap（因为别的进程可能还需要）和 RAM 的 swap cache 里，直到不再被引用才释放 swap 槽位。
3. **rmap 是全局开销**：每个匿名页的换出都要遍历 anon_vma 红黑树，所以大规模换出很耗 CPU。

## 七、衔接下一步

swap 换出/换入讲完了"匿名页的逃亡之路"。但还有一条主线没收尾：**那些脏文件页到底是谁、什么时候写回磁盘的**——下一篇 **Writeback 写回**，把"页缓存 → 脏页 → 回写"这条链闭环，届时"分配→回收"的完整图景就拼齐了。
