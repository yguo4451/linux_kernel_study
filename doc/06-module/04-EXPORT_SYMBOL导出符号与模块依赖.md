# 24. EXPORT_SYMBOL 导出符号与模块依赖

> 阶段四第四课。通过「模块 A 导出函数、模块 B 调用它」的实验，理解 EXPORT_SYMBOL 的完整机制：`__ksymtab` 段、`struct kernel_symbol`、`find_symbol` 二分查找、`ref_module` 建立模块依赖与引用计数。

## 一、实验闭环

模块 A（`my_add`）导出一个函数，模块 B（`hello`）引用并调用：

```
A: int my_add(int a, int b) { return a + b; }
   EXPORT_SYMBOL(my_add);

B: extern int my_add(int a, int b);
   printk("%d\n", my_add(10, 23));   // → 33
```

加载顺序必须 A 先 B 后，否则 B 报 `Unknown symbol my_add`。

## 二、编译期：Module.symvers 是"导出清单"

每个模块编译时，modpost 生成 `Module.symvers`，记录该模块导出的符号：

```
$ cat my_add/Module.symvers
0x00000000      my_add  /work/experiments/module/my_add/my_add  EXPORT_SYMBOL
```

四列：`<CRC> <符号名> <定义文件> <导出方式>`。CRC 为 0 是因为内核未开 `CONFIG_MODVERSIONS`。

**B 引用 A 的符号时，B 的编译必须知道 A 的导出清单**，否则 modpost 报 `my_add undefined`。解法是 `KBUILD_EXTRA_SYMBOLS`：

```makefile
# hello/Makefile
KBUILD_EXTRA_SYMBOLS := /work/experiments/module/my_add/Module.symvers
```

> 注意：相对路径是相对 make 的 cwd（`-C /tmp/kbuild`）解析的，不是相对 Makefile 所在目录。最健壮的写法是用 `$(dir $(lastword $(MAKEFILE_LIST)))` 推导，或直接写绝对路径。

内核自己的 `/tmp/kbuild/Module.symvers` 只含 vmlinux 导出的符号（如 `_printk`），**模块间的符号依赖必须靠各自 Module.symvers 拼接**。

## 三、运行期：符号解析与依赖建立

`insmod hello.ko` 时，`simplify_symbols` 遇到 `SHN_UNDEF` 的 `my_add`：

```
simplify_symbols
  → resolve_symbol_wait
    → resolve_symbol
        → find_symbol        → 在内核 __ksymtab 和各模块 __ksymtab 里找 my_add
        → check_version
        → ref_module(hello, my_add)   ← 建立依赖
```

### find_symbol：二分查找导出符号

```c
// kernel/module/main.c
bool find_symbol(struct find_symbol_arg *fsa)
{
    // 先查内核：__ksymtab / __ksymtab_gpl
    static const struct symsearch arr[] = {
        { __start___ksymtab, __stop___ksymtab, ..., NOT_GPL_ONLY },
        { __start___ksymtab_gpl, __stop___ksymtab_gpl, ..., GPL_ONLY },
    };
    // 再遍历所有已加载模块的 mod->syms / mod->gpl_syms
    list_for_each_entry_rcu(mod, &modules, list) { ... }
}
```

`find_exported_symbol_in_section` 用 `bsearch()` 在 `struct kernel_symbol` 数组里二分查找符号名。`__ksymtab` 就是 `EXPORT_SYMBOL` 生成的段（按名字排序，所以能二分）。

### ref_module：引用计数 + 依赖链

```c
static int ref_module(struct module *a, struct module *b)
{
    err = strong_try_module_get(b);   // ① b->refcnt + 1
    if (err) return err;
    err = add_module_usage(a, b);     // ② 建立 module_use 双向链
    ...
}
```

```c
static int add_module_usage(struct module *a, struct module *b)
{
    use->source = a;                       // hello（使用者）
    use->target = b;                       // my_add（被使用者）
    list_add(&use->source_list, &b->source_list); // my_add 的"谁依赖我"链表
    list_add(&use->target_list, &a->target_list); // hello 的"我依赖谁"链表
}
```

## 四、lsmod 与卸载验证

```
my_add 12288 1 hello, Live 0xffffb6c462904000 (O)
             ↑   ↑
          refcnt  used by（依赖我的模块）
```

- `my_add` refcnt = 1：`strong_try_module_get` 加的（hello 引用它）
- `used by` = `hello,`：遍历 `my_add->source_list`
- `rmmod my_add` → `Resource temporarily unavailable`（-EWOULDBLOCK）：

```c
// delete_module
if (!list_empty(&mod->source_list)) {
    ret = -EWOULDBLOCK;   // "Module my_add is in use"
    goto out;
}
```

卸载顺序：先 `rmmod hello`（触发 `module_put(my_add)`，refcnt 回 0，use 链摘除），再 `rmmod my_add` 成功。

## 五、关键结论

| 阶段 | 机制 | 载体 |
|------|------|------|
| 编译期 | 符号校验 | `Module.symvers` + `KBUILD_EXTRA_SYMBOLS` |
| 运行期 | 符号解析 | `__ksymtab` 段 + `find_symbol` 二分查找 |
| 运行期 | 依赖建立 | `ref_module` + `struct module_use` |
| 卸载期 | 依赖保护 | `source_list` 非空 → `-EWOULDBLOCK` |

`EXPORT_SYMBOL_GPL` 与 `EXPORT_SYMBOL` 的区别：前者进 `__ksymtab_gpl` 段，只有 GPL 兼容的模块（`MODULE_LICENSE("GPL")`）才能引用——这是内核的"许可证强制"机制。

## 六、踩坑记录

1. `-Wmissing-prototypes`：导出的全局函数必须补原型声明，**不能用 `static`**（static 会让别的模块看不见）。正规做法是共享头文件。
2. `my_add undefined`：B 编译时缺 A 的 `Module.symvers`，用 `KBUILD_EXTRA_SYMBOLS` 指定。
3. `../my_add/Module.symvers: No such file or directory`：相对路径按 make cwd（`/tmp/kbuild`）解析，要写绝对路径或用 `MAKEFILE_LIST` 推导。
4. `insmod ... File exists`：-EEXIST，同名模块已加载，不是文件错误。
