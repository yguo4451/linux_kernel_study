---
name: build-linux
description: 'Build and boot a minimal Linux system from scratch: compile an arm64 kernel inside Docker, build a static BusyBox root filesystem, package an initramfs, and boot it in QEMU. Use when the user wants to build Linux from 0 to 1, create a bootable minimal Linux, or practice kernel experiments in QEMU/Docker.'
argument-hint: '目标架构或内核版本，例如：arm64 + 内核 6.6 LTS'
user-invocable: true
---

# Build Linux from Scratch

从源码构建一个可启动的最小 Linux 系统（arm64），并把"编译内核 + 构建根文件系统 + QEMU 启动"固化为可复用流程。

## 为什么用 Docker

Apple Silicon 的宿主机 `gcc` 实为 Apple clang，无法编译 Linux 内核；且宿主机为 arm64，目标也是 arm64。因此在 Linux 容器内用 GNU 工具链原生编译 arm64 内核，再交给宿主机 QEMU 启动。

## 何时使用

- 用户想从 0 手动搭建最小 Linux 系统
- 需要快速创建一个可启动内核 + 根文件系统的实验环境
- 验证内核配置、模块或某个子系统行为

## 前置依赖

- **Docker Desktop**：提供 Linux 构建环境（`docker info` 需正常）
- **QEMU**：`qemu-system-aarch64`（macOS 可 `brew install qemu`）

## 流程

1. 运行 [build.sh](../../../scripts/build.sh) —— Docker 内编译内核 + BusyBox，打包 initramfs
2. 运行 [run_qemu.sh](../../../scripts/run_qemu.sh) —— 在 QEMU 中启动该系统
3. 遇到问题查阅 [build-steps.md](./references/build-steps.md) 的排错章节

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `KERNEL_VERSION` | `6.6.44` | 内核版本 |
| `BUSYBOX_VERSION` | `1.36.1` | BusyBox 版本 |
| `MEM` | `512M` | QEMU 内存大小 |
| `CPU` | `cortex-a53` | QEMU 模拟的 CPU |

## 产物

- `build/kernel/Image`：内核镜像（arm64）
- `build/kernel/.config`：内核配置
- `build/rootfs/`：根文件系统内容
- `initramfs.cpio.gz`：打包好的 initramfs
