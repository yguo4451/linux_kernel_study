# 从 0 构建最小 Linux 系统（arm64）—— 详细步骤与排错

本文件解释 `scripts/build.sh`（入口）与 `scripts/build_in_container.sh`（容器内构建逻辑）做了什么，以及如何手动完成每一步。

## 整体流程

```
宿主机(Apple Silicon)              Docker 容器(Ubuntu 22.04, arm64)
        │                                 │
        └──── docker build 镜像 ──────────►│
        └──── docker run 挂载 /work ──────►│
                                          │ 下载内核源码 → ARCH=arm64 编译 → Image
                                          │ 下载 BusyBox → 静态编译 → install 到 rootfs
                                          │ 编写 /init → find|cpio|gzip → initramfs.cpio.gz
                                          │ 产物写回 /work/build、/work/initramfs.cpio.gz
        ◄─────────────────────────────────┘
宿主机 qemu-system-aarch64 -machine virt -kernel Image -initrd initramfs.cpio.gz
```

## 构建流程六步（容器内总览）

1. 下载源码（内核走阿里云镜像 `mirrors.aliyun.com/linux-kernel/`，回退 kernel.org）
2. 解压到容器本地 `/tmp` 再 `cp -a` 回 `src/`（规避挂载盘 tar 竞态）
3. `make O=/tmp/kbuild ARCH=arm64 defconfig` + `make Image`
4. BusyBox 静态编译（`CONFIG_STATIC=y`）
5. 构建 rootfs + 写 `/init`（挂载 proc/sysfs/devtmpfs/tmpfs + `setsid cttyhack sh`）
6. `find | cpio -H newc | gzip` → `initramfs.cpio.gz`

脚本位置：`scripts/build.sh` / `scripts/build_in_container.sh` / `scripts/run_qemu.sh` / `scripts/Dockerfile`

## 关键设计取舍

- **Docker 编译**：绕开 Apple clang，原生 arm64 不模拟、速度快
- **`O=` 分离 + `/tmp`**：源码树干净，编译写容器本地盘比挂载盘快
- **静态 BusyBox**：根文件系统零动态库依赖
- **initramfs + cpio(newc)**：内核内建解包器，无需磁盘/驱动，最简启动路径
- **init 挂载虚拟文件系统**：给用户态一个能干活的环境

## 手动步骤拆解（在容器内）

### 1. 编译内核

```bash
cd linux-6.6.44
make O=/tmp/kbuild ARCH=arm64 defconfig   # 默认配置
make O=/tmp/kbuild ARCH=arm64 -j$(nproc) Image
# 产物：/tmp/kbuild/arch/arm64/boot/Image
```

- `vmlinux`：未压缩内核 ELF，供调试/反汇编
- `Image`：可直接加载的内核镜像，QEMU 用 `-kernel` 直接启动
- 用 `O=` 指定输出目录，避免污染源码树

### 2. 构建 BusyBox 根文件系统

```bash
cd busybox-1.36.1
make defconfig
# 打开 Settings -> Build static binary (no shared libs)  → CONFIG_STATIC=y
make -j$(nproc)
make CONFIG_PREFIX=/work/build/rootfs install
```

静态链接的 BusyBox 不依赖任何动态库，是最小根文件系统的首选。

### 3. 编写 /init

内核启动到最后会执行根文件系统里的 `/init`：

```sh
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
setsid cttyhack sh
```

`/proc` `/sys` `/dev` 是虚拟文件系统，必须挂载后用户态工具才能正常工作。

### 4. 打包 initramfs

```bash
cd rootfs
find . | cpio -o -H newc | gzip > ../initramfs.cpio.gz
```

### 5. QEMU 启动（宿主机）

```bash
qemu-system-aarch64 \
  -machine virt -cpu cortex-a53 \
  -kernel build/kernel/Image \
  -initrd initramfs.cpio.gz \
  -append "console=ttyAMA0" \
  -nographic -m 512M
```

arm64 的 `-machine virt` 串口是 `ttyAMA0`；x86 的 `-kernel` 用 bzImage，arm64 用 `Image`。

## 常见排错

| 现象 | 原因 | 解决 |
|------|------|------|
| `Kernel panic - VFS: Unable to mount root fs` | initramfs 缺失/损坏 | 确认 `-initrd` 路径正确、cpio 打包成功 |
| 启动后无 shell 提示符 | init 未给 tty 控制 | 确认 `setsid cttyhack sh`、`console=ttyAMA0` |
| `sh: can't access tty` | devtmpfs 未挂载 | init 中加 `mount -t devtmpfs devtmpfs /dev` |
| BusyBox 报 `not found` | 动态链接但缺库 | 静态编译 BusyBox（CONFIG_STATIC=y） |
| 内核编译报缺依赖 | 容器缺 bison/flex/bc | Dockerfile 已包含，勿手动删减 |
| `wget` 内核超时 | 网络问题 | 改用阿里云镜像（脚本已内置回退） |
| tar 解压报 `Directory renamed...` | 解压到 Docker 挂载盘竞态 | 先在容器 `/tmp` 解压，再 `cp` 回工作区 |

## 退出 QEMU

`Ctrl+A` 松开后按 `X`。

## 下一步实验

- `make menuconfig` 裁剪内核，观察哪些选项影响启动
- 写一个内核模块（.ko），用 `insmod` 加载
- 用 `strace`、`/proc`、`ftrace` 观察文件系统/调度/内存行为
- 用 `-s -S` 参数 + gdb 调试内核启动过程
