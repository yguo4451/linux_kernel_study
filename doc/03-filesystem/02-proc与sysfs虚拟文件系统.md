# 17. /proc 与 /sys 的区别：procfs vs sysfs

> 虚拟文件系统辨析篇。回答"`/proc` 和 `/sys` 都是虚拟的，到底有啥不一样"，并用 `hostname` 打通完整源码链路。

## 一句话结论

- **procfs（`/proc`）**：进程与内核运行信息的**视图**，数据来源零散（进程、内核变量），按"谁能提供信息谁注册"松散组织。
- **sysfs（`/sys`）**：设备与驱动**拓扑**，数据来源统一是 **kobject 树**，按设备树层次严格组织。
- **关键澄清**：`/proc/sys` 这个目录**不是 sysfs**，它是 **sysctl 框架**的接口，只是碰巧挂在 procfs 挂载点下。

## 现象入手：为什么 hostname 只在 /proc 不在 /sys

```sh
# QEMU 内
cat /proc/sys/kernel/hostname     # 有
ls /sys/kernel/hostname           # 没有这个文件
```

根源在于两套完全不同的数据模型：

```
  /proc/sys/kernel/hostname              /sys/devices/...
       │                                      │
   ctl_table (内核变量表)                kobject (内核对象树)
       │                                      │
  .data = init_uts_ns.name.nodename      .kobj 挂接在 devices 总线上
```

`hostname` 是一个**内核字符串变量**，不是**设备对象**，所以它天然属于 sysctl 而非 sysfs。

## 数据模型对比（数据优先）

### procfs：ctl_table（内核变量表）

```c
// include/linux/sysctl.h（字段已精简）
struct ctl_table {
    const char *procname;      // 在 /proc/sys 下生成的文件名
    void *data;                // 指向真正的内核变量
    int maxlen;                // 变量最大长度
    umode_t mode;              // 文件权限
    proc_handler *proc_handler;// 读/写回调
};
```

一个 `ctl_table` 条目 = 一个内核变量的**访问入口**，`data` 直接指向内核里的那串内存。

### sysfs：kobject（内核对象树）

```c
// include/linux/kobject.h（字段已精简）
struct kobject {
    const char *name;        // 对象名 → 生成目录/文件名
    struct kobject *parent;  // 父对象 → 形成树形层级
    struct kset *kset;       // 所属集合（同类型对象组）
    struct kobj_type *ktype; // 类型 → 决定有哪些 attribute 文件
};
```

一个 `kobject` = 内核对象模型里的一个**节点**，靠 `parent` 串成树。

## 核心差异表

| 维度 | procfs（/proc） | sysfs（/sys） |
|------|----------------|--------------|
| 建模对象 | 内核变量、运行信息 | 设备、驱动、总线、类 |
| 核心数据结构 | `ctl_table` | `kobject` |
| 数据来源 | 零散（各处内核代码） | 统一（kobject 树） |
| 组织方式 | 松散（谁注册谁出现） | 严格分层（bus/class/dev） |
| 文件内容约束 | 宽松（可多行多值） | 铁律：**一个文件一个值** |
| 典型文件 | `/proc/cpuinfo`、`/proc/meminfo` | `/sys/class/net/eth0/` |
| 写语义 | 极少数可写（sysctl） | 大量可写（触发驱动动作） |

## hostname 完整源码链路（结论落到源码）

从"QEMU 里看到的名字"倒推回"编译期常量"：

```
cat /proc/sys/kernel/hostname
        │  VFS read → proc_handler 回调
        ▼
kernel/utsname_sysctl.c:108
        .procname  = "hostname"
        .data      = init_uts_ns.name.nodename
        .mode      = 0644                  ← 可写
        .proc_handler = proc_do_uts_string
        │
        ▼  data 指向的变量
init/version-timestamp.c:10
        struct uts_namespace init_uts_ns = {
            .name = { .nodename = UTS_NODENAME, ... }
        }
        │
        ▼  宏展开
include/linux/uts.h:13
        #define UTS_NODENAME CONFIG_DEFAULT_HOSTNAME
        │
        ▼  编译期
.config / defconfig 里的 CONFIG_DEFAULT_HOSTNAME
```

**写路径**（`echo newname > /proc/sys/kernel/hostname`）：

```
write() → proc_do_uts_string → 写 init_uts_ns.name.nodename
        → kernel/sys.c:1374  SYSCALL_DEFINE2(sethostname)
        → kernel/sys.c:1394  uts_proc_notify(UTS_PROC_HOSTNAME)
        → 通知监听者（如 systemd）hostname 已变
```

两条路径（sysctl 写 vs `sethostname()` 系统调用）最终落到同一份 `init_uts_ns`，因为 `.data` 指向的就是它。

## 两个铁律

### 铁律一：`/proc/sys` 是 sysctl，不是 sysfs

`/proc/sys` 由 sysctl 框架创建，挂载点类型仍是 proc：

- 初始化入口 `fs/proc/proc_sysctl.c:1553` `proc_sys_init()`
- 文件 inode 的 mode 直接取自 `table->mode`：`fs/proc/proc_sysctl.c:470-476`
- `hostname` 是 `0644`（可写），`osrelease`/`version` 是 `0444`（只读）——权限在 `ctl_table` 里逐条声明

这解释了为什么 `ls -l /proc/sys/kernel/hostname` 显示 `-rw-r--r--`，而隔壁 `osrelease` 是 `-r--r--r--`。

### 铁律二：sysfs 一个文件一个值

sysfs 的 attribute 设计约定：**每个文件只能暴露一个标量值**。这是用户态工具（udev、systemd）能可靠解析的前提。procfs 没有这条约束，所以 `/proc/meminfo` 可以几十行、`/proc/net/snmp` 可以一堆字段。

## 实验验证（QEMU 内）

```sh
# 1. 确认 hostname 归属
ls -l /proc/sys/kernel/hostname        # -rw-r--r-- 可写
ls -l /proc/sys/kernel/osrelease       # -r--r--r-- 只读

# 2. 对比两棵树的根
ls /proc | head                        # 数字目录 = 进程 PID
ls /sys                                # block bus class dev ... 设备拓扑

# 3. 写 hostname（两条路径等价）
echo studybox > /proc/sys/kernel/hostname
hostname studybox                      # 等价，都改 init_uts_ns
cat /proc/sys/kernel/hostname

# 4. 验证 sysfs 确实没有 hostname
ls /sys/kernel/                        # 有 kobject 的才有
```

## 陷阱

- **别被路径骗了**：`/proc/sys/` 前缀里带 "sys"，但它是 sysctl（ctl_table），不是 sysfs（kobject）。判断标准看数据结构，不看路径名。
- **`/proc` 里不只有进程**：`/proc/self`、`/proc/meminfo`、`/proc/net/*` 都不是进程，procfs 本质是"内核信息杂货铺"，进程信息只是它最老、最常用的一块。
- **可写性来源不同**：sysfs 可写靠 `ktype->sysfs_ops` 的 `store` 回调；`/proc/sys` 可写靠 `ctl_table.proc_handler`。两者机制不同，别混。
- **写 /proc/sys 影响全局**：改的是内核变量（如 `hostname` 是命名空间级、`/proc/sys/net/ipv4/ip_forward` 是全局），在真实系统上需要 root 且可能影响运行行为。
