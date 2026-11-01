# 32. 设备资源与地址映射：ioremap 与 memremap

> 阶段五第十二课。给 `platform_device` 塞入硬件资源（`struct resource`），在 `probe` 里做物理地址 → 虚拟地址映射。过程中踩到一个 arm64 特有的坑：`ioremap` 拒绝映射 RAM。由此厘清 `ioremap`（映射 MMIO 设备寄存器）与 `memremap`（映射 RAM）的分工，以及内核**线性映射**的本质。

## 一、驱动怎么知道硬件在哪

真实硬件的寄存器是**物理地址**，但内核代码只能访问**虚拟地址**。驱动需要两样东西：

1. **知道物理地址** → 由 `platform_device` 的 `resource` 提供
2. **把物理地址映射成虚拟地址** → 用 `ioremap` / `memremap`

```
物理地址 0x0900_0000  ──ioremap──►  虚拟地址 ffff_xxxx  ──readl/writel──►  寄存器
   (resource 里存)                      (代码能用的)              (读写)
```

## 二、struct resource：硬件资源的"名片"

```c
// include/linux/ioport.h
struct resource {
    resource_size_t start;   // 起始地址（或中断号）
    resource_size_t end;     // 结束地址（闭区间，两端都包含）
    const char      *name;
    unsigned long    flags;  // IORESOURCE_MEM / IORESOURCE_IRQ / IORESOURCE_IO
};
```

一个 `platform_device` 可带多个 resource（一块寄存器 + 一个中断号）。probe 里按类型和序号取：

```c
struct resource *r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
```

`resource_size(r)` = `r->end - r->start + 1`（闭区间所以要 +1），是给 `ioremap` 的长度。

## 三、ioremap：映射设备寄存器

```c
void __iomem *io_base = ioremap(r->start, resource_size(r));
u32 val = readl(io_base);           // 读 32 位寄存器
writel(0xDEADBEEF, io_base);        // 写 32 位寄存器
...
iounmap(io_base);                   // 用完必须归还
```

映射后的地址声明为 `void __iomem *`，**不能直接解引用**，必须走 `readl/writel`/`ioread8`/`iowrite8`。`__iomem` 是给内核静态检查器 sparse 的"地址空间标注"：这是 I/O 内存地址，访问需走 I/O 接口（封装 volatile + 内存屏障 + 跨架构差异）。

## 四、arm64 的 ioremap 拒绝映射 RAM（实验踩坑）

第一个实验想用 `kmalloc` 一页内存当"假寄存器"，再 `virt_to_phys` 转物理地址去 `ioremap`。结果 insmod 触发 WARNING：

```
WARNING: ... at arch/arm64/mm/ioremap.c:27 __ioremap_prot+0x8c/0xb8
hello_iomem: ioremap failed
probe with driver hello_iomem failed with error -12
```

根因在 `arch/arm64/mm/ioremap.c:27`：

```c
/* Don't allow RAM to be mapped. */
if (WARN_ON(pfn_is_map_memory(__phys_to_pfn(phys_addr))))
    return NULL;
```

`virt_to_phys(kmalloc页)` 得到的一定是 **RAM 地址**（QEMU virt 的 RAM 从 `0x40000000` 起），`pfn_is_map_memory()` 判定是系统 RAM → `WARN_ON` 触发 → 返回 NULL → probe 拿 `-ENOMEM`(-12)。

**为什么拒绝**：RAM 已经有内核的线性映射，再用 `ioremap` 映射一遍会建立"不可缓存、强序"的设备属性映射，破坏缓存一致性。所以 arm64 加了这层安全检查（x86 相对宽松）。

## 五、真 ioremap：映射 MMIO 设备寄存器（成功实验）

避开 RAM，`ioremap` 一个真实的 MMIO 地址——QEMU virt 的 PL011 串口在 `0x09000000`：

```c
res[0].start = 0x09000000;   // PL011 UART（MMIO，不是 RAM）
res[0].end   = 0x09000fff;
res[0].flags = IORESOURCE_MEM;

io_base = ioremap(r->start, resource_size(r));   // 这次不会被拒
u32 fr = readl(io_base + 0x18);                  // UARTFR，偏移 0x18，只读
```

dmesg 输出：

```
hello_iomem: phys=0x09000000 -> virt=ffff800080033000
hello_iomem: UART FR=0x90 (TXFE=1 RXFE=1)
```

`FR=0x90` = `TXFE=1`（发送 FIFO 空）+ `RXFE=1`（接收 FIFO 空），是 UART 硬件实时状态，证明 `readl` 真的通到了硬件。

### 地址落在哪个区：arm64 地址布局

两课实验的虚拟地址对比：

| | 虚拟地址 | 落在 | 性质 |
|---|---|---|---|
| memremap RAM | `ffff24a303044000` | 线性映射区 | 复用 `__va` |
| ioremap MMIO | `ffff800080033000` | vmalloc 区 | 新建映射 |

源码 `arch/arm64/include/asm/memory.h`（VA_BITS=48）：

```c
#define PAGE_OFFSET    (-(1UL << 48))       // 0xffff_0000_0000_0000 线性映射起点
#define PAGE_END       (-(1UL << 47))       // 0xffff_8000_0000_0000 线性映射终点
#define MODULES_END    (MODULES_VADDR + 2G) // 0xffff_8000_8000_0000
#define VMALLOC_START  MODULES_END          // 0xffff_8000_8000_0000 vmalloc 起点
```

