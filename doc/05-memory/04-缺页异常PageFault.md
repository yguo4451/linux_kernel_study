# 缺页异常 Page Fault：malloc 不分配物理页的真相

> 内存管理子系列第 4 篇（重点）。前面铺了 Buddy（物理页）、SLUB（小对象）、页表（翻译），本篇把它们串起来：**进程第一次 touch 一块虚拟地址时，发生了什么。**

## 一、什么是缺页异常

访问一个虚拟地址时，MMU 查页表发现 **PTE 无效（`PTE_VALID=0`）或中间级页表不存在**，硬件就触发一个异常（arm64 上叫 **Data Abort**），把控制权交给内核处理——这就是**缺页异常（page fault）**。

```
CPU 访问 VA
  → MMU 查 TLB（未命中）
    → 查页表：PTE_VALID = 0   ← 页还没映射！
      → 硬件触发 Data Abort
        → 内核缺页处理程序接管
```

## 二、完整调用链（arm64）

```
硬件异常（EL0 数据访问 abort）
  → do_mem_abort                     arch/arm64/mm/fault.c（汇编入口）
    → do_page_fault                  arch/arm64/mm/fault.c:526
        ├─ 查 fault_info 表，识别故障类型
        ├─ 用户态 + 地址合法 → handle_mm_fault
        └─ 地址非法 → do_bad_area → 发 SIGSEGV（段错误）
          → handle_mm_fault           mm/memory.c:6070
            → __handle_mm_fault       mm/memory.c:5839
                ├─ 逐级补页表：pgd → p4d → pud → pmd
                └─ handle_pte_fault   mm/memory.c:5755
                      ├─ pte_none          → do_pte_missing（页不存在）
                      ├─ !pte_present      → do_swap_page（换入）
                      ├─ 写只读页           → do_wp_page（写时复制 COW）
                      └─ 匿名页             → do_anonymous_page
```

## 三、四种缺页类型

| 类型 | 触发场景 | 处理函数 | 干什么 |
|------|---------|---------|--------|
| **匿名页缺页** | `malloc` 后首次 touch | `do_anonymous_page` | 从 Buddy 分配新页（写）或映射零页（读）|
| **文件映射缺页** | `mmap` 文件后首次读 | `do_fault` → `filemap_fault` | 从页缓存/磁盘读入页 |
| **写时复制 COW** | `fork` 后写父进程共享页 | `do_wp_page` | 复制一份新页再写 |
| **换入** | 访问被 swap 出去的页 | `do_swap_page` | 从 swap 区读回 |

## 四、核心代码：do_anonymous_page（匿名页缺页）

```c
// mm/memory.c:4735
static vm_fault_t do_anonymous_page(struct vm_fault *vmf)
{
	...
	/* 读缺页：用 zero page，不分配物理页！ */
	if (!(vmf->flags & FAULT_FLAG_WRITE) && !mm_forbids_zeropage(...)) {
		entry = pte_mkspecial(pfn_pte(my_zero_pfn(vmf->address), ...));
		...
		goto setpte;     // 映射到全局共享的零页
	}

	/* 写缺页：才真正分配物理页 */
	folio = alloc_anon_folio(vmf);      // ← 从 Buddy 要页（回到模块 1）
	...
	entry = mk_pte(&folio->page, vma->vm_page_prot);
	...
	set_ptes(vma->vm_mm, addr, vmf->pte, entry, nr_pages);  // 填 PTE
}
```

**两个关键点**：

1. **读缺页用零页**：只读一个匿名页（如 `mem_hog touch=0` 时根本没碰，或只读不写），内核映射到**全局共享的 zero page**，不分配新物理页。等真的写入时，才分配自己的页（COW 零页）。
2. **写缺页才分配**：`alloc_anon_folio` → Buddy 分配器（模块 1 的 `alloc_pages`），然后 `set_ptes` 把 PTE 从"无效"改成"指向新页"。

## 五、缺页 vs 段错误：VMA 是裁判

关键问题：访问一个没映射的地址，**什么时候是"合法的缺页"（该分配），什么时候是"段错误"（该杀）**？

答案在 `do_page_fault`：先查进程的 **VMA**（模块 3）：

```
访问 VA，页表无效
  → 查 mm 的 VMA 树，看 VA 落在哪个 VMA 里？
      ├─ 落在合法 VMA 内，且权限匹配 → 合法缺页 → handle_mm_fault
      └─ 不在任何 VMA / 权限不匹配 → do_bad_area → SIGSEGV（段错误）
```

**这就是 VMA 和页表分工的体现**（模块 3 埋的伏笔）：

- **VMA** = 软件层的"承诺"（这段地址**应该**能访问，只是还没映射）
- **页表** = 硬件层的"现实"（这段地址**实际**映射到哪）

缺页 = "承诺了但还没兑现" → 内核兑现承诺（分配页）；段错误 = "从没承诺过" → 直接杀。

## 六、动手实验：malloc 不分配，touch 才分配

`mem_hog` 工具（`experiments/memory/mem_hog.c`）就是为验证这一点：

```sh
# ① 只 malloc 不 touch：只建了 VMA，没触发写缺页 → buddyinfo 几乎不变
mem_hog 200 0 &

# ② 逐页 touch：每页触发一次写缺页 → 真正分配 200MB 物理页
mem_hog 200 1 &
```

```sh
# 对比前后 buddyinfo 的 order 分布
cat /proc/buddyinfo
```

**预期**：

| 操作 | VMA | 物理页 | buddyinfo |
|------|-----|--------|-----------|
| `malloc 200MB`（不 touch）| ✅ 建立 | ❌ 0 页 | 几乎不变 |
| `touch` 每一页 | ✅ 已有 | ✅ 200MB | 明显下降 |

## 七、缺页对实时系统的影响（承上启下）

缺页异常是**不可预测的延迟来源**：

- 实时任务运行时，如果 touch 到一块没映射的内存 → 触发缺页 → 内核去 Buddy 分配、甚至触发磁盘 I/O（文件缺页）→ **延迟瞬间飙到毫秒级**
- 这就是为什么汽车实时 Linux 要 `mlockall` + 预 touch 所有内存（模块 5 之后会讲"实时内存三件套"）

`cyclictest` 里偶尔出现的 Max 值尖刺，很多就是缺页/回收造成的。

## 关键源码索引

| 功能 | 位置 |
|------|------|
| `do_page_fault`（arm64 主处理）| `arch/arm64/mm/fault.c:526` |
| `do_bad_area`（段错误）| `arch/arm64/mm/fault.c:476` |
| fault_info 表（故障分类）| `arch/arm64/mm/fault.c:823` |
| `handle_mm_fault`（通用入口）| `mm/memory.c:6070` |
| `__handle_mm_fault`（逐级补页表）| `mm/memory.c:5839` |
| `handle_pte_fault`（PTE 分派）| `mm/memory.c:5755` |
| `do_anonymous_page`（匿名页缺页）| `mm/memory.c:4735` |
| `do_wp_page`（写时复制）| `mm/memory.c:3655` |
| `do_swap_page`（换入）| `mm/memory.c:4200` |
| `do_fault`（文件缺页）| `mm/memory.c:5424` |
