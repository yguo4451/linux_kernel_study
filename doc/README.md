# 学习文档索引

本目录存放教学过程中沉淀的必要知识，供日后回看。

## 目录结构

按 Linux 子系统分目录，每个子目录内独立编号（从 01 开始），新增文档不再打乱全局顺序：

| 目录 | 主题 | 篇数 |
|------|------|------|
| [01-build](./01-build/) | 构建与环境 | 5 |
| [02-network](./02-network/) | 网络协议栈 | 13 |
| [03-filesystem](./03-filesystem/) | 文件系统与 procfs | 11 |
| [04-scheduling](./04-scheduling/) | 进程调度 | 15 |
| [05-memory](./05-memory/) | 内存管理 | 21 |
| [06-module](./06-module/) | 内核模块与驱动 | 5 |

## 命名约定

- 目录：`两位序号-子系统名`（如 `04-scheduling`）
- 文件：`两位序号-主题.md`，序号仅在该子目录内递增
- 每篇聚焦一个主题，尽量自包含
- 引用真实命令、文件路径、源码位置，不写空泛结论

## 学习路线图

实际学习顺序是"以构建为主线，遇到哪个子系统就深入哪个"，并非严格按子系统推进。建议按以下顺序回看：

```
阶段一：从 0 搭建可运行 Linux
  01-build/01 构建流程详解
  01-build/02 cpio 与 initramfs
  01-build/05 动态链接与 SSH/SCP 集成（阶段四的基础设施）

阶段二：网络协议栈（TCP/IP 全套，含工具链补强）
  02-network/01 ~ 13  （NAT → TCP → 拥塞控制 → 抓包实证）

阶段三：进程调度（单核 CFS → 多核 SMP → EEVDF 进阶）
  04-scheduling/01 ~ 04

阶段四：内存管理（Buddy → slab → 页表 → 缺页 → 回收）
  05-memory/01 ~ 05

阶段五：内核模块与驱动开发（从"读内核"到"写内核"）
  06-module/01 hello 模块与 taint 机制
```

> 文件系统（03-filesystem）与工具链补强（01-build/03、04）是在主线中"随用随学"穿插进来的，按需查阅即可。

---

## 索引

### 01-build · 构建与环境

| 编号 | 文档 | 内容 |
|------|------|------|
| 01 | [构建流程详解](./01-build/01-构建流程详解.md) | 从 0 搭建 Linux：Docker/QEMU 分工、六步构建、设计取舍、启动链路 |
| 02 | [cpio 与 initramfs](./01-build/02-cpio与initramfs.md) | cpio 是什么、initramfs 为什么用 cpio(newc) 而不是 tar |
| 03 | [tcpdump 集成与静态链接](./01-build/03-tcpdump集成与静态链接.md) | BusyBox 1.36 移除 tcpdump、源码编译 libpcap+tcpdump、静态 vs 动态链接取舍 |
| 04 | [多终端进入同一系统](./01-build/04-多终端进入同一系统.md) | 串口控制台 vs 登录会话、hostfwd 端口转发、telnetd + devpts 伪终端 |
| 05 | [动态链接与 SSH/SCP 集成](./01-build/05-动态链接与SSH-SCP集成.md) | 静态→动态的取舍、ld-linux/PT_INTERP/PT_NEEDED、SSH 选型（dropbear→OpenSSH）、glibc sysroot 复制、ssh/scp 免密登录 |

### 02-network · 网络协议栈

