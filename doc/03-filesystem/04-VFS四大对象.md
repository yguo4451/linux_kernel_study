# VFS 四大对象：super_block / inode / dentry / file

> 文件系统子系列第 4 篇。前三篇讲的是 procfs/sysfs 这两个"伪文件系统"的用法，本篇正式进入**文件系统核心**：VFS 抽象层赖以工作的四大数据结构。配套 QEMU 实验已跑通，数据见文末。

## 一、为什么需要 VFS

Linux 要同时支持 ext4、procfs、sysfs、tmpfs、devtmpfs……每种文件系统在磁盘（或内存）里的布局完全不同。VFS（Virtual File System）定义一套**统一抽象接口**，所有文件系统实现这套接口，用户程序只跟这套接口打交道：

```
用户程序 → 系统调用 open/read/write/close
             ↓ ↓ ↓（统一接口）
           VFS 抽象层（四大对象 + 函数指针表）
             ↓ ↓ ↓ ↓
        ext4  procfs  sysfs  tmpfs ...（各自实现）
```

VFS 的"多态"手段是**函数指针表**：每个对象挂一个 `*_operations` 结构，里面全是函数指针，具体文件系统填自己的实现：

| 对象 | 函数表成员 | 作用 |
|------|-----------|------|
| `super_block` | `s_op`（`super_operations`） | 挂载/卸载、写 inode、统计 |
| `inode` | `i_op`（`inode_operations`） | 建/删文件、建/删链接、改属性 |
| `dentry` | `d_op`（`dentry_operations`） | 名字校验、比较、清理 |
| `file` | `f_op`（`file_operations`） | read/write/llseek/mmap |

## 二、四大对象：各自管什么

| 对象 | 定义位置 | 一句话职责 | 生命周期 |
|------|---------|-----------|---------|
| `super_block` | `include/linux/fs.h:1304` | 一个**已挂载的文件系统** | 挂载创建，卸载销毁 |
| `inode` | `include/linux/fs.h:665` | 一个**文件本体**（磁盘持久对象） | 文件在，inode 就在 |
| `dentry` | `include/linux/dcache.h:83` | 一个**路径名**（内存缓存对象） | 纯内存，可回收 |
| `file` | `include/linux/fs.h:1075` | 一次**打开**（进程打开文件描述） | open 创建，close 销毁 |

**最关键的一对区分**：

- **inode = 文件本身**，一个文件只有一个 inode。
- **dentry = 文件的名字**，一个文件可以有多个名字（硬链接）。

所以"文件"本质是"**一个 inode + 若干 dentry**"的组合。

## 三、源码关键字段

### `struct inode`（`fs.h:665`）

```c
struct inode {
	umode_t		i_mode;      // 文件类型 + 权限（ls -l 第一列）
	kuid_t		i_uid;       // 属主
	kgid_t		i_gid;       // 属组
	const struct inode_operations *i_op;  // inode 操作函数表
	struct super_block	*i_sb;  // 属于哪个已挂载文件系统
	struct address_space	*i_mapping; // 页缓存入口（mm 与 fs 交汇点）
	unsigned long		i_ino;      // inode 号（ls -i）
	unsigned int		i_nlink;    // 硬链接数（ls -l 第二列）
	dev_t			i_rdev;     // 设备文件号
	loff_t			i_size;     // 文件大小
	...
};
```

`i_mapping` 是 `address_space` 页缓存入口，**文件系统和内存管理的交汇点**（见 05-memory/10 页缓存）。

### `struct dentry`（`dcache.h:83`）

```c
struct dentry {
	struct dentry	*d_parent;      // 父目录
	struct qstr	d_name;         // 文件名
	struct inode	*d_inode;       // 对应 inode（负 dentry 时为 NULL）
	struct super_block *d_sb;       // 属于哪个文件系统
	struct list_head d_lru;         // 不用的 dentry 挂 LRU 可回收
	struct hlist_node d_sib;        // 兄弟节点
	struct hlist_head d_children;   // 子节点（目录树结构）
	...
};
```

dentry 组成一棵**目录树**（`d_parent`/`d_children`/`d_sib`），根挂在 `super_block->s_root`。

### `struct file`（`fs.h:1075`）

```c
struct file {
	const struct file_operations *f_op;   // read/write 函数表
	struct address_space	*f_mapping;  // 页缓存（同 inode 的 i_mapping）
	struct inode		*f_inode;    // 文件本体
	struct path		f_path;      // 路径（dentry + mount）
	loff_t			f_pos;       // 当前读写位置（每次打开独立！）
	fmode_t			f_mode;      // O_RDONLY/O_WRONLY...
	...
};
```

