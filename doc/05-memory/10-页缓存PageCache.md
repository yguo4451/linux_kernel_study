# 页缓存 Page Cache：内存与文件的枢纽

> 内存管理子系列第 10 篇。前九篇讲的是"匿名页"（malloc 出来的内存）的一生。本篇引入第二大类页：**文件页（file-backed page）**。它不归某个进程私有，而是挂在文件的 `address_space` 下，被所有读这个文件的人共享——这就是页缓存。

## 一、概念：为什么要有页缓存

磁盘慢（μs~ms 级），RAM 快（ns 级）。页缓存用**空闲内存**缓存文件内容，让第二次读同一个文件不再打磁盘：

```
第一次读: 磁盘 → 页缓存 → 用户        （慢，读盘）
第二次读: 页缓存 → 用户                （快，命中）
```

`/proc/meminfo` 里的 `Cached` 就是它占用的内存。它和匿名页一起，构成 Linux 内存管理的两条主线：

| 页类型 | 归属 | 换出时 | 回收时 |
|--------|------|--------|--------|
| 匿名页 | 进程私有 | 写 swap 分区 | 需要 swap |
| **文件页** | `address_space` 共享 | 脏页写回文件，干净页直接丢弃 | 无需 swap |

## 二、核心对象：`struct address_space`

页缓存不是"一块内存池"，而是**挂在每个 inode 上的页索引**（`include/linux/fs.h`）：

```c
struct address_space {
    struct inode *host;          // 所属文件
    struct xarray i_pages;       // ★ 页缓存本体：文件偏移 → struct folio 的映射树
    unsigned long nrpages;       // 缓存的页数
    const struct address_space_operations *a_ops;  // 具体文件系统的读写回调
    ...
};
```

- **`i_pages`** 是一棵 xarray（radix tree 的后继），键是"页在文件中的下标"（`offset >> PAGE_SHIFT`），值是 `struct folio *`。O(1) 查找某段文件是否已缓存。
- **`a_ops`** 抽象了不同文件系统的差异：ext4 的 `read_folio` 走块层读磁盘，tmpfs 的 `read_folio` 直接返回。

## 三、读路径（`mm/filemap.c`）

调用链：`read()` → `generic_file_read_iter` → `filemap_read` → `filemap_get_pages` → `filemap_create_folio`：

```c
// generic_file_read_iter：先处理 direct I/O，否则走页缓存
if (iocb->ki_flags & IOCB_DIRECT) { ... direct_IO ... }
return filemap_read(iocb, iter, retval);   // 页缓存路径

// filemap_get_pages：批量取页
filemap_get_read_batch(mapping, index, last_index - 1, fbatch);  // ① 查 i_pages 命中
if (!folio_batch_count(fbatch)) {                                // ② 未命中
    page_cache_sync_readahead(...);                              //    先同步预读
    filemap_get_read_batch(...);
}
if (!folio_batch_count(fbatch)) {                                // ③ 还是空 → 建新页
    filemap_create_folio(filp, mapping, pos, fbatch);            //    分配 folio + 插进 xarray
}                                                                 //    + aops->read_folio 读盘

// filemap_create_folio：核心两步
folio = filemap_alloc_folio(...);                    // 分配物理页
filemap_add_folio(mapping, folio, index, gfp);       // 插进 i_pages
filemap_read_folio(file, mapping->a_ops->read_folio, folio);  // 从磁盘读入 → PG_uptodate
```

关键点：**命中和未命中的分水岭就是 `i_pages` xarray 里有没有这个 offset 的 folio**。`filemap_add_folio` 用 `xas_store` 把 folio 存进去，之后所有人再读同一段就命中。

## 四、写路径（`mm/filemap.c`）

调用链：`write()` → `generic_file_write_iter` → `__generic_file_write_iter` → `generic_perform_write`：

```c
ssize_t generic_perform_write(struct kiocb *iocb, struct iov_iter *i)
{
    do {
        balance_dirty_pages_ratelimited(mapping);        // ① 脏页超限？节流
        status = a_ops->write_begin(file, mapping, pos,  // ② 取/建页
                        bytes, &folio, &fsdata);
        copied = copy_folio_from_iter_atomic(folio, ...); // ③ 数据拷进页
        status = a_ops->write_end(file, mapping, pos, ...); // ④ 标记 PG_dirty
    } while (iov_iter_count(i));
}
```

