# pstore 与 fsnotify：崩溃日志与文件事件通知

> 文件系统子系列第 11 篇。doc 08~10 讲的是"存储类"文件系统，本篇讲 `fs/` 下两个**非存储**成员：**pstore**（内核黑匣子，崩溃日志持久化）和 **fsnotify**（文件事件通知，inotify/fanotify）。两者组合成车载日志监控链路。

## 第一部分：pstore —— 内核的黑匣子

### 一、核心问题

内核 panic 时，dmesg 在 RAM 里（重启即失）、磁盘写入已不可靠。pstore 在 panic 瞬间把日志写进"能扛过重启"的存储，供重启后分析。

```
正常运行时：pstore 静默待命（注册 kmsg_dump）
    ↓
内核 panic → kmsg_dump 触发 → pstore_dump 写进持久存储
    ↓
重启 → 挂载 pstore → 读 /sys/fs/pstore/dmesg-ramoops-0
```

### 二、存什么：`enum pstore_type_id`（`include/linux/pstore.h:28`）

```c
enum pstore_type_id {
	PSTORE_TYPE_DMESG   = 0,  // ★ 内核崩溃日志（最重要）
	PSTORE_TYPE_MCE     = 1,  // 机器检查异常（硬件错误）
	PSTORE_TYPE_CONSOLE = 2,  // 控制台日志
	PSTORE_TYPE_FTRACE  = 3,  // 崩溃前函数调用链
	PSTORE_TYPE_PMSG    = 7,  // 用户态消息
	...
};
```

对应 `/sys/fs/pstore/` 下的文件：`dmesg-ramoops-0`、`console-ramoops-0`、`ftrace-ramoops-0`、`pmsg-ramoops-0`。

### 三、后端（数据物理存哪）

| 后端 | 文件 | 原理 | 适用 |
|------|------|------|------|
| **ramoops**（持久 RAM） | `ram.c`/`ram_core.c` | 预留 RAM，暖重启内容不丢 | **Android/车载（最常用）** |
| blk（块设备） | `blk.c` | 存到分区 | 有存储的设备 |
| efi | `platform.c` | EFI 变量 | x86 UEFI |
| zone | `zone.c` | zoned 存储 | 分区块设备 |

ramoops 核心参数：`record_size`/`console_size`/`ftrace_size`/`pmsg_size`/`mem_address`。

### 四、pmsg：用户态"能扛重启的写字板"

```c
fd = open("/dev/pmsg0", O_WRONLY);
write(fd, "entering critical section\n", ...);
```

- **只存你写的字符串本身**，内核不自动附加 PID/时间戳/线程名
- 环形缓冲，只保留最后 `pmsg_size` 大小
- 价值在于"用最后写到的位置反推崩溃点"：最后一条是 `"entering CAN send"`，说明崩溃发生在进入 CAN 发送之后

### 五、怎么知道"何时崩溃"

两个维度分开获取：

| 问题 | 来源 |
|------|------|
| 何时崩溃（时间点） | dmesg 的内核时间戳 `[123.456789]`（距启动秒数） |
| 崩溃在哪（位置） | pmsg 最后写到哪一句 |

pmsg 里想带时间，用 `CLOCK_MONOTONIC`（距启动秒数），和 dmesg 的 `[123.456789]` 同坐标系，可精确对齐。

### 六、陷阱

1. **持久 RAM 依赖暖重启**：不断电才保留，断电即失（掉电崩溃要用 blk 后端）。
2. **内存要预留**：ramoops 占用一部分 RAM。
3. **环形覆盖**：只留最后 N KB。
4. **panic 路径不能阻塞**：写入必须快、不能调会阻塞的函数。

## 第二部分：fsnotify —— 文件事件通知（不是文件系统）

### 七、它解决什么：轮询 → 通知

```
老办法（轮询）：while(1) { 检查文件变了没; sleep(1); }   ← 浪费 CPU
新办法（通知）：文件一变，内核主动告诉你                    ← 高效
```

### 八、一个核心 + 三个接口

```
fs/notify/
├─ fsnotify.c      ← 核心事件分发引擎
├─ group.c         ← fsnotify_group（订阅组）
├─ mark.c          ← fsnotify_mark（watch，挂在 inode 上）
├─ inotify/        ← 接口1：单文件/目录变更通知
├─ fanotify/       ← 接口2：文件系统级通知 + 拦截/审计
└─ dnotify/        ← 接口3：老式目录通知（已过时）
```

| 接口 | 粒度 | 典型用户 |
|------|------|---------|
| inotify | 单文件/目录 | 编辑器热重载、文件同步 |
| fanotify | 整个挂载点 | 杀毒、安全审计 |

### 九、核心数据结构

```c
struct fsnotify_group {   // 一个 inotify/fanotify fd = 一个订阅组
	wait_queue_head_t notification_waitq;  // 事件来了唤醒 read
};

struct fsnotify_mark {    // 一个 watch：挂在 inode 上
	struct fsnotify_group *group;   // 属于哪个订阅组
	struct inode *inode;            // 监控哪个 inode
	__u32 mask;                     // 监控哪些事件
};
```

### 十、钩子遍布 VFS

`fsnotify_*` 钩子散布在 VFS 各处（doc 06 的 `do_dentry_open` 里就有 `fsnotify_open_perm`）：

```
open 文件 → fsnotify_open()      创建文件 → fsnotify_create()
删除文件 → fsnotify_unlink()     修改文件 → fsnotify_modify()
```

有订阅就通知，没订阅就跳过（几乎零开销）。

## 第三部分：车载日志监控链路（两者串起来）

```
┌─────────────────────────────────────────────────────────┐
│ 正常运行时                                               │
│  inotify 监控日志文件增长 → 触发增量上传                    │
│  fanotify 审计关键文件访问（标定/证书文件）                  │
│  pmsg 在关键操作前后打点（应用层"最后位置"）                 │
├─────────────────────────────────────────────────────────┤
│ 崩溃瞬间                                                 │
│  pstore 自动抓 dmesg（内核时间戳 + panic 原因）             │
│  pmsg 保留"死前最后写到哪"                                 │
├─────────────────────────────────────────────────────────┤
│ 重启后                                                   │
│  dmesg 给"何时崩溃" + pmsg 给"崩溃在哪" → 定位崩溃点         │
│  上传到厂商服务器（bugreport）                             │
└─────────────────────────────────────────────────────────┘
```

## 要点总结

1. **pstore 是"内核黑匣子"**：panic 时把日志写进持久存储（通常持久 RAM），扛过重启。
2. **pmsg 是"能扛重启的用户态写字板"**：只存你写的字节，无自动元数据，用于反推崩溃位置。
3. **"何时崩溃"看 dmesg 时间戳，"崩溃在哪"看 pmsg 最后一条**。
4. **fsnotify 不是文件系统**：是寄生在 VFS 上的事件通知框架（inotify/fanotify）。
5. **目录位置 ≠ 功能类别**：`mm/shmem.c`（tmpfs）在 mm/ 却是文件系统；`fs/notify/` 在 fs/ 却不是文件系统。
