# 22. insmod 调用链：从用户态到 do_init_module

> 阶段四第二课。把 `insmod hello.ko` 从 BusyBox 用户态一路追到内核 `do_init_module()`，理解模块加载的完整数据流，并定位之前学过的 taint、vermagic、重定位分别发生在哪一步。

## 一、完整调用链

```
用户态 BusyBox                              内核 kernel/module/main.c
─────────────────────                      ────────────────────────────
insmod_main (modutils/insmod.c)
  └─ bb_init_module(filename, opts)
     ├─ finit_module(fd, opts, 0)  ──────▶ SYSCALL_DEFINE3(finit_module)
     │                                     ├─ may_init_module()      权限检查
     │                                     └─ idempotent_init_module()  幂等去重
     │                                          └─ init_module_from_file()
     │                                               └─ kernel_read_file() 读入 .ko
     └─ init_module(image, len, opts) ──▶ SYSCALL_DEFINE3(init_module)
                                           └─ copy_module_from_user()
                                               └─ load_module()
                                                    ├─ module_sig_check()         签名
                                                    ├─ elf_validity_cache_copy()  解析 ELF
                                                    ├─ early_mod_check()→check_modinfo()  vermagic
                                                    ├─ layout_and_allocate()      分配内存/定址
                                                    ├─ add_unformed_module()      挂链表
                                                    ├─ module_augment_kernel_taints()  打 taint
                                                    ├─ simplify_symbols()         符号定值
                                                    ├─ apply_relocations()        重定位落地
                                                    └─ do_init_module()
                                                         └─ do_one_initcall(mod->init)
```

## 二、用户态：finit_module vs init_module

`bb_init_module`（`busybox/modutils/modutils.c:194`）先试 `finit_module`，失败再回退 `init_module`：

```c
// modutils.c:220 —— 先试 finit_module，只把 fd 交给内核
int fd = open(filename, O_RDONLY | O_CLOEXEC);
if (fd >= 0) {
    rc = finit_module(fd, options, 0) != 0;   // 成功就返回
    close(fd);
    if (rc == 0) return rc;
}
// modutils.c:241 —— 回退：用户态读好 .ko，整个镜像传内核
init_module(image, image_size, options);
```

| | `init_module` | `finit_module` |
|---|---|---|
| 参数 | 内存镜像 + 长度 | 文件描述符 fd |
| 谁读文件 | 用户态读好 | 内核自己读 |
| 适合场景 | 模块在内存/压缩包 | 模块在安全存储（只读 rootfs） |
| 压缩模块 | 用户态解压后传 | 内核用 `MODULE_INIT_COMPRESSED_FILE` 解压 |

两条命令底层都是 `syscall(__NR_xxx, ...)` 直接系统调用（modutils.c:12-16）。

## 三、内核入口：幂等去重

`SYSCALL_DEFINE3(finit_module)`（main.c:3232）→ `idempotent_init_module()`（main.c:3213）：

- 以 `file_inode(f)` 为 key 的哈希表做去重
- 第一个到达的进程是 "winner"，真正执行 `init_module_from_file()`
- 其它进程发现同 cookie 已在表中，`wait_for_completion()` 睡眠等 winner 完成后拿同一返回值

避免同一模块被并发加载两次、白分配两份内存。

## 四、load_module 十五步（main.c:2864）

| 步骤 | 函数 | 作用 | 对应现象 |
|------|------|------|----------|
| ① | `module_sig_check()` | 模块签名校验 | — |
| ② | `elf_validity_cache_copy()` | 解析 ELF 头/段，校验 `.gnu.linkonce.this_module` 大小 | `readelf -S` |
| ③ | `early_mod_check()`→`check_modinfo()` | vermagic 校验（main.c:2103） | 版本不匹配报错 |
| ④ | `layout_and_allocate()` | 计算段布局 + 分配内存 | `.text` 落到最终地址 |
| ⑤ | `add_unformed_module()` | 模块以"未成形"状态挂链表 | — |
| ⑥ | `module_augment_kernel_taints()` | 打 taint（main.c:2043 打印、2045 置位） | `tainted=4096` |
| ⑦ | `simplify_symbols()` | 符号表 st_value 改成真实地址 | `nm` 里 `U printk` 被填 |
| ⑧ | `apply_relocations()` | 重定位落地（main.c:1472） | `.rela.text` 被改写 |
| ⑨ | `complete_formation()` | state: UNFORMED→COMING | `lsmod` 的 `Live` 前身 |
| ⑩ | `mod_sysfs_setup()` | 建 `/sys/module/<name>/` | sysfs 目录 |

> 关键观察：taint（⑥）在 `module->init()` 之前就打上了。即使 init 函数失败、加载最终失败，taint 已经留下——这就是 taint "粘性"的根源之一。

## 五、do_init_module（main.c:2523）

```c
do_mod_ctors(mod);                       // 先跑构造函数
if (mod->init != NULL)
    ret = do_one_initcall(mod->init);    // main.c:2552 ← 调 hello_init()
...
mod->state = MODULE_STATE_LIVE;          // 转正
...
/* 把 .init.text/.init.data 挂进 init_free_list，工作队列稍后释放 */
```

`do_one_initcall(mod->init)` 里那个 `mod->init`，正是 `module_init(hello_init)` 宏注册进 `struct module` 的函数指针。`dmesg` 里的 `hello init...` 就在这里打出。init 成功后，`__init` 段被释放（对应 readelf 里 `.init.text`/`.init.data` 段）。

## 六、源码坐标速查

| 现象/函数 | 位置 |
|-----------|------|
| insmod 入口 | `busybox/modutils/insmod.c:70` |
| bb_init_module | `busybox/modutils/modutils.c:194` |
| finit_module 系统调用 | `kernel/module/main.c:3232` |
| init_module 系统调用 | `kernel/module/main.c:3078` |
| 幂等去重 | `kernel/module/main.c:3213` |
| load_module | `kernel/module/main.c:2864` |
| taint 打印/置位 | `kernel/module/main.c:2043 / 2045` |
| vermagic 校验 | `kernel/module/main.c:2103` |
| do_init_module | `kernel/module/main.c:2523` |
| 调 module->init | `kernel/module/main.c:2552` |
