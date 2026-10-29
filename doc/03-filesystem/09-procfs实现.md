# procfs 实现：没有磁盘的文件系统

> 文件系统子系列第 9 篇（真正收官）。doc 04~08 讲 VFS 抽象 + ext4 磁盘布局，本篇补上另一类文件系统：**procfs 没有磁盘，读文件 = 现场调用内核函数生成数据**。配套 QEMU 实验已跑通（uptime 两次不同、proc_inode_cache 实测）。

## 一、核心思想：把内核数据结构"投影"成文件

```
ext4：读文件 = 读磁盘块（doc 08）
procfs：读文件 = 现场调用内核函数生成数据

cat /proc/meminfo
  → 不读磁盘！内核现场遍历全局内存统计，seq_printf 拼字符串给你
```

`/proc` 里的每个文件，本质是**内核某个数据结构的投影**。这是理解"文件系统不一定在磁盘上"的关键。

## 二、数据结构：`struct proc_dir_entry`（PDE）

每个 `/proc` 文件/目录在内存里对应一个 PDE：

```c
struct proc_dir_entry {
	char	*name;           // 文件名
	umode_t	mode;            // 权限
	const struct proc_ops *proc_ops;   // ★ procfs 自己的函数表
	const struct inode_operations *proc_iops;
	void	*data;           // 私有数据
	...
};
```

## 三、注册：`proc_create`（`fs/proc/generic.c:597`）

```c
struct proc_dir_entry *proc_create(const char *name, umode_t mode,
				   struct proc_dir_entry *parent,
				   const struct proc_ops *proc_ops)
```

以 `/proc/meminfo` 为例（`fs/proc/meminfo.c:177`）：

```c
pde = proc_create_single("meminfo", 0, NULL, meminfo_proc_show);
```

一行：创建 "meminfo" 的 PDE，绑定 `meminfo_proc_show`。之后 `cat /proc/meminfo` 就调它。

## 四、inode 现场造（`fs/proc/inode.c`）

ext4 的 inode 从磁盘读；procfs 的 inode 现场 `kmem_cache` 分配：

```c
static struct inode *proc_alloc_inode(struct super_block *sb)
{
	struct proc_inode *ei;
	ei = alloc_inode_sb(sb, proc_inode_cachep, GFP_KERNEL);
	ei->pid = NULL;
	ei->pde = NULL;   // inode 里存 PDE 指针，反查"我是哪个 proc 文件"
	...
	return &ei->vfs_inode;
}
```

`struct proc_inode` 内嵌 `struct vfs_inode`，额外存 PDE 指针。

## 五、两层函数表（procfs 的精髓）

```c
// 第一层：VFS 统一入口（所有 proc 文件共用）
static const struct file_operations proc_reg_file_ops = {
	.read	= proc_reg_read,      // 统一分发器
	.open	= proc_reg_open,
	...
};

// proc_reg_read 里转发到第二层
static ssize_t proc_reg_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct proc_dir_entry *pde = PDE(file_inode(file));
	return pde->proc_ops->proc_read(file, buf, count, ppos);  // ★ 转发
}
```

```
vfs_read → file->f_op->read          (proc_reg_read，统一)
              ↓ 查 inode 里的 PDE
         → pde->proc_ops->proc_read  (每个 proc 文件自己填)
              ↓
         → meminfo_proc_show / seq_read
```

## 六、seq_file：边读边生成（`fs/seq_file.c`）

```c
static int meminfo_proc_show(struct seq_file *m, void *v)
{
	struct sysinfo i;
	si_meminfo(&i);          // 现场采集内核统计
	...
	show_val_kb(m, "MemTotal:       ", i.totalram);   // 现场拼字符串
	show_val_kb(m, "MemFree:        ", i.freeram);
	show_val_kb(m, "Cached:         ", cached);
	...
}
```

每次 `cat /proc/meminfo`，`si_meminfo()` 重新跑、`seq_printf` 重新拼——**所以 /proc 数据永远是最新的**。

## 七、ext4 vs procfs

| | ext4（doc 08） | procfs |
|---|---|---|
| inode 从哪来 | 读磁盘 inode 表 | 现场 kmem_cache 分配 |
| 数据从哪来 | 读磁盘数据块 | 现场调函数生成 |
| read 最终到 | ext4_file_read_iter → 读块 | pde->proc_ops->proc_read → seq_printf |
| 有页缓存吗 | 有 | 基本没有（数据瞬态） |

## 八、QEMU 实测

### uptime：现场生成铁证

```sh
cat /proc/uptime; sleep 1; cat /proc/uptime
# 11660.26 46562.47
# 11661.26 46566.46   ← 正好 +1 秒
```

两次读不同——每次 read 都现场调 `ktime_get_boottime_seconds()`。

### self：动态符号链接

```sh
readlink /proc/self
# 210                  ← 当前 shell 的 PID
```

每个进程访问 `/proc/self`，内核现场算出"你自己的 PID"返回。

### meminfo：实时状态

```sh
cat /proc/meminfo | head -5
# MemFree: 8368 kB      ← 之前 mem_hog 500 还在驻留，吃掉 500MB
```

MemFree 只有 8MB 不是 bug，是 `mem_hog` 后台驻留的实时反映——证明数据是现场采集的。

### proc_inode_cache：inode 现场造的实体

```sh
cat /proc/slabinfo | grep -iE "proc|pid"
# proc_inode_cache  80  80  784  20  4
```

- 80 个活跃对象 = 当前内存里的 proc inode
- 784 字节 = sizeof(struct proc_inode)

## 九、要点总结

1. **procfs 无磁盘**：文件是内核数据结构的投影，读 = 现场生成。
2. **两层函数表**：VFS 的 `file_operations`（统一分发）→ 每个 PDE 的 `proc_ops`（具体实现）。
3. **inode 现场造**：`proc_alloc_inode` 从 slab 分配，inode 里存 PDE 指针反查。
4. **seq_file 边读边生成**：不一次性放内存，`seq_printf` 流式输出。
5. **数据永远最新**：每次 read 都重新采集（uptime 两次不同是铁证）。
6. **写内核模块 = 填 proc_ops/file_operations**：这是阶段四的天然入口。
