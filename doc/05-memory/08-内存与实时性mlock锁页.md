# 内存与实时性：mlock 锁页

> 内存管理子系列第 8 篇（通往汽车实时 Linux 的桥梁）。前七篇讲完"分配→回收"完整闭环，本篇回答一个实时领域的关键问题：**为什么实时任务必须把内存锁死在物理内存里？** 核心机制是 `mlock`/`mlockall`，它把页标记为 unevictable，从此不被回收、不被换出、访问不再缺页。

## 一、实时性的天敌：三个"不可预测延迟"

实时任务（汽车制动控制、传感器融合）最怕的不是"慢"，而是"不可预测"。内存管理贡献了三大延迟源：

| 延迟源 | 触发时机 | 量级 |
|--------|---------|------|
| 缺页异常 minor fault | touch 未映射页，分配物理页 | 微秒~几十微秒 |
| 缺页异常 major fault | 访问被 swap 换出的页，读磁盘换入 | **毫秒~百毫秒** |
| TLB miss | 地址翻译 miss，遍历多级页表 | 几十纳秒~微秒 |

对汽车实时任务，一次 100ms 的 swap 换入延迟 = 一次制动失败的灾难。所以实时任务铁律：

```c
mlockall(MCL_CURRENT | MCL_FUTURE);   // 把所有内存锁死在物理内存
```

## 二、mlock 的两个动作

`mlock` 不只是"打个标记"，它做两件事（`do_mlock`，`mm/mlock.c:614`）：

```c
error = apply_vma_lock_flags(start, len, flags);   // ① 给 VMA 设 VM_LOCKED
...
error = __mm_populate(start, len, 0);              // ② 立即触发缺页，页物理驻留
```

- **① 标记**：`VM_LOCKED = 0x00002000`（`include/linux/mm.h:296`），告诉内核"这些页不可换出"。
- **② populate**：`__mm_populate` 主动 touch 每一页，把缺页"提前发生"。之后运行期访问这些页**不再缺页**——这才是实时性的关键。

`mlock` 系统调用入口（`mm/mlock.c:661`）：

```c
SYSCALL_DEFINE2(mlock, unsigned long, start, size_t, len)
{
	return do_mlock(start, len, VM_LOCKED);
}
```

## 三、锁定的页如何被回收绕开

### 1. 移到 unevictable LRU（`__mlock_folio`，`mm/mlock.c:61`）

```c
lruvec_del_folio(lruvec, folio);    // 从 active/inactive LRU 摘除
folio_clear_active(folio);
folio_set_unevictable(folio);       // 标记 PG_unevictable
lruvec_add_folio(lruvec, folio);    // 加入 unevictable
```

### 2. 回收扫描直接跳过（`folio_check_references`，`mm/vmscan.c:876`）

```c
/* The supposedly reclaimable folio was found to be in a VM_LOCKED vma. */
if (vm_flags & VM_LOCKED)
	return FOLIOREF_ACTIVATE;   // 不回收，保留
```

所以 kswapd 扫描到 mlocked 页直接放回，swap 也不会换出它。

## 四、mlockall 三参数（实时任务标准）

`mlockall`（`mm/mlock.c:747`）+ `apply_mlockall_flags`（`:697`）：

| 参数 | 作用 |
|------|------|
| `MCL_CURRENT` | 锁当前所有 VMA |
| `MCL_FUTURE` | 设 `mm->def_flags |= VM_LOCKED`，未来所有新分配自动锁 |
| `MCL_ONFAULT` | 不立即 populate，等 fault 时再锁（延迟锁）|

```c
SYSCALL_DEFINE1(mlockall, int, flags)
{
	...
	apply_mlockall_flags(flags);      // 设 def_flags + 锁现有 VMA
	...
	if (!ret && (flags & MCL_CURRENT))
		mm_populate(0, TASK_SIZE);    // 立即填充整个地址空间
}
```

**MCL_FUTURE 的生效机制**：新 VMA 从 `mm->def_flags` 继承 VM_LOCKED；缺页路径 `do_anonymous_page` → `folio_add_lru_vma` → `mm/swap.c:530`：

```c
if (unlikely((vma->vm_flags & (VM_LOCKED | VM_SPECIAL)) == VM_LOCKED))
	/* 新页直接标记 mlocked → unevictable */
```

## 五、限制与权限

`can_do_mlock`（`mm/mlock.c:40`）：

```c
bool can_do_mlock(void)
{
	if (rlimit(RLIMIT_MEMLOCK) != 0)   // 非零资源限制即可
		return true;
	if (capable(CAP_IPC_LOCK))         // root 或 CAP_IPC_LOCK 能力
		return true;
	return false;
}
```

- `do_mlock` 检查 `RLIMIT_MEMLOCK`（`rlimit`），超限且无 `CAP_IPC_LOCK` → EPERM。
- root 天然有 `CAP_IPC_LOCK`，可锁任意大小。
- 普通进程默认 `RLIMIT_MEMLOCK` 有限制（常见 8MB），需 `ulimit -l` 调大。

## 六、实验（mlock_demo）

