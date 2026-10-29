# 路径解析 pathwalk：从字符串到 dentry

> 文件系统子系列第 5 篇。doc 04 认识了 VFS 四大对象，本篇讲它们如何**协作**：内核拿到字符串路径 `/mnt/dir/a.txt`，怎么一步步解析成最终的 dentry + inode。配套 QEMU 实验已跑通（dentry 缓存增长实测）。

## 一、pathwalk 要解决的问题

内核拿到字符串路径，要变成最终的 `struct path`（dentry + mount）：

```
"/mnt/dir/a.txt"
  ├─ 确定起点（绝对路径→root，相对路径→pwd）
  ├─ 逐级切分：mnt → dir → a.txt
  ├─ 每一级"查名字找 dentry"
  └─ 得到 a.txt 的 dentry + inode
```

每查一级名字都可能要**读磁盘**（去 inode 表找）。路径解析是内核最热的路径之一，所以用 **dentry 缓存（dcache）**缓存查过的结果——第二次走同样路径时内存命中，不碰磁盘。这就是 fast path / slow path 的分野。

## 二、核心数据结构：`struct nameidata`（`fs/namei.c:579`）

```c
struct nameidata {
	struct path	path;      // 当前走到哪了（dentry + mount）
	struct qstr	last;      // 当前要解析的名字（含 hash）
	struct path	root;      // 根目录
	struct inode	*inode;    // 当前 inode
	unsigned	depth;     // symlink 嵌套深度
	int		total_link_count;  // symlink 总数（防死循环）
	struct saved *stack, internal[EMBEDDED_LEVELS]; // symlink 栈
	...
};
```

这是 pathwalk 的工作上下文，全程传递。

## 三、主循环：`link_path_walk`（`fs/namei.c:2356`）

简化骨架：

```c
static int link_path_walk(const char *name, struct nameidata *nd)
{
	for(;;) {
		nd->last.name = name;
		name = hash_name(nd, name, &lastword);   // 1. 算名字 hash

		switch(lastword) {                        // 2. 判断分量类型
		case LAST_WORD_IS_DOTDOT: nd->last_type = LAST_DOTDOT; break;
		case LAST_WORD_IS_DOT:    nd->last_type = LAST_DOT;    break;
		default:                  nd->last_type = LAST_NORM;   break;
		}

		if (!*name) goto OK;                     // 3. 到末尾
		name++;                                  // 跳过 '/'
		link = walk_component(nd, WALK_MORE);    // 4. 解析这一级
		if (link) {                              // 5. 遇到 symlink → 压栈
			nd->stack[depth++].name = name;
			name = link;                          // 换目标继续
			continue;
		}
	}
}
```

## 四、fast path vs slow path（核心）

每一级名字查找走 `walk_component`（`fs/namei.c:2049`）：

```c
static const char *walk_component(struct nameidata *nd, int flags)
{
	struct dentry *dentry;
	if (nd->last_type != LAST_NORM)          // . 和 .. 特殊处理
		return handle_dots(nd, nd->last_type);

	dentry = lookup_fast(nd);                // ① 快路径：查 dcache
	if (unlikely(!dentry))
		dentry = lookup_slow(&nd->last, nd->path.dentry, nd->flags); // ② 慢路径
	return step_into(nd, flags, dentry);     // ③ 进入这一级
}
```

### ① lookup_fast（`fs/namei.c:1669`）—— 内存命中

```c
dentry = __d_lookup_rcu(parent, &nd->last, &nd->next_seq); // dcache 哈希表查找
status = d_revalidate(dentry, nd->flags);                  // 文件系统确认还有效
```

- 纯内存查找，**不碰磁盘**；还能走 RCU 无锁模式（`LOOKUP_RCU`），并发 rename 用 seqcount 检测
- 找不到返回 NULL，交给慢路径

### ② lookup_slow → __lookup_slow（`fs/namei.c:1718`）—— 缓存未命中

```c
dentry = d_alloc_parallel(dir, name, &wq);       // 分配新 dentry
old = inode->i_op->lookup(inode, dentry, flags); // ★ 调用具体文件系统的 lookup
```

关键在 `inode->i_op->lookup()`——**VFS 多态落地的地方**。ext4 填自己的 `ext4_lookup`（去磁盘 inode 表找），procfs 填内存查找。查不到就得到 `d_inode == NULL` 的**负 dentry**（negative dentry）。

### ③ step_into（`fs/namei.c:1899`）—— 进入分量

处理挂载点切换、symlink 跟随、更新 `nd->path`。

## 五、负 dentry（negative dentry）

查一个不存在的文件，内核也缓存一个 dentry，只是 `d_inode == NULL`：

```
cat /mnt/nonexistent.txt   # 失败
  → lookup_slow("nonexistent.txt")
    → ext4_lookup 找不到
      → 返回负 dentry（d_inode=NULL）并缓存
```

**为什么值得缓存"不存在"**：程序常反复探测同一路径（如检查配置文件是否存在）。缓存"查过没有"的结论，下次就不用再读磁盘。代价是内存里多一些空壳 dentry。

## 六、symlink 处理

解析到 symlink 时（inode 的 `i_mode == S_IFLNK`），`link_path_walk` 把当前位置压进 `nd->stack`，换成链接目标重新解析。`depth` 和 `total_link_count` 限制嵌套深度（防止 `a→b→a` 死循环）。

## 七、完整调用链

```
open("/mnt/a.txt")
 → sys_open → do_sys_open → do_filp_open
   → path_openat
     → path_lookupat (namei.c:2581)
       → path_init           ← 起点：绝对→root，相对→pwd/dfd
       → link_path_walk      ← 主循环逐级解析
         → walk_component    ← 每级 fast/slow 查找 + step_into
       → lookup_last         ← 最后一级
       → complete_walk       ← 收尾
```

## 八、QEMU 实测：dcache 增长

`/proc/slabinfo` 的 dentry 行（字段：活跃数/总数/对象大小/每slab对象数）：

```sh
# 基线
dentry  1303  1394  240  17  1
#        ↑活   ↑总   ↑240字节

# 创建 100 个文件后
for i in $(seq 1 100); do echo x > /mnt/f$i; done
dentry  1394  1394  240  17  1
# 活跃 1303 → 1394，+91 个 dentry
```

**解读**：

- **100 个文件 → +91 个 dentry**（约 9 个被回收/复用）：每个名字缓存一个 dentry 的直接证据。
- **objsize=240 字节**：dentry 是高频分配/释放的小对象，靠 slab 分配器高效管理（doc 05-memory/02）。
- **负 dentry 单个难观察**：单次失败查找 +1 会被对象复用/快速回收淹没，需批量测试：

```sh
for i in $(seq 1 200); do ls /mnt/neg_$i 2>/dev/null; done  # 200 个负 dentry
cat /proc/slabinfo | grep '^dentry'   # 应涨 ~200
```

## 九、要点总结

1. **pathwalk 是内核最热路径之一**，dcache 为它而生。
2. **fast path（lookup_fast）**：dcache 内存命中，不碰磁盘；**slow path（lookup_slow）**：调 `i_op->lookup` 读磁盘。
3. **负 dentry 缓存"不存在"的结论**，避免重复磁盘查找。
4. **symlink 靠 nameidata 栈**展开，深度受限防死循环。
5. **`i_op->lookup` 是 VFS 多态的落地处**：每种文件系统实现自己的查找。
