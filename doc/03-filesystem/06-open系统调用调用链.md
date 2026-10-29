# open 系统调用完整调用链：从路径到 struct file

> 文件系统子系列第 6 篇。doc 04 认识了四大对象，doc 05 讲了 pathwalk，本篇把两者**串起来**：一个 `open()` 如何从系统调用走到 `struct file` 诞生。配套 QEMU 实验已跑通（f_pos 独立性铁证）。

## 一、open 的六步总览

```
用户: fd = open("/etc/passwd", O_RDONLY)
  ├─ ① build_open_flags  — 解析 flags 成内核 open_flags
  ├─ ② getname           — 路径字符串从用户态拷贝进内核
  ├─ ③ get_unused_fd_flags — 先占一个空 fd 号
  ├─ ④ do_filp_open      — 核心：pathwalk + 打开，造出 struct file
  ├─ ⑤ fd_install        — 把 file 装进进程 fd 表
  └─ 返回 fd
```

## 二、主驱动：`do_sys_openat2`（`fs/open.c:1405`）

```c
static long do_sys_openat2(int dfd, const char __user *filename, struct open_how *how)
{
	struct open_flags op;
	int fd = build_open_flags(how, &op);     // ① 解析 flags
	struct filename *tmp;

	tmp = getname(filename);                 // ② 拷贝路径
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	fd = get_unused_fd_flags(how->flags);    // ③ 先分配空 fd 号
	if (fd >= 0) {
		struct file *f = do_filp_open(dfd, tmp, &op);  // ④ 打开文件
		if (IS_ERR(f)) {
			put_unused_fd(fd);           // 失败回收 fd
			fd = PTR_ERR(f);
		} else {
			fd_install(fd, f);           // ⑤ 装进 fd 表
		}
	}
	putname(tmp);
	return fd;
}
```

顺序设计：**先分配 fd 号，再打开文件**——打开失败只需回收 fd 号，不泄露。

## 三、核心：`do_filp_open` → `path_openat`（`fs/namei.c:4019`）

```c
struct file *do_filp_open(int dfd, struct filename *pathname, const struct open_flags *op)
{
	struct nameidata nd;
	int flags = op->lookup_flags;
	struct file *filp;

	set_nameidata(&nd, dfd, pathname, NULL);
	filp = path_openat(&nd, op, flags | LOOKUP_RCU);   // 先 RCU 无锁
	if (unlikely(filp == ERR_PTR(-ECHILD)))
		filp = path_openat(&nd, op, flags);            // 并发冲突回退
	if (unlikely(filp == ERR_PTR(-ESTALE)))
		filp = path_openat(&nd, op, flags | LOOKUP_REVAL); // 陈旧重查
	restore_nameidata();
	return filp;
}
```

`path_openat`（`fs/namei.c:3980`）三步：

```c
file = alloc_empty_file(op->open_flag, current_cred());  // 先造空壳 file

const char *s = path_init(nd, flags);                    // pathwalk 起点
while (!(error = link_path_walk(s, nd)) &&
       (s = open_last_lookups(nd, file, op)) != NULL)    // pathwalk 主循环
	;
if (!error)
	error = do_open(nd, file, op);                     // ★ 真正打开
```

**先 alloc_empty_file 造空壳，再用 doc 05 的 pathwalk 填 f_path，最后 do_open 绑定 inode。**

## 四、最核心一行：`do_dentry_open`（`fs/open.c:901`）

`do_open`（`fs/namei.c:3793`）→ `vfs_open` → `do_dentry_open`：

```c
static int do_dentry_open(struct file *f, int (*open)(struct inode *, struct file *))
{
	struct inode *inode = f->f_path.dentry->d_inode;

	f->f_inode = inode;                // file 指向文件本体
	f->f_mapping = inode->i_mapping;   // 共享页缓存

	f->f_op = fops_get(inode->i_fop);  // ★★★ 多态落地：f_op 从 inode 挂到 file

	if (!open)
		open = f->f_op->open;
	if (open)
		error = open(inode, f);       // 调具体文件系统的 open（可选）
	f->f_mode |= FMODE_OPENED;
	...
}
```

