# 31. platform 总线驱动：设备与驱动分离

> 阶段五第十一课。把前一课的字符设备驱动（cdev 注册）搬进 `probe()`，理解 Linux 的"总线-设备-驱动"分离模型：注册一个 `platform_device` + 一个 `platform_driver`，靠**名字匹配**自动触发 `probe/remove`，实现设备信息与驱动逻辑解耦。这是串口、GPIO、I2C 从设备等真实驱动的标准骨架。

## 一、为什么需要 platform 驱动

纯 cdev 驱动（06～10 课）有个根本问题：**设备信息和驱动逻辑写死在同一个 `init` 里**。

```c
static int __init hello_dev_init(void) {
    alloc_chrdev_region(&dev_num, 0, 2, "hello_dev");  // 设备信息
    cdev_init(...); cdev_add(...);                       // 驱动逻辑
    ...
}
```

真实硬件上，一片 SoC 上有几百个外设，每个外设的**基地址、中断号、DMA 通道**都不同。如果每个驱动都自己硬编码这些信息，内核里会充满重复代码，而且**同一块驱动代码无法复用给不同硬件**。

Linux 的解法：把"设备"和"驱动"拆成两个独立对象，交给一个"总线"来配对。

## 二、总线-设备-驱动三要素

```
         ┌───────────────────────────┐
         │          总线 bus          │
         │   /sys/bus/platform/      │
         │                           │
         │   match() 名字匹配          │
         │                           │
   ┌─────┴─────┐             ┌───────┴──────┐
   │   device   │  ←配对→     │    driver     │
   │  设备（硬件） │             │  驱动（代码）  │
   └───────────┘             └──────────────┘
```

- **device**：描述"有什么硬件"——名字、资源（地址/中断）
- **driver**：描述"怎么操作硬件"——`probe/remove` 函数
- **bus**：负责配对——拿 device 的名字去 driver 列表里找同名的

`platform` 是一种**虚拟总线**：它不对应真实的物理总线（如 PCI、USB、I2C），而是把那些"直接挂在 CPU 地址空间上、没有发现协议"的片上外设统一挂到 `/sys/bus/platform/` 下管理。

## 三、数据结构

```c
// include/linux/platform_device.h
struct platform_device {
    const char  *name;          // 设备名，用于匹配
    int         id;
    struct device dev;          // 内嵌通用 device
    u32         num_resources;
    struct resource *resource;  // 基地址、中断号等资源
    ...
};

struct platform_driver {
    int  (*probe)(struct platform_device *);
    void (*remove)(struct platform_device *);
    struct device_driver driver;    // 内嵌通用 driver，含 .name
    const struct platform_device_id *id_table;  // 可选的 ID 表
    ...
};
```

关键：`platform_driver.driver.name` 和 `platform_device.name` **必须字符串相等**才能配对。

## 四、名字匹配：platform_match

配对的核心是总线上的 `match` 回调，`platform` 总线的实现（drivers/base/platform.c）：

```c
static int platform_match(struct device *dev, struct device_driver *drv)
{
    struct platform_device *pdev = to_platform_device(dev);
    struct platform_driver *pdrv = to_platform_driver(drv);

    /* driver_override 强制绑定优先 */
    if (pdev->driver_override)
        return !strcmp(pdev->driver_override, drv->name);

    /* ① 设备树（OF）风格匹配 */
    if (of_driver_match_device(dev, drv))
        return 1;

    /* ② ACPI 风格匹配 */
    if (acpi_driver_match_device(dev, drv))
        return 1;

    /* ③ ID 表匹配 */
    if (pdrv->id_table)
        return platform_match_id(pdrv->id_table, pdev) != NULL;

    /* ④ 兜底：驱动名 == 设备名 */
    return (strcmp(pdev->name, drv->name) == 0);
}
```

四种匹配方式按优先级走，我们实验用的是**第 ④ 种兜底方式**：`"hello_plat" == "hello_plat"`。

## 五、注册与自动 probe 的调用链

```
platform_driver_register(&hello_driver)
  → __platform_driver_register(drv, THIS_MODULE)
    → driver_register(&drv->driver)          // 注册进 driver 列表
      → bus_add_driver(drv)
        → driver_attach(drv)
          → bus_for_each_dev(bus, NULL, drv, __driver_attach)  // 遍历总线上所有 device
            → __driver_attach(dev, data)
              → driver_match_device(drv, dev)
                → drv->bus->match(dev, drv)   // = platform_match，名字匹配成功
              → driver_probe_device(drv, dev)
                → really_probe()
                  → drv->probe(dev)            // ← 自动调用你的 hello_probe
```

