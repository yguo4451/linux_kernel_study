# sysfs 与 kobject：内核对象模型的投影

> 文件系统子系列第 10 篇。doc 09 讲了 procfs（内核信息的报表），本篇讲另一个虚拟文件系统 **sysfs**：它投影的是**内核对象模型 kobject**，铁律是"一个文件一个值"。这是进入设备驱动（阶段四）的必经之路。

## 一、核心思想：sysfs = kobject 树的投影

procfs 投影"内核信息"，sysfs 投影"内核**对象**"：

```
/sys/devices/...   ← 设备对象
/sys/class/...     ← 类对象
/sys/bus/...       ← 总线对象
/sys/block/vda/    ← 块设备对象
```

每个目录 = 一个 `kobject`（内核对象），每个文件 = 一个 `attribute`（对象属性）。**创建设备时 `kobject_add` 自动生成目录，`device_create_file` 自动生成文件**——不需要像 procfs 那样手写 `proc_create`。

## 二、核心数据结构

### `struct kobject`（`include/linux/kobject.h:64`）

```c
struct kobject {
	const char	*name;       // 对象名 = 目录名
	struct kobject	*parent;     // 父对象 = 父目录
	struct kset	*kset;       // 所属集合
	const struct kobj_type *ktype;  // 类型（决定 show/store）
	struct kernfs_node *sd;     // ★ sysfs 目录项（kernfs）
	struct kref	kref;        // 引用计数
	...
};
```

`sd` 是 kernfs 节点——sysfs 是 **kernfs** 框架的用户（procfs 是 proc_dir_entry，sysfs 是 kernfs_node）。

### `struct kobj_type`（`include/linux/kobject.h:116`）

```c
struct kobj_type {
	void (*release)(struct kobject *kobj);
	const struct sysfs_ops *sysfs_ops;      // ★ show/store 函数表
	const struct attribute_group **default_groups;
	...
};
```

### `struct attribute` + `struct sysfs_ops`（`include/linux/sysfs.h`）

```c
struct attribute {          // 一个文件
	const char	*name;   // 文件名
	umode_t		mode;    // 权限
};

struct sysfs_ops {          // 读写函数表
	ssize_t (*show)(struct kobject *, struct attribute *, char *);
	ssize_t (*store)(struct kobject *, struct attribute *, const char *, size_t);
};
```

## 三、铁律：一个文件一个值（`fs/sysfs/file.c:101`）

```c
static ssize_t sysfs_kf_read(struct kernfs_open_file *of, char *buf, ...)
{
	const struct sysfs_ops *ops = sysfs_file_ops(of->kn);
	struct kobject *kobj = of->kn->parent->priv;

	len = ops->show(kobj, of->kn->priv, buf);   // show 输出一个值
	...
	if (len >= (ssize_t)PAGE_SIZE) {
		printk("fill_read_buffer: %pS returned bad count\n", ops->show);
		len = PAGE_SIZE - 1;   // ★ 硬性限制 < 4KB
	}
	...
}
```

**输出超过 4KB 直接报错**——因为 sysfs 设计铁律是 **one value per file**：

```
/sys/block/vda/size → "262144"   一个数字
/sys/block/vda/dev  → "254:0"    一个设备号
```

对比 procfs 的 `/proc/meminfo`（几十行流式输出），这是两者最本质的区别。

## 四、show/store 读写模型

```c
// 读：show 输出一个值
static ssize_t show_xxx(struct kobject *kobj, struct attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", value);
}

// 写：store 接收一个值
static ssize_t store_xxx(struct kobject *kobj, struct attribute *attr,
			 const char *buf, size_t count)
{
	// 解析 buf 里的值，设置到设备
}
```

procfs 主要**读**；sysfs 是**读 + 写**（`echo 1 > /sys/.../power` 控制设备）。

## 五、procfs vs sysfs 对照

| 维度 | procfs | sysfs |
|------|--------|-------|
| 投影 | 进程 + 内核统计 | 内核对象（设备/驱动） |
| 数据模型 | PDE + proc_ops | kobject + attribute + sysfs_ops |
| 一个文件 | 流式多值（seq_file） | 一个值（< 4KB） |
| 读写 | 主要读 | show/store 读写 |
| 目录来源 | 手写 proc_mkdir | kobject_add 自动 |
| 底层 | proc_dir_entry | kernfs |
| 铁律 | 无 | one value per file |
| 典型 | `cat /proc/meminfo` | `echo 1 > /sys/.../power` |

## 六、QEMU 实测

```sh
ls /sys                    # kobject 树：block/bus/class/devices/fs/kernel/...
ls /sys/block              # 块设备对象
cat /sys/block/vda/size    # 一个数字：块设备扇区数
cat /sys/block/vda/dev     # 一个值：设备号 254:0
```

每个文件读出来都是**一个值**——对比 `cat /proc/meminfo` 的几十行。

## 七、要点总结

1. **sysfs = kobject 树的投影**：目录 = kobject，文件 = attribute。
2. **kobj_type->sysfs_ops 提供 show/store**：类型决定对象的读写行为。
3. **铁律 one value per file**：`sysfs_kf_read` 硬性限制输出 < 4KB。
4. **底层是 kernfs**：procfs 用 proc_dir_entry，sysfs 用 kernfs_node。
5. **写驱动必用 sysfs**：`device_create_file` + `DEVICE_ATTR` 暴露设备属性给用户态。