**`f->f_op = fops_get(inode->i_fop)` 是 VFS 多态的具体落地**：

- ext4 的 inode 挂 `ext4_file_operations`（其 `i_fop`）
- 打开时复制到 `file->f_op`
- 之后 `read(fd)` 走 `file->f_op->read`，就是 ext4 实现

同一个 inode 被多个进程打开，每个 file 复制一份 `f_op` 指针，但指向**同一张**函数表。

## 五、收尾：`fd_install`（`fs/file.c:595`）

把 `struct file` 指针塞进进程的 `files_struct->fd` 数组，返回的 fd 就是数组索引。

## 六、完整调用链

```
open(path, flags)
 → SYSCALL_DEFINE3(open)          fs/open.c:1440
   → do_sys_open                   fs/open.c:1433
     → do_sys_openat2              fs/open.c:1405
       → get_unused_fd_flags       fs/file.c:555
       → do_filp_open              fs/namei.c:4019
         → path_openat             fs/namei.c:3980
           → alloc_empty_file      （空壳 file）
           → path_init + link_path_walk  （doc 05 pathwalk）
           → do_open               fs/namei.c:3793
             → may_open            （权限检查）
             → vfs_open            fs/open.c:1089
               → do_dentry_open    fs/open.c:901
                 → f->f_op = inode->i_fop   ★多态落地
                 → f_op->open()    （具体文件系统）
       → fd_install                fs/file.c:595
```

## 七、QEMU 实测

### slab 命名陷阱

`grep '^file'` 抓到的是 `files_cache`，但**那不是 `struct file`**：

```c
// fs/file_table.c:566
filp_cachep = kmem_cache_create("filp", sizeof(struct file), ...);   // struct file → "filp"
// kernel/fork.c:3203
files_cachep = kmem_cache_create("files_cache", ...);                 // struct files_struct → "files_cache"
```

| slab 名 | 实际对象 | 你的输出 |
|---------|---------|---------|
| `files_cache` | `struct files_struct`（fd 表） | 161 个，704 字节 |
| `filp` | `struct file`（打开的文件） | grep '^file' 抓不到 |

`files_struct` 是"fd→file 映射数组"，`file` 才是"一次打开"。观察 struct file 要 `grep '^filp'`。

### f_pos 独立性铁证

```sh
exec 3</etc/passwd      # fd 3 = 第一个 struct file
exec 4</etc/passwd      # fd 4 = 第二个 struct file

head -c 5 <&3; echo     # root:   ← fd3 偏移 0-4
head -c 5 <&4; echo     # root:   ← fd4 也偏移 0-4（独立！）
head -c 5 <&3; echo     # x:0:0   ← fd3 偏移 5-9（继续）
```

`/etc/passwd` = `root:x:0:0:root:/root:/bin/sh`，所以：

1. **两个 fd（两个 file）f_pos 独立**——fd4 读到 `root:`，不是接着 fd3 的位置。
2. **同一 fd（同一 file）f_pos 推进**——fd3 第二次读到 `x:0:0`。

这就是 `struct file` 必须与 inode 分离的原因：**它记录"这次打开"的私有状态（f_pos、f_mode）**。

## 八、要点总结

1. **open 六步**：解析 flags → 拷贝路径 → 分配 fd → pathwalk → 绑定 inode → 装 fd 表。
2. **先分配 fd 再打开文件**：失败只需回收 fd，不泄露。
3. **`f->f_op = inode->i_fop` 是多态落地**：inode 上文件系统填的函数表，复制到 file 供 read/write 调用。
4. **`f_mapping = i_mapping`**：file 与 inode 共享页缓存 address_space（mm 与 fs 交汇点）。
5. **RCU 优先**：do_filp_open 先无锁，`-ECHILD` 回退非 RCU，`-ESTALE` 强制重查。
6. **f_pos 每次打开独立**：两个 fd 指向同一 inode，但各有各的读写位置。