| 编号 | 文档 | 内容 |
|------|------|------|
| 01 | [NAT 网络地址转换](./02-network/01-NAT网络地址转换.md) | NAT/SNAT/DNAT/端口映射原理、为什么外网看不到内网、QEMU user 网络 |
| 02 | [网络配置实战](./02-network/02-网络配置实战.md) | 上网四要素、ifconfig/route/resolv.conf 手动配置、三个错误对照、完整链路 |
| 03 | [ARP 地址解析](./02-network/03-ARP地址解析.md) | ARP 缓存表、工作流程、IP→MAC 翻译、QEMU MAC 彩蛋、arp 命令 |
| 04 | [TCP 传输控制协议](./02-network/04-TCP传输控制协议.md) | 三次握手、四次挥手、状态机、/proc/net/tcp、CLOSE_WAIT 排查、inode 反查 |
| 05 | [三次握手的内核实现](./02-network/05-三次握手的内核实现.md) | SYN→SYN-ACK→ACK 调用链、request_sock 半连接、两队列、SYN flood 防护 |
| 06 | [半连接与全连接队列](./02-network/06-半连接与全连接队列.md) | request_sock 结构、半连接队列职责、半连接→全连接转换、accept 取队头 |
| 07 | [数据快速路径](./02-network/07-数据快速路径.md) | tcp_rcv_established 头部预测、fast/slow path、pred_flags、为何快 |
| 08 | [TCP 拥塞控制](./02-network/08-TCP拥塞控制.md) | 慢启动/拥塞避免/ssthresh、AIMD、两个窗口、Reno 源码 |
| 09 | [TCP 拥塞控制-丢包恢复](./02-network/09-TCP拥塞控制-丢包恢复.md) | 快速重传/快速恢复、RTO Loss、状态机、undo 撤销 |
| 10 | [现代拥塞控制 CUBIC 与 BBR](./02-network/10-现代拥塞控制CUBIC与BBR.md) | 三次函数、凸/凹/平台区、β=0.7、vs Reno、BBR |
| 11 | [数据发送路径](./02-network/11-数据发送路径.md) | tcp_sendmsg→tcp_write_xmit、五道闸门、cwnd 一写一读、Nagle/TSQ |
| 12 | [TCP 连接关闭与定时器](./02-network/12-TCP连接关闭与定时器.md) | 四次挥手源码、TIME_WAIT 与 2MSL、tcp_timer.c 定时器体系、close 发 FIN 还是 RST |
| 13 | [真实挥手报文抓包](./02-network/13-真实挥手报文抓包.md) | tcpdump 抓包实证、三次 vs 四次挥手对比、FIN 消耗序号、RST 替代 FIN |

### 03-filesystem · 文件系统与 procfs

| 编号 | 文档 | 内容 |
|------|------|------|
| 01 | [BusyBox 与 procfs 统计](./03-filesystem/01-BusyBox与procfs统计.md) | 为什么 BusyBox netstat 没有 -s、/proc/net/snmp 数据源、awk 替代方案 |
| 02 | [proc 与 sysfs 虚拟文件系统](./03-filesystem/02-proc与sysfs虚拟文件系统.md) | ctl_table vs kobject、/proc/sys 是 sysctl 不是 sysfs、hostname 源码链路、一个文件一个值 |
| 03 | [从 lsof 看 /proc 与文件描述符](./03-filesystem/03-lsof与proc文件描述符.md) | /proc/PID/fd 机制、socket inode 匹配、lsof 参数、为什么慢 |
| 04 | [VFS 四大对象](./03-filesystem/04-VFS四大对象.md) | super_block/inode/dentry/file 四结构、函数指针表多态、硬链接实测（dentry vs inode 铁证）、类型 vs 实例 |
| 05 | [路径解析 pathwalk](./03-filesystem/05-路径解析pathwalk.md) | nameidata、link_path_walk 主循环、fast/slow path、负 dentry、symlink 栈、dcache 增长实测（slabinfo +91） |
| 06 | [open 系统调用调用链](./03-filesystem/06-open系统调用调用链.md) | do_sys_openat2 六步、f->f_op=inode->i_fop 多态落地、filp vs files_cache 命名陷阱、f_pos 独立性实测 |
| 07 | [read/write 调用链](./03-filesystem/07-read-write调用链.md) | vfs_read→f_op->read_iter→filemap_read、页缓存交汇点、dd 956MB/s 脏页写入、readahead 预读窗口实测 |
| 08 | [ext4 磁盘布局](./03-filesystem/08-ext4磁盘布局.md) | 块组/超级块/组描述符/inode表、inode号→磁盘位置换算、extent (0):33280 实测、目录项 ext4_dir_entry_2 |
| 09 | [procfs 实现](./03-filesystem/09-procfs实现.md) | 无磁盘文件系统、PDE/proc_create、两层函数表、seq_file 边读边生成、proc_inode_cache 实测 |
| 10 | [sysfs 与 kobject](./03-filesystem/10-sysfs与kobject.md) | kobject 树投影、attribute/sysfs_ops、one value per file 铁律、show/store 读写模型、与 procfs 对照 |
| 11 | [pstore 与 fsnotify](./03-filesystem/11-pstore与fsnotify崩溃日志与文件通知.md) | 内核黑匣子 pstore/ramoops、pmsg 用户态消息、dmesg时间戳+pmsg定位崩溃、inotify/fanotify 文件事件通知、车载日志监控链路 |

