# OOM Killer：内存耗尽时的最后裁决

> 内存管理子系列第 7 篇（模块 5 收官）。前一篇讲了 kswapd 在水位线下回收，本篇讲回收失败后的最后手段：**OOM Killer**。核心结论一句话：**它杀的是"最肥的"进程，不是"最后喊饿的"进程。**

## 一、现象：一条调用栈串起全部模块

`mem_hog 200 1`（touch 200MB）在 free 只剩 129MB 时触发 OOM，dmesg 完整栈：

```
el0_da                          ← 用户态 touch → Data Abort（硬件）
  → do_mem_abort → do_translation_fault → do_page_fault      【模块4：缺页】
    → handle_mm_fault → __handle_mm_fault
      → vma_alloc_zeroed_movable_folio（do_anonymous_page）   【模块4：匿名页】
        → vma_alloc_folio → alloc_pages_mpol → __alloc_pages  【模块1：Buddy】
          → out_of_memory → oom_kill_process                 【模块5：OOM】
```

`gfp_mask=0x140dca(GFP_HIGHUSER_MOVABLE|__GFP_COMP|__GFP_ZERO)` 正是 `do_anonymous_page` 里 `vma_alloc_zeroed_movable_folio` 的分配标志。

## 二、核心：触发者 ≠ 被杀者

- **触发 OOM**：pid 141（touch 到一半，128MB 时分配失败）
- **被杀**：pid 138（已经吃了 300MB）

Tasks 表（单位页）：

| pid | total_vm | rss(页) | 实占 | 说明 |
|-----|----------|---------|------|------|
| 125 | 256211 (1GB) | 98 | 0.4MB | `1000 0`，只建 VMA |
| **138** | 77011 | **76898** | **300MB** | `300 1` ← **被杀** |
| 141 | 51411 | 32706 | 128MB | `200 1`，触发 OOM |

## 三、源码：oom_badness 打分（`mm/oom_kill.c:203`）

```c
long oom_badness(struct task_struct *p, unsigned long totalpages)
{
	if (oom_unkillable_task(p))          // init/内核线程 → 不杀
		return LONG_MIN;

	adj = (long)p->signal->oom_score_adj;
	if (adj == OOM_SCORE_ADJ_MIN)        // -1000 → 永不杀
		return LONG_MIN;

	points = get_mm_rss(p->mm) + MM_SWAPENTS + mm_pgtables_bytes/页大小;
	adj *= totalpages / 1000;            // oom_score_adj 平移分数
	points += adj;
	return points;                       // 分最高者被杀
}
```

- 基础分 = RSS + swap 占用 + 页表 → **谁吃得多谁分高**
- `oom_score_adj=-1000` 直接 `LONG_MIN` → 受保护不杀
- pid 138 RSS=76898 页全场最高 → 被杀；pid 141 仅 32706 页 → 逃过

杀进程时（`mm/oom_kill.c:955`）：

```c
do_send_sig_info(SIGKILL, SEND_SIG_PRIV, victim, PIDTYPE_TGID);
pr_err("%s: Killed process %d (%s) total-vm:... anon-rss:... oom_score_adj:%hd\n", ...);
```

对应现场输出：`Out of memory: Killed process 138 (mem_hog) total-vm:308044kB, anon-rss:307200kB, ...`

## 四、为什么回收救不回：Mem-Info 解读

```
Node 0 DMA free:2068kB min:2712kB low:3388kB high:4064kB
                        ↑ free < min，最危险一档
all_unreclaimable? yes   ← 无缓存可回收、无 swap 可换
Free swap = 0kB / Total swap = 0kB
Node 0 DMA: 4*4kB 4*8kB ... 0*2048kB 0*4096kB = 2048kB   ← Buddy 碎片化，大块清零
```

回收两板斧（页缓存/swap 换出）都失效 → 只能 OOM。

## 五、OOM 决策流程

```
out_of_memory (mm/oom_kill.c:1113)
  → select_bad_process（遍历所有进程，oom_badness 取最高分）
  → oom_kill_process → do_send_sig_info(SIGKILL)
```

日志关键行：

```
oom-kill:constraint=CONSTRAINT_NONE,...,global_oom,task=mem_hog,pid=138,uid=0
Out of memory: Killed process 138 (mem_hog) ...
```

`CONSTRAINT_NONE` 表示全局 OOM（非 cgroup/CPUSET 限制）。

## 六、实验记录

| 操作 | 结果 |
|------|------|
| `mem_hog 300 1 &` | 成功，吃 300MB（pid 138）|
| `mem_hog 200 1 &` | touch 到 128MB 时分配失败，触发 OOM（pid 141）|
| OOM 裁决 | 杀 pid 138（RSS 最高），放行 pid 141 |
| 杀后 | pid 141 继续 touch 完剩余页，系统恢复 |

## 七、陷阱

1. **OOM 杀最高 RSS，不是最后触发者**——`oom_badness` 比的是存量不是增量。
2. **`oom_score_adj=-1000` 永不杀**，`-1~-999` 降低被杀概率，正数增加。
3. **`all_unreclaimable? yes` 是关键信号**：回收彻底失败才 OOM；若有 swap，会先疯狂换出，不一定走到 OOM。
4. **free < min 才触发 direct reclaim/OOM**，不是 free=0 才触发。

## 八、模块 5 总结（内存回收闭环）

```
分配失败（free < low）→ 唤醒 kswapd → 回收页缓存/换出匿名页
   → free ≥ high → kswapd 睡（平衡恢复）
   → 回收无效（无缓存无 swap）→ free < min → direct reclaim 也失败
   → out_of_memory → oom_badness 打分 → 杀最高 RSS 进程
```

至此，内存管理主线（Buddy → SLUB → 页表 → 缺页 → overcommit → 回收 → OOM）全部闭环。
