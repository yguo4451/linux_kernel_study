# 21. 第一个内核模块 hello.ko 与 taint 机制

> 阶段四开篇。从"读内核"升级到"写内核"：编写、编译、加载、卸载第一个内核模块，理解模块生命周期与内核 taint 污染标记。

## 一、模块 vs 用户态程序

| | 用户态程序 | 内核模块 |
|---|---|---|
| 入口 | `main()` | `module_init()` 指定的函数 |
| 退出 | `return` | `module_exit()` 指定的函数 |
| 库 | libc（printf） | 内核 API（printk） |
| 出错后果 | 进程崩溃 | **整个内核崩溃** |
| 加载方式 | `execve` | `insmod` |

模块与内核运行在同一个地址空间、同一个特权级，没有任何保护屏障——一个野指针就能让系统挂掉。这是模块开发的"敬畏感"来源。

## 二、最小模块的结构（hello.c）

```c
#include <linux/module.h>
#include <linux/kernel.h>

static int hello_init(void) {
    printk(KERN_INFO "hello init...\n");
    return 0;
}

static void hello_exit(void) {
    printk(KERN_INFO "hello exit...\n");
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
```

四个必备元素：

1. **头文件**：`<linux/module.h>`（模块宏）+ `<linux/kernel.h>`（printk）
2. **init 函数**：返回 `int`，成功返回 0、失败返回负错误码
3. **exit 函数**：返回 `void`
4. **注册 + 声明**：`module_init()` / `module_exit()` / `MODULE_LICENSE("GPL")`

## 三、编译环境：三个先决条件

### 1. Makefile 结构

```makefile
KDIR := /tmp/kbuild

obj-m += hello.o
all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean
```

- `obj-m`：声明要编译成模块（`obj-y` 是编进内核）
- `-C $(KDIR) M=$(PWD) modules`：进入内核构建树，用 `M=` 指定模块目录
- **命令行必须 TAB 缩进**（空格报 `missing separator`）

### 2. KDIR 指向的内核构建树

需要 `make modules_prepare` 生成头文件树 + scripts。本项目用 `O=/tmp/kbuild` 分离构建，KDIR 指向它。

### 3. Module.symvers（最易漏的一环）

`make modules_prepare` **不生成** `Module.symvers`，导致外部模块 modpost 报 `_printk undefined`。正确生成（几秒）：

```bash
cd /tmp/kbuild && scripts/mod/modpost -M -E -o Module.symvers vmlinux.o
```

- `-M` 是关键：告诉 modpost "处理 vmlinux 的导出符号"（漏了会生成 0 字节空文件）
- `printk` 在 arm64 上是宏，真实符号是 `_printk`，由内核 `EXPORT_SYMBOL` 导出

## 四、加载链路：insmod 发生了什么

```
宿主机 scp hello.ko 进系统
        ↓
insmod hello.ko
        ↓ 内核解析 .ko（ELF）→ 分配内存 → 重定位符号
        ↓
调用 module_init 注册的 hello_init()
        ↓
printk(KERN_INFO "hello init...\n")   → 内核日志缓冲 → dmesg 可见
        ↓
模块常驻内核，lsmod 可见
```

## 五、关键现象：taint 污染标记

```bash
# insmod 时 dmesg 第一行：
hello: loading out-of-tree module taints kernel.
```

这是内核的 **taint（污染）机制**：加载"树外模块"（out-of-tree，不属于内核源码树的第三方模块）时，内核打上污染标记。

**为什么**：内核维护者收到 bug 报告时，如果内核被 taint，就知道加载过第三方代码，bug 可能来自第三方而非内核本身。taint 标记是"免责声明"。

```bash
# 查看 taint 状态
cat /proc/sys/kernel/tainted
# 0 = 未被污染；非 0 表示被污染（bit 12 = OOT 模块）
```

`lsmod` 里模块名后的 `(O)` 就是 Out-of-tree 标记。

## 六、lsmod 输出解读

```bash
~ # lsmod
hello 12288 0 - Live 0xffffa0905d900000 (O)
```

| 字段 | 含义 |
|------|------|
| `hello` | 模块名 |
| `12288` | 模块占用内存（字节，12KB） |
| `0` | 引用计数（有多少模块依赖它，0 = 无人使用） |
| `-` | 加载状态（`-` 未使用 / `Live` 活跃） |
| `Live` | 模块状态（可加载/卸载） |
| `0xffffa0905d900000` | 模块在内核地址空间的起始地址 |
| `(O)` | Out-of-tree 树外模块标记 |

`lsmod` 的底层数据源是 `/proc/modules`，可以 `cat /proc/modules` 对照看。

## 七、踩坑记录

1. **`void hello_exit()` 编译失败**：内核开 `-Werror=strict-prototypes`，`()` 被当错误，必须 `(void)`。
2. **`MODULE_LICENSE("GPL")` 缺分号**：宏展开是变量定义，需以 `;` 结尾。
3. **`-Wmissing-prototypes` 警告**：init/exit 函数建议加 `static`。
4. **`_printk undefined`**：缺 `Module.symvers`，modpost 无法校验导出符号。
5. **Makefile `missing separator`**：命令行没用 TAB。

## 八、下一课预告

`hello.ko` 的 ELF 内部结构（`.modinfo`/`.text`/`.rodata` 段）、`insmod` 的完整调用链（`init_module` 系统调用 → `load_module` → `do_init_module`）、以及 `__init`/`__exit` 宏如何让 init 代码在运行后释放内存。
