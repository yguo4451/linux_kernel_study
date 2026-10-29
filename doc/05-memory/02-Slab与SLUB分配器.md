# Slab / SLUB 分配器：内核小对象的"批发+零售"

> 内存管理子系列第 2 篇。上一篇 Buddy 管物理页（4KB 的 2 的幂），本篇讲 slub 怎么在 Buddy 之上高效管理成千上万的**固定大小小对象**。我们内核用的是 `CONFIG_SLUB=y`（6.12 默认），所以以 SLUB 为准。

## 一、为什么需要 slab：Buddy 的浪费问题

内核里有海量的固定大小结构体：

```c
struct task_struct  ~ 数 KB
struct inode        ~ 数百 B
struct sk_buff      ~ 数百 B
```

如果每次创建 `inode` 都直接找 Buddy 要一个 4KB 页：

1. **内部碎片**：200 字节的对象独占 4KB 页 → 95% 浪费
2. **频繁往返 Buddy**：每次分配/释放都要进全局页分配器，慢且争锁
3. **加剧碎片**：小对象散落在各个页里，破坏 Buddy 的大块

**slab/slub 的解法**：向 Buddy **批发**一整页（甚至多页），自己**零售**成固定大小的对象，每个对象带一个"空闲指针"串成链表复用。这就是"对象缓存"。

## 二、核心数据结构（数据优先）

### 1. struct kmem_cache —— 一种"型号"的缓存

```c
// mm/slab.h:258
struct kmem_cache {
	struct kmem_cache_cpu __percpu *cpu_slab;  // 每 CPU 缓存
	slab_flags_t flags;
	unsigned int size;          // 对象大小（含元数据）
	unsigned int object_size;   // 对象大小（不含元数据）
	unsigned int offset;        // 空闲指针在对象内的偏移
	struct kmem_cache_order_objects oo;  // 打包了"阶数 + 每 slab 对象数"
	const char *name;           // 缓存名（如 "kmalloc-256"）
	struct kmem_cache_node *node[MAX_NUMNODES];  // 每 NUMA 节点一份
};
```

**每种对象类型一个 kmem_cache**：`task_struct`、`inode`、`dentry`、`sk_buff` 各有一个，加上 kmalloc 的 8/16/32/.../8192 一整套尺寸档。

### 2. 三级缓存结构（slub 的灵魂）

```
kmem_cache
 ├─ cpu_slab（每 CPU 一个 kmem_cache_cpu）       ★ 快路径，无锁
 │     ├─ freelist   指向下一个空闲对象
 │     ├─ slab       当前正在分配的那个 slab
 │     └─ partial    部分空 slab 列表
 └─ node[0]（每 NUMA 节点一个 kmem_cache_node）   ★ 慢路径，加锁
       └─ partial    部分空 slab 链表
             ↓ 都空
        Buddy System 要新页（new_slab）
```

```c
// mm/slub.c:387
struct kmem_cache_cpu {
	void **freelist;      // 下一个可用对象（锁无关快路径）
	unsigned long tid;    // 事务 ID（配合 cmpxchg 做无锁）
	struct slab *slab;    // 当前 slab
	struct slab *partial; // 部分空 slab
};

// mm/slub.c:428
struct kmem_cache_node {
	spinlock_t list_lock;
	unsigned long nr_partial;
	struct list_head partial;   // 部分空 slab 链表
};
```

## 三、一个 slab 内部长什么样

```
slab（一个/多个物理页，被切成 N 个等大对象）
+--------+--------+--------+--------+
| obj[0] | obj[1] | obj[2] | obj[3] |   ← 每个对象大小 = cache->size
+--------+--------+--------+--------+
   ↑
freelist：空闲对象用"对象内的下一个指针"串成单向链表
   释放 = 头插法入链；分配 = 头删法出链
```

关键点：**空闲链表不额外占内存**，它复用对象本身的空间存"下一个空闲对象"的指针（`offset` 记录指针在对象内的位置）。对象被分配出去后，这段空间就归用户数据用。

## 四、分配路径：快路径 + 慢路径

```
kmem_cache_alloc(s, flags)
  → slab_alloc_node                          mm/slub.c:4205
    → __slab_alloc_node                       mm/slub.c:4085
        ┌─ 快路径：per-CPU freelist 非空
        │    object = c->freelist
        │    cmpxchg 把 freelist 前进到下一个对象   ← 无锁，极快
        │
        └─ 慢路径：freelist 空了
             → __slab_alloc → ___slab_alloc     mm/slub.c:3737
                 ├─ get_partial：从 node/CPU 的 partial 链表拿一个部分空 slab
                 └─ new_slab → allocate_slab    mm/slub.c:2627
                       → alloc_slab_page → Buddy 拿新页
                       → 把页切成对象 + 建 freelist
```