### 04-scheduling · 进程调度

| 编号 | 文档 | 内容 |
|------|------|------|
| 01 | [task_struct 与优先级](./04-scheduling/01-task_struct与优先级.md) | task_struct/sched_entity、PF_KTHREAD、/proc/PID/stat 52 字段、优先级换算、调度类链 |
| 02 | [CFS 核心：vruntime 与红黑树](./04-scheduling/02-CFS的vruntime与红黑树.md) | 权重表、vruntime 加权公式、红黑树排序、EEVDF 进化 |
| 03 | [CFS 调度周期与抢占](./04-scheduling/03-CFS调度周期与抢占.md) | base_slice 时间片、TIF_NEED_RESCHED 抢占标志、用户态/内核态抢占、PREEMPT_RT 入口 |
| 04 | [多核调度与负载均衡](./04-scheduling/04-多核调度与负载均衡.md) | per-CPU 运行队列、fork 主动选核 vs load_balance 被动搬运、can_migrate_task、migration/N、taskset 实验 |
| 05 | [EEVDF 调度器](./04-scheduling/05-EEVDF调度器.md) | eligible 与虚拟截止时间、lag、debugfs 观测（E/N 标记）、base_slice 实测 2.25ms、nice 对比实验 |
| 06 | [RT 实时调度类](./04-scheduling/06-RT实时调度类.md) | push/pull 多核迁移、RT throttling 带宽限流、优先级反转与优先级继承（rt_mutex）、汽车相关性 |
| 07 | [优先级反转实测](./04-scheduling/07-优先级反转实测.md) | 三线程实验程序、普通 mutex vs PI mutex 实测 314ms→40ms、时间线分析、踩坑（顺序/节流） |
| 08 | [PID 与线程模型](./04-scheduling/08-PID与线程模型.md) | 三重 ID（PID/TGID/TID）、/proc 文件两类语义、PID 分配机制、跨 OS 对比 |
| 09 | [CPU 隔离与实时](./04-scheduling/09-CPU隔离与实时.md) | isolcpus 机制与源码、默认 affinity 验证、QEMU 性能测试局限、通往 cyclictest/PREEMPT_RT |
| 10 | [PREEMPT_RT 内核升级](./04-scheduling/10-PREEMPT_RT内核升级.md) | 6.6 arm64 不支持 PREEMPT_RT、升级 6.12 LTS、ARCH_SUPPORTS_RT 依赖、VirtioFS cp 竞态与 rsync 解决 |
| 11 | [中断线程化与 PREEMPT_RT](./04-scheduling/11-中断线程化与PREEMPT_RT.md) | 中断两段式、下半部四种机制、强制线程化源码、/proc/interrupts 对照、优先级三层换算 |
| 12 | [网络中断路径实测](./04-scheduling/12-网络中断路径实测.md) | ping 实验、中断计数增量、收包异步 vs 发包同步、NET_RX/NET_TX 不对称、完整中断旅程 |
| 13 | [SCHED_DEADLINE 调度器](./04-scheduling/13-SCHED_DEADLINE调度器.md) | EDF + CBS、三参数、准入控制、dl_demo 实验、EPERM 源码发现、调度类层次、汽车 AUTOSAR 相关性 |
| 14 | [CPU 隔离与 cgroup](./04-scheduling/14-CPU隔离与cgroup.md) | cgroup v2、cpu.max 带宽控制器、CFS_BANDWIDTH、throttle/unthrottle 源码、ECU 多负载分区 |
| 15 | [cgroup 控制器全景](./04-scheduling/15-Cgroup控制器全景.md) | 通用文件 vs 控制器文件、cpu/cpuset/memory/pids/io 五大控制器、水位递进链、ECU 分区隔离 |