要点：**probe 不是你在 init 里主动调的**，而是 `driver_register` 注册后，内核在 `driver_attach` 遍历设备时发现名字匹配，自动回调。

同理，卸载时 `platform_driver_unregister` 会走 `device_release_driver` → `__device_release_driver` → `drv->remove(dev)`，自动回调你的 `hello_remove`。

## 六、实验：hello_plat.c 结构

```c
// ① 设备信息从 init 搬进 probe：probe 里做 cdev 注册
static int hello_probe(struct platform_device *pdev) {
    alloc_chrdev_region(&dev_num, 0, 2, DEVICE_NAME);   // 申请设备号
    hello_class = class_create(DEVICE_NAME);
    for (i = 0; i < 2; ++i) {
        cdev_init(&hello_devs[i].cdev, &hello_fops);
        cdev_add(&hello_devs[i].cdev, dev_num + i, 1);
        device_create(hello_class, NULL, dev_num + i, NULL,
                      DEVICE_NAME"%d", i);
    }
    return 0;
}

// ② remove 里做 cdev 清理
static void hello_remove(struct platform_device *pdev) {
    for (i = 0; i < 2; ++i) {
        device_destroy(hello_class, dev_num + i);
        cdev_del(&hello_devs[i].cdev);
    }
    class_destroy(hello_class);
    unregister_chrdev_region(dev_num, 2);
}

// ③ 驱动对象：绑定 probe/remove + 驱动名
static struct platform_driver hello_driver = {
    .probe  = hello_probe,
    .remove = hello_remove,
    .driver = { .name = "hello_plat", .owner = THIS_MODULE },
};

// ④ init：先注册设备，再注册驱动（顺序可换，注册驱动时就会匹配）
static int __init hello_plat_init(void) {
    pdev = platform_device_register_simple("hello_plat", -1, NULL, 0);
    return platform_driver_register(&hello_driver);
}

static void __exit hello_plat_exit(void) {
    platform_driver_unregister(&hello_driver);
    platform_device_unregister(pdev);
}
```

注意 `platform_device_register_simple` 会动态分配一个 `platform_device`（名字 `"hello_plat"`、无资源），它的 `id = -1` 表示"不要编号后缀"。

## 七、验证输出

```
insmod hello_plat.ko
[173440.015105] hello_plat: probe called, name=hello_plat   ← 自动回调 probe
[173440.015135] hello_plat: major = 510 minor=0

$ ls -l /dev/hello_plat*
crw------- 1 root root 510, 0 /dev/hello_plat0
crw------- 1 root root 510, 1 /dev/hello_plat1

rmmod hello_plat
[173441.023821] hello_plat: remove called                  ← 自动回调 remove
```

`/sys/bus/platform/` 下也能看到两个对象：

```
/sys/bus/platform/devices/hello_plat/       ← 设备
/sys/bus/platform/drivers/hello_plat/       ← 驱动
```

## 八、与纯 cdev 驱动对比

| | 纯 cdev（06～10 课） | platform 驱动（本课） |
|---|---|---|
| 注册时机 | init 里硬编码 | probe 里（自动回调） |
| 设备信息 | 写死在 init | 独立成 platform_device |
| 驱动逻辑 | 写死在 init | 独立成 platform_driver |
| 硬件复用 | 换硬件要改代码 | 换硬件只换 device |
| 匹配机制 | 无（手动 insmod） | 总线名字匹配 |
| 适用场景 | 教学、纯软件设备 | 所有真实片上外设 |

## 九、真实内核对应

几乎每个片上外设驱动都是这个结构，例如：

- **串口** `drivers/tty/serial/amba-pl011.c`：`pl011_driver` 是 `platform_driver`，probe 里读 `resource` 拿到寄存器基地址和中断号，再注册 tty
- **GPIO** `drivers/gpio/gpio-pl061.c`：probe 里 `devm_platform_ioremap_resource` 映射寄存器
- **I2C 从设备**：platform 之上再叠一层 i2c 总线（platform 挂 I2C 控制器，I2C 总线再挂从设备）

真实驱动的 probe 和我们的唯一区别是：它会用 `pdev->resource` 里的**硬件资源**（基地址/中断号），而我们实验里 `pdev` 是 `register_simple` 造的、没有资源，所以 probe 只做了纯软件的 cdev 注册。下一步进阶方向就是给 `platform_device` 塞入真实 `resource`，在 probe 里 `ioremap` 映射寄存器操作硬件。
