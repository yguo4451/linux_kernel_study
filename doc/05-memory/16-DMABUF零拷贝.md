# DMA-BUF 零拷贝：设备间共享内存

> 内存管理子系列第 16 篇（汽车方向第二篇）。上一篇 CMA 解决了"DMA 要连续内存"的问题，本篇解决下一个问题：**多个设备（摄像头→ISP→GPU→显示）之间怎么共享这块内存，而不做 memcpy**。答案是 DMA-BUF 框架——零拷贝的核心。

## 一、问题：设备间传数据的两种方式

车载摄像头管线里，一帧图像要经过多个设备：

```
摄像头 sensor → ISP → AI 推理 → 显示/编码
```

传统做法（拷贝）：每个设备各自分配 buffer，数据一环一环 `memcpy`：

```
[摄像头 buffer] --memcpy--> [ISP buffer] --memcpy--> [AI buffer] --memcpy--> [显示 buffer]
```

4K@60fps 单帧 ~30MB，每环拷贝一次 = 每秒多拷 2GB 数据，浪费 CPU + 内存带宽 + 多占 4 份内存。

**DMA-BUF 做法（零拷贝）**：所有设备共享**同一块物理内存**，各自用 DMA 直接读写，全程不拷贝。

## 二、三个角色

| 角色 | 是谁 | 干什么 |
|------|------|--------|
| **exporter（导出者）** | 拥有 buffer 的驱动（如摄像头）| `dma_buf_export()` 把 buffer 导出成 dmabuf |
| **importer（导入者）** | 想用 buffer 的驱动（如 ISP/GPU）| `dma_buf_attach()` + `dma_buf_map_attachment()` 拿到物理地址 |
| **dmabuf fd** | 用户态的文件描述符 | 在进程/设备间传递"这块 buffer 的引用" |

## 三、核心机制：调用链

```
exporter: dma_buf_export(exp_name, ops, size, flags)
              ↓ 生成一个 struct dma_buf + 一个 fd

用户态: fd 在进程间传递（或通过 IPC）

importer: dma_buf_attach(dmabuf, dev)          // ① 挂接：声明"我要用"
          dma_buf_map_attachment(attach, dir)  // ② 映射：拿到 sg_table
              ↓ 返回 struct sg_table
              ↓ 里面是 scatterlist（物理地址列表）

importer: 把 sg_table 编程进自己的 DMA 控制器
          → DMA 直接读写同一块物理内存（零拷贝！）
```

**关键点**：`dma_buf_map_attachment` 返回的 `sg_table` 里是**物理地址**，importer 的 DMA 控制器直接按这些物理地址读写——这就是零拷贝的本质：**各设备的 DMA 都指向同一块物理内存**。

## 四、数据结构（`include/linux/dma-buf.h`）

```c
struct dma_buf {
    struct file *file;
    const struct dma_buf_ops *ops;   // exporter 提供的操作（map_dma_buf/mmap/...）
    size_t size;
    void *priv;                       // exporter 的私有数据
};

struct dma_buf_attachment {
    struct dma_buf *dmabuf;
    struct device *dev;
    struct list_head node;
};

struct sg_table {                     // 零拷贝的"钥匙"
    struct scatterlist *sgl;          // 物理地址列表
    unsigned int nents;
    unsigned int orig_nents;
};
```

## 五、与 CMA 的连接（汽车场景的核心链条）

```
摄像头驱动
  ├─ dma_alloc_contiguous() ──► 从 CMA 池拿 32MB 连续物理内存（doc 15）
  ├─ 把图像 DMA 到这块内存
  └─ dma_buf_export() ──► 导出成 dmabuf

ISP / GPU / AI 驱动
  ├─ dma_buf_attach + map ──► 拿到同一块 CMA 内存的物理地址
  └─ 自己的 DMA 直接读写 ──► 零拷贝处理
```

**CMA 提供"连续物理内存"，DMA-BUF 提供"跨设备共享它的协议"**。两者配合，就是车载摄像头管线的内存基石。

## 六、汽车场景全景

```
摄像头 → ISP → AI 推理 → 显示/编码 → 存储
   └────── 全部共享同一块 CMA 内存（DMA-BUF）──────┘
```

工业界谁在用：

- **Android gralloc**：相机预览、GPU 纹理共享全靠 DMA-BUF
- **GStreamer / Wayland**：视频管线零拷贝
- **vsomeip / DDS**：大帧数据传输避免 memcpy
- **车载 AI**：摄像头帧 → NPU 推理 → 显示，三环共享

## 七、源码位置

| 模块 | 路径 | 作用 |
|------|------|------|
| 核心框架 | `drivers/dma-buf/dma-buf.c` | export/attach/map/unmap 全套 |
| 用户态分配器 | `drivers/dma-buf/heaps/` | dma-heap，用户态直接分配 dmabuf |
| 数据结构 | `include/linux/dma-buf.h` | `dma_buf`/`attachment`/`ops` |
| 用户态接口 | `include/uapi/linux/dma-buf.h` | `DMA_BUF_IOCTL_SYNC` 等 ioctl |

## 八、用户态怎么用（概念）

1. exporter 生成 dmabuf，得到一个 fd
2. fd 通过 Unix socket / `fork` 传给另一个进程
3. 另一个进程 `mmap(fd)` 把这块内存映射进自己的地址空间，直接读写
4. 需要时 `DMA_BUF_IOCTL_SYNC` 处理缓存一致性（CPU 与 DMA 之间的 cache 同步）

## 九、观测与实验限制

- debugfs 可看已导出的 dmabuf：`/sys/kernel/debug/dma_buf/bufinfo`（需 `CONFIG_DMABUF_DEBUG`）
- 本系统 `CONFIG_DMABUF_HEAPS` 未开，用户态 dma-heap 实验暂不可做——留待下次重编内核时补 `CONFIG_DMABUF_HEAPS=y`（与 PSI 一起）

## 十、陷阱

1. **缓存一致性是最大坑**：CPU 和 DMA 同时访问同一块内存，必须正确 `DMA_BUF_IOCTL_SYNC`，否则读到旧数据（cache 和内存不一致）。
2. **生命周期**：buffer 被多个 importer 引用，exporter 不能提前释放——dmabuf 用引用计数管理。
3. **不是所有内存都能导出**：普通 `kmalloc` 内存物理不连续，DMA 用不了；必须用 CMA 或 dma-coherent 分配。
