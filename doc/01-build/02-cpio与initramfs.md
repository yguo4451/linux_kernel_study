# cpio 与 initramfs

## 一、cpio 是什么

**cpio** = "copy in and out"，一个打包/归档工具，和 `tar` 定位相同，但工作方式相反：

| | tar | cpio |
|---|---|---|
| 找文件 | 自己递归遍历目录 | 从 **stdin 读文件路径列表** |
| 常用搭配 | `tar -czf x.tar.gz dir` | `find . \| cpio -o -H newc \| gzip` |

关键参数：

- `cpio -o`：输出模式（打包）
- `-H newc`：使用 **newc 格式**（SVR4 便携格式）
- `cpio -i`：输入模式（解包）

## 二、为什么 initramfs 用 cpio 而不是 tar

Linux 内核**内建**了一个极简的 `newc` 解包器（源码在 `init/initramfs.c`）。内核在挂载任何磁盘、加载任何驱动**之前**，就能把这个归档解压到内存里的 `rootfs`。

- newc 格式足够简单，适合放进内核（几行代码就能解析）
- tar 格式更复杂，不适合塞进内核
- 所以打包命令是 `find . | cpio -o -H newc | gzip > initramfs.cpio.gz`

## 三、这条命令在做什么

```bash
cd rootfs
find . | cpio -o -H newc | gzip > ../initramfs.cpio.gz
```

1. `find .`：列出根文件系统所有文件路径
2. `cpio -o -H newc`：按 newc 格式打包成归档
3. `gzip`：压缩
4. `>`：输出到 `initramfs.cpio.gz`

## 四、和 tar 的对比记忆点

- tar：自己遍历，`tar -cf` 直接给目录
- cpio：被动接收路径列表，天然和 `find` 管道配合
- 内核启动场景用 cpio(newc)，日常归档用 tar 更常见

## 五、延伸：initramfs vs 磁盘镜像

最小系统可以有两种根文件系统载体：

| | initramfs | 磁盘镜像(ext4) |
|---|---|---|
| 载体 | 内存盘，cpio 包 | 磁盘分区 |
| 解包者 | 内核内建 newc 解包器 | 需要 ext4 驱动 + 磁盘控制器 |
| 启动路径 | 最简 | 更接近真实系统 |
| 适用 | 学习、最小系统、initramfs 引导 | 真实部署 |

本项目的 `initramfs.cpio.gz` 就是走的最简路径。
