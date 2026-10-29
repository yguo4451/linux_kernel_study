# Linux Kernel Study

从 0 手把手搭建一个可运行的 ARM64 Linux 系统，并沿路深入内核四大核心子系统（文件系统、网络、进程调度、内存管理），最终延伸到汽车实时 Linux（PREEMPT_RT / 驱动 / 中间件）方向。

学习以**构建为主线、动手 + 源码并重**：先编译出自己的内核 + 根文件系统，再用 QEMU 跑起来，遇到哪个子系统就深挖哪个。

---

## 这是什么

- 一套可复现的「最小 Linux 系统」构建环境：**Docker 内编译 + 宿主机 QEMU 运行**
- 一份沉淀式学习笔记：每个知识点落到 `doc/` 的 Markdown（含真实命令、源码路径、实验现象）
- 一组可动手的实验程序：调度、内存、内核模块等（`experiments/`）

---

## 环境要求

| 项 | 说明 |
|----|------|
| 宿主机 | macOS（Apple Silicon / arm64） |
| Docker | Docker Desktop（编译在容器内完成，绕开 Apple clang） |
| QEMU | `brew install qemu`，提供 `qemu-system-aarch64` |
| Git | 仓库管理（可选） |

> 为什么用 Docker：macOS 自带的 `gcc` 是 Apple clang，无法编译 Linux 内核；Docker 内 Ubuntu 22.04 使用 GNU gcc，且 arm64 原生运行不模拟。

---

## 快速开始

```bash
# 1. 构建：Docker 内编译内核 + BusyBox + rootfs + initramfs
./scripts/build.sh

# 2. 启动：宿主机 QEMU 运行（Ctrl+A 再按 X 退出）
./scripts/run_qemu.sh
```

### 进入系统

QEMU 启动后，可通过两种方式进入这个最小 Linux：

```bash
# 方式一：SSH（root 空密码，实验环境免密）
ssh -p 2222 root@127.0.0.1

# 方式二：telnet
telnet 127.0.0.1 2323
```

> 退出 QEMU：先按 `Ctrl+A`，再按 `X`。

### 常用构建参数（环境变量）

```bash
# 指定内核 / BusyBox 版本（默认 6.12.112 / 1.36.1）
KERNEL_VERSION=6.12.112 BUSYBOX_VERSION=1.36.1 ./scripts/build.sh

# 调整 QEMU 内存 / 核数 / 端口
MEM=1G SMP=4 SSH_PORT=2222 ./scripts/run_qemu.sh
```

---

## 目录结构

```
Linux_kernel_study/
├── doc/            # 学习笔记（按子系统分目录，见下方导航）
├── experiments/    # 动手实验源码（C）
├── scripts/        # 构建 / 运行 / 工具脚本
├── src/            # 下载的内核与工具链源码（git 忽略，不入库）
├── build/          # 构建产物（git 忽略，不入库）
│   ├── kernel/Image        # 编译出的内核镜像
│   ├── rootfs/             # 根文件系统
│   └── disk.img            # 数据磁盘（可 mount 到 /mnt 做实验）
└── initramfs.cpio.gz       # initramfs 包（git 忽略）
```

---

## 脚本说明

| 脚本 | 作用 | 运行环境 |
|------|------|----------|
| `scripts/build.sh` | 构建入口：build Docker 镜像 + 容器内编译 | 宿主机 |
| `scripts/build_in_container.sh` | 容器内实际构建逻辑（六步） | 容器内 |
| `scripts/run_qemu.sh` | QEMU 启动最小系统 | 宿主机 |
| `scripts/enter_container.sh` | 进入常驻容器 `linux-builder` | 宿主机 |
| `scripts/guest_exec.py` | 从宿主机执行 guest 内命令（走 telnet 通道） | 宿主机 |
| `scripts/netstat_stat.sh` | 模拟 `netstat -s`（解析 `/proc/net/snmp`） | guest 内 |
| `scripts/tcp_fin_server.py` | 制造标准四次挥手的 TCP server | 宿主机 |
| `scripts/telnet_verify.py` | 验证 telnet 通道可达 | 宿主机 |

---

## 学习文档导航（doc/）

| 目录 | 主题 | 篇数 |
|------|------|------|
| `01-build/` | 构建与环境（构建流程、initramfs、动态链接、SSH 集成） | 5 |
| `02-network/` | 网络协议栈（NAT → TCP → 拥塞控制 → 抓包实证） | 13 |
| `03-filesystem/` | 文件系统（VFS、ext4、procfs、sysfs、pstore） | 11 |
| `04-scheduling/` | 进程调度（CFS → EEVDF → RT → PREEMPT_RT） | 15 |
| `05-memory/` | 内存管理（Buddy → 页表 → 回收 → CMA/DMA-BUF/zram） | 21 |
| `06-module/` | 内核模块与驱动 | 1 |

完整索引与学习路线见 [`doc/README.md`](doc/README.md)。

---

## 实验代码（experiments/）

| 目录 | 实验 |
|------|------|
| `pi_demo/` | 优先级反转实测（普通 mutex vs PI mutex） |
| `dl_demo/` | SCHED_DEADLINE 调度器 |
| `cpu_isol/` | CPU 隔离与实时性（latency_test） |
| `memory/` | mlock 锁页、巨页 mmap、mem_hog 内存压力 |
| `module/hello/` | 第一个内核模块（Makefile + hello.c） |

> 内核模块阶段的实验代码由学习者自己编写，AI 只讲解原理与提示，不直接改代码。

---

## 构建流程概览

`build.sh` 在容器内完成六步：

1. 下载源码（内核走阿里云镜像，回退 kernel.org）
2. 解压到容器 `/tmp`，再 `cp -a` 回 `src/`（规避挂载盘 tar 竞态）
3. `make O=/tmp/kbuild ARCH=arm64 defconfig` + `make Image`
4. BusyBox 静态/动态编译
5. 构建 rootfs + 写 `/init`（挂载虚拟文件系统 + 启动 sshd/telnetd）
6. `find | cpio -H newc | gzip` → `initramfs.cpio.gz`

技术要点与踩坑记录详见 `doc/01-build/` 与 `scripts/build_in_container.sh` 注释。

---

## Git 说明

- 已纳入版本管理的内容：`doc/`、`experiments/`、`scripts/`、`.github/`、`README.md`
- 已忽略的内容：`src/`（3.5G 源码）、`build/`（构建产物）、`initramfs.cpio.gz`、内核模块编译产物（`*.o` `*.ko` 等），见 `.gitignore`

日常提交：

```bash
git add -A && git commit -m "说明" && git push
```
