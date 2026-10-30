# 25. 模块参数 module_param

> 阶段四第五课。通过 `module_param` 实验，理解模块如何接受加载参数与运行时配置：宏展开、`struct kernel_param`、`__param` 段、`parse_args` 加载期解析、sysfs 参数接口。

## 一、三个时机，三次赋值

```c
static int base = 0;                 // ① 编译期：初始值
module_param(base, int, 0644);
```

```
① 编译期   static int base = 0;        → 初始值 0
② 加载期   insmod my_add.ko base=42    → parse_args → ops->set("42") → 42
③ 运行期   echo 99 > .../base          → sysfs store → ops->set("99") → 99
```

实验三步：

```bash
insmod my_add.ko
cat /sys/module/my_add/parameters/base     # 0（默认）

rmmod my_add
insmod my_add.ko base=42
cat /sys/module/my_add/parameters/base     # 42（加载期传参）

echo 99 > /sys/module/my_add/parameters/base
cat /sys/module/my_add/parameters/base     # 99（运行期写）
```

## 二、宏展开：生成 struct kernel_param 放进 __param 段

`module_param` 是宏链（`include/linux/moduleparam.h`）：

```
module_param(name, type, perm)
  → module_param_named → module_param_cb → __module_param_call
```

`__module_param_call`（moduleparam.h:286）最终生成：

```c
static const char __param_str_##name[] = prefix #name;
static struct kernel_param __param_##name
	__used __section("__param")     // ← 关键：放进 __param 段
	= { __param_str_##name, THIS_MODULE, ops, perm, level, flags, { arg } };
```

**闭环**：doc 03 里 `find_module_sections()` 读的 `__param` 段，正是这些宏生成的：

```c
mod->kp = section_objs(info, "__param", sizeof(*mod->kp), &mod->num_kp);
```

## 三、struct kernel_param

```c
struct kernel_param {
	const char *name;              // "base"
	struct module *mod;            // THIS_MODULE
	const struct kernel_param_ops *ops;  // int 类型的 set/get 表
	const u16 perm;                // 0644 —— sysfs 权限
	s8 level;
	u8 flags;                      // UNSAFE 等
	union { void *arg; ... };      // 指向真实变量 &base
};
```

`perm` 语义：`0` = 不暴露 sysfs；`0444` = 只读；`0644` = root 可写。

## 四、param_ops_int：set/get 实现

`type` 参数（int/bool/charp/…）就是选 `param_ops_##type` 表。由宏 `STANDARD_PARAM_DEF` 生成（kernel/params.c:215）：

```c
// STANDARD_PARAM_DEF(int, int, "%i", kstrtoint) 展开为：
int param_set_int(const char *val, const struct kernel_param *kp)
{
	return kstrtoint(val, 0, (int *)kp->arg);      // "42" → 42 写进 &base
}
int param_get_int(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%i\n", *((int *)kp->arg)); // 42 → "42\n"
}
```

- set：字符串 → int（`kstrtoint`）
- get：int → 字符串（`scnprintf`）

## 五、加载期：parse_args

`load_module()` 里（doc 02 十五步之一）：

```c
parse_args(mod->name, mod->args, mod->kp, mod->num_kp, ...)
```

- `mod->args` = `"base=42"`
- `mod->kp` = `__param` 段数组

`parse_args` → `parse_one` 把 `"base=42"` 按 `=` 拆开，找到 name 匹配的 kp，调 `ops->set("42", kp)`。

## 六、运行期：sysfs 转发

`kernel/params.c`：

```c
static ssize_t param_attr_show(...) {
	count = attribute->param->ops->get(buf, attribute->param);  // cat → get
}
static ssize_t param_attr_store(...) {
	err = attribute->param->ops->set(buf, attribute->param);    // echo → set
}
```

sysfs 层只是转发，真正干活的是 `param_ops_int`。`param_attr_store` 里有 `param_check_unsafe()`：`module_param_unsafe` 声明的参数被写时会打 taint。

## 七、踩坑

1. 变量建议 `static`，避免全局符号污染与 `-Wmissing-prototypes` 提醒。
2. `charp`（字符串指针）可写时，旧内存会被 `kfree`，并发访问需 `kernel_param_lock()` 保护。
3. 不想要运行时可写就设 `0444` 或 `0`。
