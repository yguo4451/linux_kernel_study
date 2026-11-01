# 33. 资源声明与 devm 托管：request_mem_region 与 devm_ioremap_resource

> 阶段五第十三课。把手动 `ioremap` 升级为托管版 `devm_platform_ioremap_resource`，理解两个核心机制：① `request_mem_region` 的**资源占用声明**（`IORESOURCE_BUSY` 标志）；② devm 托管 API 的**自动清理**。实验中发现"嵌套"而非"冲突"，揭示了 BUSY 标志的本质。

## 一、上一课的隐患

上一课直接 `ioremap` 了 UART 地址，但 `/proc/iomem` 显示：

```
09000000-09000fff : uart0     ← 早已被声明
```

直接 `ioremap` 等于"没打招呼就进门"——不检查资源是否被占用。真实驱动里两个驱动同时操作同一块硬件会乱套，所以需要**资源声明**机制。

## 二、request_mem_region：资源占用声明

```c
request_mem_region(start, size, name);   // 声明"这块物理地址归我用"
```

- 成功 → 在 `/proc/iomem` 里登记一行（`uart0` 就是声明后留下的）
- 冲突 → 返回 NULL
- 用完 `release_mem_region` 归还

## 三、BUSY 标志：记录 vs 占用

`/proc/iomem` 里的 region 分两类：

| API | BUSY 标志 | 含义 |
|---|---|---|
| `insert_resource`（设备树框架记录 reg） | 无 | 只是"登记在册"，不阻止别人 |
| `request_mem_region`（驱动声明占用） | **有** | 真正"占用"，重叠就 -EBUSY |

冲突检测源码（`kernel/resource.c:1279`）：

```c
if (conflict != parent) {
    if (!(conflict->flags & IORESOURCE_BUSY)) {   // 遇到不含 BUSY 的
        parent = conflict;                         // → 不算冲突，嵌套进去
        continue;
    }
}
return -EBUSY;                                     // 只有含 BUSY 才真正冲突
```

**关键**：冲突检测看的是 **BUSY 标志**，不是地址重叠本身。不含 BUSY 的 region 只是记录，可以嵌套。

## 四、实验现象：嵌套而非冲突

实验预期 `devm_request_mem_region` 会因 `uart0` 被占用而失败，实际却成功了。看 `/proc/iomem`：

```
09000000-09000fff : uart0                        ← 外层：不含 BUSY（console 框架的记录）
  09000000-09000fff : hello_iomem uart0          ← 我们的，嵌套进去

09010000-09010fff : pl031@9010000                ← 外层：设备树 insert（不含 BUSY）
  09010000-09010fff : rtc-pl031                  ← 内层：rtc 驱动 request（含 BUSY）
```

`uart0` 这层不含 BUSY，所以我们的 request 把它当父节点**嵌套**进去，没触发 -EBUSY。

**真正的冲突**发生在两个驱动都 `request_mem_region` 同一块（都含 BUSY）时，第二个返回 -EBUSY。

## 五、devm 托管 API：登记 + 映射 + 自动清理三合一

`devm_platform_ioremap_resource`（`drivers/base/platform.c`）→ `devm_ioremap_resource`（`lib/devres.c`）：

```c
__devm_ioremap_resource(...)
{
    if (!res || resource_type(res) != IORESOURCE_MEM) return ERR_PTR(-EINVAL);

    // ① 声明资源占用（冲突返回 -EBUSY）
    if (!devm_request_mem_region(dev, res->start, size, name))
        return ERR_PTR(-EBUSY);

    // ② ioremap
    dest_ptr = __devm_ioremap(dev, res->start, size, type);
    if (!dest_ptr) { devm_release_mem_region(...); return ERR_PTR(-ENOMEM); }

    return dest_ptr;
}
```

**devm（device managed）核心思想**：把资源生命周期绑定到 `struct device`。probe 失败、remove 时，内核**自动**释放所有 devm 资源，无需手动 `iounmap`/`release_mem_region`。

## 六、实验改动

```c
// 手动版（上一课）
r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
io_base = ioremap(r->start, resource_size(r));
...
iounmap(io_base);   // remove 里手动清理

// 托管版（本课）
io_base = devm_platform_ioremap_resource(pdev, 0);   // 取资源+声明+映射
if (IS_ERR(io_base))
    return PTR_ERR(io_base);
// remove 里不需要 iounmap —— 自动释放
```

验证输出：

```
hello_iomem: UART FR=0x90 (TXFE=1 RXFE=1)
```

## 七、要点回顾

```
直接 ioremap      ──不检查占用──>  抢资源（危险但静默）
request_mem_region ──带 BUSY──>  真正占用，重叠返回 -EBUSY
insert_resource   ──不带 BUSY──>  只是记录，允许嵌套
devm 托管 API     ──绑定 device──>  自动声明+映射+清理，现代驱动标准写法
```