**核心陷阱：`write()` 返回 ≠ 数据落盘**。写只把数据拷进页缓存并标脏（`PG_dirty`），真正写回磁盘由 writeback 线程异步做（下一篇）。所以 `dd` 能跑到 1.6GB/s——因为它写的是 RAM，不是磁盘。

## 五、实验验证（QEMU + ext4 虚拟磁盘）

环境：`build/disk.img`（64MB ext4）经 `-drive file=...,if=virtio` 挂载，guest 内 `mount /dev/vda /mnt`。

```sh
cat /proc/meminfo | grep -E "^Cached|^Dirty"      # ② 基线
dd if=/dev/zero of=/mnt/big bs=1M count=32         # ③ 写 32MB
cat /proc/meminfo | grep -E "^Cached|^Dirty"
echo 3 > /proc/sys/vm/drop_caches                   # ④ 释放页缓存
cat /proc/meminfo | grep -E "^Cached"
cat /mnt/big > /dev/null                            # ⑤ 读一遍
cat /proc/meminfo | grep -E "^Cached"
cat /mnt/big > /dev/null                            # ⑥ 再读
cat /proc/meminfo | grep -E "^Cached"
```

### 实测数据

| 步骤 | Cached (kB) | Dirty (kB) | 解读 |
|------|------------|-----------|------|
| ② 基线 | 13692 | 0 | 还没缓存文件数据 |
| ③ 写 32MB | 46460 | 4120 | **+32768 kB = 精确 32MB**，写入进了页缓存 |
| ④ drop_caches | 17780 | — | 掉了 28680 kB，但**没回到基线** |
| ⑤ 读一遍 | 46452 | — | 涨回 +28672 kB，页被重新缓存 |
| ⑥ 再读 | 46452 | — | **0 变化 = 命中缓存** |

### 三个关键结论

1. **写 = 进缓存**：`46460 − 13692 = 32768 kB`，一分不差等于 32MB（32×1024）。`dd` 写的每个字节都先落在页缓存。
2. **读 = 命中即复用**：⑤ 涨回 46452，⑥ 纹丝不动——第二次读完全命中缓存，没有新增页。
3. **★ drop_caches 只释放"干净页"**：④ 之后 Cached 是 17780 而非基线 13692，差 `17780 − 13692 = 4088 kB`，恰好 ≈ ③ 时刻的 `Dirty: 4120 kB`。这 4MB **脏页还没回写完，躲过了 drop_caches**——内核不能直接丢脏页，否则数据丢失。先回写、变干净，下次 drop 才会释放。

`drop_caches` 三个值（`fs/drop_caches.c` 的 `drop_caches_sysctl_handler`）：
- `1` = 释放文件页缓存（干净页）
- `2` = 释放 dentry + inode 缓存
- `3` = 两者都释放

> 内核日志 `sh (121): drop_caches: 3` 就是这条 sysctl 的审计输出——生产环境用它清缓存会被打日志，便于追溯"谁手贱清了缓存"。

## 六、陷阱

1. **`Cached` 高 ≠ 泄漏**：页缓存随用随还，内存紧张时 kswapd 优先回收干净文件页。看到 Cached 占满内存通常是好事（缓存利用率高）。
2. **`write()` 不保证落盘**：数据在 RAM，断电可能丢。数据库靠 `fsync` 强制刷盘。
3. **drop_caches 清不掉脏页**：脏页必须先 writeback。想强制立即落盘用 `sync`。
4. **页缓存是共享的**：多个进程读同一个文件共享同一批页，这也是 `mmap` 零拷贝的基础（下一篇）。

## 七、衔接下一步

页缓存讲清了"文件页"的存储。但还有两个问题没回答：

1. **反向映射 rmap**：一个文件页可能被多个进程 mmap 到不同地址，回收/迁移时怎么找到所有引用它的页表项？
2. **写回 Writeback**：那些 `PG_dirty` 页到底什么时候、由谁、按什么顺序写回磁盘？

下一篇先讲 **rmap 反向映射**——它是 swap、页迁移、写回共同依赖的基础设施。