**为什么快**：绝大多数分配只走 `cmpxchg` 一步（`__update_cpu_freelist_fast`），不需要任何锁，甚至不需要关中断——这正是 SLUB 相比老 slab 的核心优化。

## 五、释放路径：同样是快/慢

```
kmem_cache_free(s, x)                          mm/slub.c:4785
  → slab_free → do_slab_free                   mm/slub.c:4670
       ┌─ 快路径：x 属于当前 CPU 的 slab
       │    把 x 头插回 freelist（cmpxchg，无锁）
       └─ 慢路径：x 不属于当前 slab
             → __slab_free
                 ├─ slab 变成全空 → 太多就 discard_slab 还回 Buddy
                 └─ 否则挂回 node 的 partial 链表
```

**一个反直觉的点**：释放**不会立刻把内存还给 Buddy**。对象先回 freelist 复用，整个 slab 全空后还要看 `min_partial` 阈值，留一部分空 slab 缓存着，避免下次又要向 Buddy 要新页。

## 六、kmalloc 的尺寸分档

用户态 `malloc` 对应内核态 `kmalloc(size, flags)`：

```c
// mm/slub.c:4363
s = kmalloc_slab(size, b, flags, caller);   // 按 size 选一档缓存
```

内核预置了一整套尺寸档：`kmalloc-8`、`kmalloc-16`、`kmalloc-32`、...、`kmalloc-8192`。`kmalloc(100)` 会落到 `kmalloc-128`（向上取 2 的幂）。

**大分配绕过 slab**：`size > KMALLOC_MAX_CACHE_SIZE`（约 8KB）时，`___kmalloc_large_node` 直接调 `alloc_pages` 走 Buddy——因为大对象用 slab 零售没意义，直接按页分配。

## 七、动手实验（QEMU 内）

### 实验 1：看所有对象缓存

```sh
cat /proc/slabinfo | head -20
# slabinfo - version: 2.1
# name       <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> ...
```

字段含义（挑关键的）：

| 字段 | 含义 |
|------|------|
| `active_objs` | 正在使用中的对象数 |
| `num_objs` | 已创建的对象总数（含空闲）|
| `objsize` | 每个对象大小（字节）|
| `objperslab` | 每个 slab 能放几个对象 |
| `pagesperslab` | 每个 slab 占几页 |

### 实验 2：找内核大牌对象

```sh
grep -E "task_struct|inode_cache|dentry|skbuff|kmalloc-256" /proc/slabinfo
# 看 task_struct 的 objsize、active_objs
```

### 实验 3：观察对象复用（释放不还 Buddy）

```sh
cat /proc/slabinfo | grep kmalloc-256    # 记下 active_objs
# 跑点活，再看 active_objs 变化；释放后 num_objs 通常不降（缓存着）
```

## 八、slab 与之前模块的联系

```
kmalloc(100)
  → kmalloc-128 缓存
    → freelist 有货 → 直接给（快路径）
    → freelist 空 → new_slab → allocate_slab
        → alloc_slab_page → Buddy 拿页（回到模块 1）
```

slab 的对象属于 **MIGRATE_UNMOVABLE**（模块 1 的迁移类型）——因为内核对象钉死在原处，不能搬。这就是为什么 `pagetypeinfo` 里 Unmovable 类型的 order10 大块永远是 0（slab 都是小块）。

## 关键源码索引

| 功能 | 位置 |
|------|------|
| `struct kmem_cache` | `mm/slab.h:258` |
| `struct kmem_cache_cpu`（每 CPU）| `mm/slub.c:387` |
| `struct kmem_cache_node`（每节点）| `mm/slub.c:428` |
| `allocate_slab`（向 Buddy 要页）| `mm/slub.c:2627` |
| `new_slab` | `mm/slub.c:2695` |
| `___slab_alloc`（慢路径）| `mm/slub.c:3737` |
| `__slab_alloc_node`（快路径）| `mm/slub.c:4085` |
| `slab_alloc_node`（分配入口）| `mm/slub.c:4205` |
| `kmalloc_slab`（选尺寸档）| `mm/slub.c:4363` |
| `do_slab_free`（释放快路径）| `mm/slub.c:4670` |
| `kmem_cache_free`（释放入口）| `mm/slub.c:4785` |