编译部署了 `mlock_demo [MB] [mode]`：

| mode | 行为 |
|------|------|
| 0 | 只 touch 不锁（对照组）|
| 1 | `mlock` 指定范围 |
| 2 | `mlockall(MCL_CURRENT\|MCL_FUTURE)` |
| 3 | 先 `mlockall(MCL_FUTURE)` 再 malloc（验证后续分配也被锁）|
| 4 | `mlockall(MCL_FUTURE)` + malloc + free（验证 free 释放 mlocked 页）|

```sh
# ① 对照组：只 touch 不锁
mlock_demo 64 0 &
cat /proc/$!/status | grep VmLck      # VmLck: 0 kB（touch 了但没锁）

# ② 实验组：mlock 64MB
mlock_demo 64 1 &
cat /proc/$!/status | grep VmLck      # VmLck: 65540 kB（64MB + 4KB 页对齐）
cat /proc/meminfo | grep Unevictable  # Unevictable: 65540 kB（与 VmLck 严格相等）

# ③ 实时标准：mlockall(MCL_CURRENT|MCL_FUTURE)
mlock_demo 64 2 &
cat /proc/$!/status | grep VmLck      # VmLck: 66364 kB（64MB + 824KB 进程自身）

# ④ MCL_FUTURE：后续分配自动被锁（程序内置 print_vmlck，一键自动打印）
mlock_demo 64 3 &
# [after-malloc] VmLck:      65540 kB   ← malloc 的新内存已被锁

# ⑤ free 释放 mlocked 页
mlock_demo 64 4 &
# [locked ] VmLck:      65540 kB       ← free 前锁住
# [freed  ] VmLck:          0 kB       ← free 后归零
```

### 实测数据的六个关键点

| 现象 | 数值 | 结论 |
|------|------|------|
| mode=0 → VmLck=0 | 0 kB | touch 不锁，页仍是普通可回收页 |
| mode=1 → VmLck=Unevictable | 65540 kB 全等 | 锁页 = 移到 unevictable 的铁证 |
| 65540 − 65536 = 4KB | 一页 | `do_mlock` 的 `PAGE_ALIGN` 页对齐 |
| 66364 − 65540 = 824KB | 进程自身 | `MCL_CURRENT` 锁全部 VMA（代码+栈+堆），非只锁数据 |
| mode=3 → VmLck=65540 | 新分配即锁 | `mm->def_flags` 继承 `VM_LOCKED`，缺页路径自动 mlock |
| mode=4 → 65540→0 | free 即解锁 | VMA 销毁 → `VM_LOCKED` 随 VMA 消失 → 页回到可回收 LRU |

### mode=4 的深层机制：free 为什么能释放 mlocked 页

`mlock` 的锁**不是全局属性，而是挂在 VMA 上的 `VM_LOCKED` 标志**。`free()` → `munmap` → 内核 `exit_mmap`/`unmap_region` 销毁 VMA 时，页的 `VM_LOCKED` 身份随 VMA 一起消失。随后 `munlock_folio`（`mm/mlock.c`）把页从 unevictable LRU 摘回 active/inactive LRU：

```c
/* munlock 方向：VMA 没了，页不再有 VM_LOCKED 保护 */
folio_clear_unevictable(folio);
folio_set_active(folio);
lruvec_add_folio(lruvec, folio);   // 回到普通可回收 LRU
```

所以 **mlock 锁不住"主动释放"**——它只挡被动回收（kswapd、swap），挡不住进程自己 `free`。这也正是实时任务想要的行为：任务结束释放内存，锁随 VMA 自然解除，不留僵尸。

> 观察提示：上一条 `mlock_demo 64 3` 的进程（pid=124）还驻留在后台 `pause()`，所以 `cat /proc/meminfo | grep Unevictable` 显示 65540 kB——那是 mode=3 进程仍锁着 64MB，不是 mode=4 留下的。`kill 124` 后 Unevictable 会归零。

## 七、陷阱

1. **mlock 防的是"swap 抖动"，不是"OOM 免疫"**：mlocked 页仍计入 `oom_badness` 的 RSS，锁大内存的进程 OOM 时反而更容易被杀。
2. **mlock 有内存开销**：锁多少就占多少物理内存，锁太多会挤压其他进程，甚至触发 OOM。
3. **`mlock` 立即 populate**：大范围 mlock 会在启动时一次性触发大量缺页，有一次性延迟（`MCL_ONFAULT` 可延迟到 fault 时）。
4. **实时任务要 `MCL_CURRENT | MCL_FUTURE` 一起用**：只锁当前不够，运行中动态分配的内存也要锁。

## 八、衔接下一步

mlock 解决了"内存驻留"问题。接下来是 **THP 巨页**（减少 TLB miss）和 **cyclictest 实测抖动**，配合之前做的 CPU 隔离（isolcpus）+ PREEMPT_RT，构成完整的实时性技术栈：

```
isolcpus（CPU 隔离）→ sched_fifo（实时调度）→ mlockall（内存驻留）→ cyclictest（抖动测量）
```