- `kmalloc` 地址 `ffff24a3...` 在 `[PAGE_OFFSET, PAGE_END)` → **线性映射区**
- `ioremap` 地址 `ffff800080033000` 在 `VMALLOC_START` 之上 → **vmalloc 区**

这才是"映射"二字的真义：`ioremap` 在 vmalloc 区**新建了页表映射**，虚拟地址和物理地址无简单关系。

### 别名映射：同一物理地址的多个虚拟地址

`/proc/iomem` 显示 UART 早已被内核自带驱动声明：

```
09000000-09000fff : uart0    ← amba-pl011 驱动已 request_mem_region
```

所以 `0x09000000` 实际被映射了两次：

- `amba-pl011` 的映射（console 正在用）
- 我们 `hello_iomem` 的映射（`ffff800080033000`）

页表允许多个 PTE 指向同一 PFN（别名映射），但**缓存属性必须兼容**。真实驱动的正确姿势是用 `devm_platform_ioremap_resource`（内部先 `request_mem_region` 声明占用），避免两个驱动抢同一块硬件。

## 六、memremap：映射 RAM

正确做法是 `memremap`：

```c
void *io_base = memremap(r->start, resource_size(r), MEMREMAP_WB);
...
memunmap(io_base);
```

`MEMREMAP_WB`（Write-Back 可缓存）表示"这是内存，不是设备寄存器"。

| | `ioremap` | `memremap` |
|---|---|---|
| 映射对象 | MMIO 设备寄存器 | 系统 RAM |
| 缓存属性 | 不可缓存、强序 | 可缓存（WB） |
| arm64 限制 | **拒绝 RAM** | 允许 RAM |
| 对应场景 | 串口/GPIO/网卡寄存器 | 映射某段物理内存 |

## 七、线性映射（direct map）：为什么 memremap 会"偷懒"

实验 dmesg 输出：

```
hello_iomem: phys=0x43044000 size=0x1000 -> virt=ffff24a303044000 (kmalloc=ffff24a303044000)
hello_iomem: wrote=0xdeadbeef readback=0xdeadbeef -> MATCH
```

注意 `virt` 和 `kmalloc` **完全相等**。根因在 `kernel/iomem.c`：

```c
static void *try_ram_remap(resource_size_t offset, size_t size, unsigned long flags)
{
    unsigned long pfn = PHYS_PFN(offset);

    /* In the simple case just return the existing linear address */
    if (pfn_valid(pfn) && !PageHighMem(pfn_to_page(pfn)) &&
        arch_memremap_can_ram_remap(offset, size, flags))
        return __va(offset);     // 直接返回已有的线性地址

    return NULL;
}
```

memremap 文档注释：`MEMREMAP_WB` 且区域是 RAM 时，**绕过建立新映射，直接返回 direct map 里的指针**。

所以 `memremap(virt_to_phys(x)) == x` 是必然的：`virt_to_phys` 和 `__va` 互逆。这揭示了**线性映射的本质**——内核启动时把整块 RAM 做了 1:1 固定偏移映射，RAM 的物理地址和虚拟地址之间只是加减一个常数。

## 八、virt_to_phys 的适用边界

`virt_to_phys` 只是一行加减法（arm64 `arch/arm64/include/asm/memory.h`）：

```c
#define __virt_to_phys(x)  ((x) - PAGE_OFFSET + PHYS_OFFSET)   // 概念版
```

它**只对线性映射区的地址成立**：

- ✅ `kmalloc` 内存：在线性映射区，`virt_to_phys` 合法
- ❌ `vmalloc` 内存：在 vmalloc 区，虚拟连续但物理不连续，`virt_to_phys` 结果错误
- ❌ **用户态 `malloc` 地址**：在进程地址空间，物理页分散，必须走页表

## 九、延伸：用户态地址走页表（pagemap）

用户态 `malloc` 的地址**不能像 `virt_to_phys` 那样算**，只能**查页表**：

- 用户态：`/proc/self/pagemap`（每页 8 字节，`bit63`=present，`bits0~54`=PFN，需 root）
- 内核态：`get_user_pages` + `page_to_phys`

实验 `experiments/memory/pagemap_demo.c` 验证：

```
vaddr(mmap) = 0xffffafeab000
访问前: NOT present（尚无物理页）          ← malloc/mmap 只是记账
写入后: phys = 0x48a12000                 ← 首次写触发缺页才分配

p1 vaddr = 0xee81520, phys = 0x42841520
p2 vaddr = 0xee82530, phys = 0x4886c530
|phys1 - phys2| = 0x602b010（不连续）     ← 相邻虚拟页，物理页分散
```

相邻虚拟页的物理地址差了约 100MB，证明用户态地址和物理地址**没有固定偏移关系**，必须逐页查页表。

## 十、要点回顾

```
kmalloc 地址  ──减法(virt_to_phys)──>  物理地址     （线性映射，固定偏移）
ioremap        ──建新映射──>  MMIO 寄存器          （设备寄存器专用，拒绝 RAM）
memremap       ──复用线性映射──> RAM              （WB，直接返回 direct map）
用户态地址     ──查页表(pagemap/GUP)──> 散落物理页 （无固定关系，且会变）
别名映射       ──多 PTE 指向同一 PFN──> 缓存属性必须兼容（拒绝 ioremap RAM 的深层原因）
```
