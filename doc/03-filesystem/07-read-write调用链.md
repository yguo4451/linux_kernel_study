# read/write 调用链：从 f_op 到页缓存

> 文件系统子系列第 7 篇。doc 06 把 `f_op` 挂到了 file 上，本篇看它**第一次被真正调用**：`read()` 沿着函数指针表走到页缓存，把内存管理和文件系统两个子系统在 `address_space` 处焊死。配套 QEMU 实验已跑通（页缓存填充实测）。

## 一、read 调用链总览

```
read(fd, buf, count)
 → SYSCALL_DEFINE3(read)            fs/read_write.c:720
   → ksys_read                      fs/read_write.c:701
     → vfs_read                     fs/read_write.c:549
       → file->f_op->read_iter      ★ doc 06 挂的函数表，这里被调用
         → ext4_file_read_iter      fs/ext4/file.c:130
           → generic_file_read_iter mm/filemap.c:2805
             → filemap_read         mm/filemap.c:2623   ← 页缓存
```

## 二、vfs_read：多态的调用点（`fs/read_write.c:549`）

```c
ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	...
	if (file->f_op->read)
		ret = file->f_op->read(file, buf, count, pos);
	else if (file->f_op->read_iter)
		ret = new_sync_read(file, buf, count, pos);   // 现代文件系统走这条
	else
		ret = -EINVAL;
	...
}
```

**doc 06 的 `f->f_op = fops_get(inode->i_fop)` 在这里兑现**：ext4 的 inode 挂 `ext4_file_operations`，所以 `file->f_op->read_iter` 就是 `ext4_file_read_iter`。

## 三、ext4 函数表与转发（`fs/ext4/file.c:931`）

```c
const struct file_operations ext4_file_operations = {
	.llseek		= ext4_llseek,
	.read_iter	= ext4_file_read_iter,      // read 走这里
	.write_iter	= ext4_file_write_iter,     // write 走这里
	.mmap		= ext4_file_mmap,
	.open		= ext4_file_open,
	...
};

static ssize_t ext4_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	...
	return generic_file_read_iter(iocb, to);   // 通用页缓存读
}
```

**分层之美**：具体文件系统只填"特殊情况"，普通读交给通用页缓存逻辑 `generic_file_read_iter`。

## 四、页缓存核心：filemap_read（`mm/filemap.c:2623`）

```c
ssize_t filemap_read(struct kiocb *iocb, struct iov_iter *iter, ssize_t already_read)
{
	struct file *filp = iocb->ki_filp;
	struct address_space *mapping = filp->f_mapping;   // ★ 页缓存入口

	do {
		error = filemap_get_pages(iocb, iter->count, &fbatch, false);
		// ↑ 页缓存(xarray)里找；miss 触发 readahead + 磁盘 I/O

		for (i = 0; i < folio_batch_count(&fbatch); i++) {
			copied = copy_folio_to_iter(folio, offset, bytes, iter);
			// ↑ 页缓存内容拷贝到用户缓冲区
			iocb->ki_pos += copied;   // 推进 f_pos
		}
	} while (...);
}
```

**关键连接**：`filp->f_mapping` 就是 doc 06 `do_dentry_open` 赋值的 `inode->i_mapping`——**页缓存 `address_space` 是文件系统和内存管理的交汇点**。

- **命中**：`copy_folio_to_iter` 直接从内存拷给用户，零磁盘 I/O
- **未命中**：`filemap_get_pages` 触发 readahead，从磁盘读进页缓存

## 五、write 对称（`fs/read_write.c:663`）

```c
ssize_t vfs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	...
	file_start_write(file);
	if (file->f_op->write)
		ret = file->f_op->write(...);
	else if (file->f_op->write_iter)
		ret = new_sync_write(...);      // → ext4_file_write_iter
	...
}
```

写路径：`ext4_file_write_iter` → `generic_file_write_iter` → 写进**页缓存（脏页）**，数据先不落盘，等 writeback（doc 05-memory/12）刷回磁盘。

## 六、f_pos 在这里被读写（`fs/read_write.c:701`）

```c
ssize_t ksys_read(unsigned int fd, char __user *buf, size_t count)
{
	struct fd f = fdget_pos(fd);
	loff_t pos, *ppos = file_ppos(fd_file(f));   // &file->f_pos
	if (ppos) {
		pos = *ppos;      // 读 f_pos 起点
		ppos = &pos;
	}
	ret = vfs_read(fd_file(f), buf, count, ppos);
	if (ret >= 0 && ppos)
		fd_file(f)->f_pos = pos;   // 写回新位置
	...
}
```

doc 06 里两个 fd 各自独立读 `root:` 的机制就在这里：**read 读 f_pos 起点、读完写回新位置**。

## 七、QEMU 实测

```sh
dd if=/dev/zero of=/mnt/bigfile bs=1M count=20
# 20971520 bytes (20.0MB) copied, 956.3MB/s   ← 写进页缓存，不是真磁盘写

echo 3 > /proc/sys/vm/drop_caches
grep -E "^Cached" /proc/meminfo
# Cached: 3728 kB         ← 基线

cat /mnt/bigfile > /dev/null && grep -E "^Cached" /proc/meminfo
# Cached: 24144 kB        ← 一次读就缓存满 20MB（基线 3720 + 20424）
```

### 两个彩蛋

1. **`dd` 956MB/s**：写的是页缓存脏页，不是真磁盘写——"写入完成"≠"落盘完成"，中间隔着 writeback。
2. **一次 read 就完整填充页缓存**：`filemap_read` 逐页把文件读进 `address_space`，读完 20MB 就缓存 20MB，无需第二次读。

> 踩坑记录：曾在 `dd` 刚写完（脏页）后立刻 `drop_caches` + `cat`，观察到第一次只缓存 16MB、第二次补 4MB，一度误判为"预读窗口不会一次拉完整文件"。后经干净重跑（`cat && grep` 立即采样）证实：**一次读就缓存满**，之前的 16+4 是 `dd` 脏页 + writeback + drop_caches 竞态的采样假象，不是 readahead 的普遍行为。

## 八、要点总结

1. **read/write 是 f_op 多态的兑现处**：`vfs_read` 调 `file->f_op->read_iter`，ext4 填的是 `ext4_file_read_iter`。
2. **普通读交给通用页缓存逻辑**：具体文件系统只填特殊情况，`generic_file_read_iter` 兜底。
3. **页缓存 `address_space` 是 mm/fs 交汇点**：`filp->f_mapping == inode->i_mapping`。
4. **读填充页缓存，写也先进页缓存**：读 miss 触发磁盘 I/O，写是脏页 + 异步 writeback。
5. **f_pos 在 read/write 中被读写**：这是 doc 06 两个 fd 独立位置的根本原因。