### 05-memory · 内存管理

| 编号 | 文档 | 内容 |
|------|------|------|
| 01 | [Buddy 物理内存分配](./05-memory/01-Buddy物理内存分配.md) | node/zone/page 层次、free_area、XOR 找伙伴、分配/释放调用链、迁移类型、/proc/buddyinfo 实验 |
| 02 | [Slab 与 SLUB 分配器](./05-memory/02-Slab与SLUB分配器.md) | 对象缓存、三级结构（per-CPU freelist/partial/Buddy）、快慢路径、kmalloc 分档、/proc/slabinfo |
| 03 | [页表与虚拟地址空间](./05-memory/03-页表与虚拟地址空间.md) | 多级页表（5 级）、MMU 翻译、PTE 位、mm_struct/VMA、TLB、/proc/PID/maps |
| 04 | [缺页异常 Page Fault](./05-memory/04-缺页异常PageFault.md) | 四种缺页类型、do_page_fault 调用链、匿名页/COW/换入、零页优化、VMA 判定、mem_hog 实验 |
| 05 | [内存超卖 Overcommit](./05-memory/05-内存超卖overcommit.md) | __vm_enough_memory 记账、三种 overcommit 策略、单次 vs 累计检查、glibc mmap 三连、malloc 两阶段模型 |
| 06 | [内存回收：水位线与 kswapd](./05-memory/06-内存回收kswapd与水位线.md) | 三条水位线 min/low/high、平方根公式、__setup_per_zone_wmarks、kswapd 唤醒/睡眠、balance_pgdat、SwapTotal=0 隐患 |
| 07 | [OOM Killer](./05-memory/07-OOMKiller.md) | oom_badness 打分、触发者≠被杀者、完整调用栈（缺页→Buddy→OOM）、all_unreclaimable、oom_score_adj 保护 |
| 08 | [内存与实时性：mlock 锁页](./05-memory/08-内存与实时性mlock锁页.md) | 三大不可预测延迟、mlock 两动作（标记+populate）、unevictable LRU、mlockall 三参数、MCL_FUTURE 生效机制 |
| 09 | [cyclictest 实时抖动测量](./05-memory/09-cyclictest实时抖动测量.md) | rt-tests 静态编译、隔离验证、TCG vs HVF 硬件加速、Max 被宿主调度污染、直方图两段式分析 |
| 10 | [页缓存 Page Cache](./05-memory/10-页缓存PageCache.md) | address_space/xarray、读路径 filemap_read、写路径 generic_perform_write、drop_caches 只释放干净页、ext4 磁盘实测 |
| 11 | [反向映射 rmap 与 swap 换出](./05-memory/11-反向映射rmap与swap换出.md) | anon_vma/anon_vma_chain、page->mapping 双用途、shrink_folio_list→try_to_unmap→swap_writepage、mem_hog 换出实测、swap entry |
| 12 | [Writeback 写回](./05-memory/12-Writeback写回.md) | 五个触发条件、动态脏页阈值（global_dirtyable_memory）、balance_dirty_pages_ratelimited 节流、wb_writeback、阈值 90% 实测 Dirty 堆积 |
| 13 | [/proc/meminfo 字段详解](./05-memory/13-proc-meminfo字段详解.md) | 全部字段按主题分组、MemAvailable vs MemFree、Cached/Buffers/Shmem、CMA 预留、真实 meminfo 解读 |
| 14 | [/proc/vmstat 字段详解](./05-memory/14-proc-vmstat字段详解.md) | 累计计数器 vs 快照、pgscan/pgsteal 回收、pswpin/pswpout、零页 swap 优化、workingset refault、cma_alloc |
| 15 | [CMA 连续内存分配器](./05-memory/15-CMA连续内存分配器.md) | 预留+借出+迁移、MIGRATE_MOVABLE 限制、cma_alloc/alloc_contig_range 源码、pagetypeinfo order 读法、车载摄像头 DMA 相关性 |
| 16 | [DMA-BUF 零拷贝](./05-memory/16-DMABUF零拷贝.md) | exporter/importer/dmabuf fd、dma_buf_export/attach/map 调用链、sg_table 物理地址、与 CMA 配合、车载摄像头管线、缓存一致性 |
| 17 | [内存压缩 Compaction](./05-memory/17-内存压缩Compaction.md) | 外部碎片、搬可移动页合并高阶块、compact_zone/migrate_pages、compact_memory 手动触发实测（小页→大块）、与 CMA/THP 连接 |
| 18 | [巨页 hugetlbfs 与 THP](./05-memory/18-巨页hugetlbfs与THP.md) | TLB miss、hugetlbfs vs THP、nr_hugepages + MAP_HUGETLB 实测、巨页不可超配、THP 与实时性冲突 |
| 19 | [内存 cgroup 与分区隔离](./05-memory/19-内存cgroup与分区隔离.md) | memory.max/high/low、cgroup OOM 调用链、CONSTRAINT_MEMCG 铁证、64MB 限额实测、ECU 分区故障隔离 |
| 20 | [zram 与 zswap 压缩内存](./05-memory/20-zram与zswap压缩内存.md) | 用 CPU 换内存、zram 压缩块设备 vs zswap 压缩缓存、无磁盘 vs 有磁盘、车载 AAOS 标配、观测接口 |
| 21 | [zram 与 zswap 实战](./05-memory/21-zram与zswap实战.md) | 内核配置实测（THP 与 PREEMPT_RT 互斥、FRONTSWAP 6.12 移除）、hot_add 陷阱、Zswap/Zswapped 字段、零页换出优化复现、换入解压 |

### 06-module · 内核模块与驱动

| 编号 | 文档 | 内容 |
|------|------|------|
| 01 | [hello 模块与 taint 机制](./06-module/01-hello模块与taint机制.md) | 第一个内核模块全流程、编译三前提（Makefile/KDIR/Module.symvers）、taint 污染标记、lsmod 字段解读 |
| 02 | [insmod 调用链](./06-module/02-insmod调用链.md) | finit_module/init_module 系统调用、幂等去重、load_module 十五步、do_init_module |
| 03 | [模块重定位](./06-module/03-模块重定位.md) | layout_and_allocate 定址、simplify_symbols 定符号、apply_relocations 改写引用、arm64 指令级重定位 |
| 04 | [EXPORT_SYMBOL 导出符号与模块依赖](./06-module/04-EXPORT_SYMBOL导出符号与模块依赖.md) | __ksymtab 段、find_symbol 二分查找、Module.symvers、ref_module 依赖链、lsmod used-by、引用计数 |
| 05 | [模块参数 module_param](./06-module/05-模块参数module_param.md) | 宏展开、struct kernel_param、__param 段、parse_args 解析、param_ops、sysfs 参数接口 |

> 后续教学新增的文档，放入对应子目录并同步更新本表。