**关键点**：`f_pos` 每次打开独立。两个进程打开同一文件，各有各的 `struct file` 和 `f_pos`，但共享同一个 `inode` 和 `dentry`。

### `struct super_block`（`fs.h:1304`）

```c
struct super_block {
	dev_t			s_dev;         // 设备号
	struct file_system_type	*s_type; // 文件系统类型
	const struct super_operations *s_op; // 超级块操作函数表
	unsigned long		s_magic;   // 魔数（区分文件系统）
	struct dentry		*s_root;   // 挂载点根 dentry
	struct block_device	*s_bdev;   // 底层块设备（ext4 有，procfs 无）
	void			*s_fs_info; // 文件系统私有数据
	...
};
```

## 四、关系图

```
super_block（一个挂载的文件系统）
   │ s_type / s_root / s_bdev
   ▼
dentry 树（路径名缓存，一棵目录树）
   │ d_parent/d_children/d_sib
   │ d_name（名字）            d_inode
   ▼                            ▼
dentry ─────────────────────► inode（文件本体）
   │                            │ i_mapping
   │                            ▼
   │                       address_space（页缓存）
   │
   │ 进程 open() 后创建
   ▼
file（一次打开）
   ├─ f_inode ──► inode（同一个）
   ├─ f_path ───► dentry + mount
   ├─ f_pos（独立读写位置）
   └─ f_op（read/write 函数指针）
```

一句话记忆：**super_block 管挂载、inode 管文件、dentry 管名字、file 管打开**。

## 五、类型 vs 实例

| 文件 | 语义 |
|------|------|
| `/proc/filesystems` | 注册的 `file_system_type`（"类"） |
| `/proc/mounts` | 当前挂载的 `super_block`（"对象"） |

`nodev` 前缀 = 不需要块设备（内存型文件系统）。同一个类型可挂载多份，每份一个 `super_block`。

## 六、QEMU 实测数据

### super_block（`/proc/mounts`，10 个实例）

```
/dev/vda /mnt ext4 rw,relatime 0 0      ← 唯一真磁盘文件系统（s_bdev=/dev/vda）
proc     /proc proc rw,relatime 0 0     ← 内存型，无块设备
sysfs    /sys sysfs rw,relatime 0 0
...
```

### 文件系统类型（`/proc/filesystems`，26 种"类"）

`nodev` 前缀代表内存型：sysfs/tmpfs/proc/cgroup2/debugfs/...；非 nodev 的是磁盘型：ext4/ext3/ext2/squashfs/vfat。

### dentry vs inode 铁证（硬链接）

```sh
/mnt # echo hello > a.txt
/mnt # ln a.txt b.txt
/mnt # ls -li a.txt b.txt
15 -rw-r--r-- 2 root root 6 a.txt
15 -rw-r--r-- 2 root root 6 b.txt
```

- 两个文件 **inode 号都是 15** → 同一个 `inode`
- 硬链接数 `i_nlink` 从 1 变 **2** → 两个名字指向同一文件

**这就是 dentry 与 inode 分离的直接证据**：两个 `dentry`（两个名字），一个 `inode`（文件本体）。

删除逻辑（inode 生命周期）：

```sh
rm a.txt   # i_nlink: 2→1，inode 还在
rm b.txt   # i_nlink: 1→0，inode 才释放
```

### file（`/proc/self/fd/`）

```
0 -> /dev/ttyAMA0   # stdin（串口控制台）
1 -> /dev/ttyAMA0   # stdout
2 -> /dev/ttyAMA0   # stderr
3                   # ls 列目录时自己打开的目录 fd，列完即关
```

fd 是进程文件描述符表的索引，每个 fd 对应一个 `struct file`。

## 七、要点总结

1. **四大对象职责分明**：挂载（super_block）/ 文件（inode）/ 名字（dentry）/ 打开（file）。
2. **多态靠函数指针表**：`s_op`/`i_op`/`d_op`/`f_op` 各司其职。
3. **dentry 是缓存**：纯内存、可回收，路径解析的加速结构；inode 才是磁盘持久对象。
4. **硬链接 = 多 dentry 共享一个 inode**：`i_nlink` 归零 inode 才释放。
5. **页缓存 `address_space` 是 mm 与 fs 的交汇点**，通过 `inode->i_mapping` 和 `file->f_mapping` 进入。
