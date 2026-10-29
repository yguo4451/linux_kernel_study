# 16. 静态编译 tcpdump 与静态/动态链接取舍

> 工具链补充篇。BusyBox 1.36 移除了 tcpdump，这里源码编译补上；顺带讲清静态 vs 动态链接的取舍。

## 背景：BusyBox 1.36 没有 tcpdump

- BusyBox 1.36（2023 年）**移除了 tcpdump applet**：`networking/` 目录无 `tcpdump.c`，全源码 grep `tcpdump` 为空，`.config` 无 `CONFIG_TCPDUMP`。
- 移除原因：精简版功能太弱、长期无人维护、安全漏洞多。
- 要抓包两条路：① QEMU `filter-dump`（宿主机抓 pcap）② 源码编译完整 tcpdump（guest 内抓）。

## 源码编译的三个前置条件

1. **依赖 libpcap**：tcpdump 链接 libpcap（抓包库），后者也要源码编译。
2. **架构必须是 Linux arm64**：macOS 宿主机直接编出来是 Mach-O，Linux guest 跑不了；须进 Docker 容器（Ubuntu 22.04 arm64）编出 Linux arm64 ELF。
3. **必须静态链接**：rootfs 是静态 BusyBox，没有 glibc 动态库（连 `/lib/ld-linux-aarch64.so.1` 都没有），动态版会 `not found`。

## 集成方式（`build_in_container.sh` 第 4.5 步）

关键步骤：

```bash
# libpcap：静态库 + pcap-config
( cd "$LIBPCAP_DIR" && \
  ./configure --disable-shared --enable-static && \
  make -j"$JOBS" )

# tcpdump：静态链接 libpcap（PATH 注入让 configure 找到 pcap-config）
( cd "$TCPDUMP_DIR" && \
  PATH="$LIBPCAP_DIR:$PATH" ./configure --disable-shared --enable-static \
    --without-crypto LDFLAGS="-static" && \
  make -j"$JOBS" )
```

设计细节：
- 源码 tarball 缓存到 `src/`，复用内核/BusyBox 的"先 `/tmp` 解压再 `cp` 回工作区"模式，规避挂载盘 tar 竞态。
- `--without-crypto` 禁用 libcrypto，避免静态链接时拖入 zlib/dl 的依赖链（抓包核心功能不受影响）。
- 产物 `tcpdump` 作为缓存标志，已存在则跳过重编。
- `Dockerfile` 里 `flex bison` 已装（原本给内核编译用），libpcap 的 parser 生成依赖现成满足，无需改镜像。

## 验证结果

```text
$ file src/tcpdump-4.99.4/tcpdump
ELF 64-bit LSB executable, ARM aarch64, ..., statically linked, ...

$ ls -l build/rootfs/usr/bin/tcpdump
-rwxr-xr-x ... 6462224 ... build/rootfs/usr/bin/tcpdump
```

静态链接的 arm64 ELF，6.4MB（内嵌 libpcap + libc 副本），已随 initramfs 打包。

## 静态 vs 动态链接取舍

| 维度 | 静态（当前） | 动态 |
|------|-------------|------|
| 依赖 | 零，initramfs 自包含 | 需整套 glibc `.so` |
| 单工具体积 | 大（每个都内嵌 libc） | 小 |
| 多工具总体积 | 线性增长 | 共享 libc，反而省 |
| 启动复杂度 | 最低 | 要动态链接器 `ld.so` |
| 典型场景 | 嵌入式 / initramfs / 救援盘 | 完整发行版 |

**当前选静态的理由**：
1. initramfs 核心理念是"内核内建解包器、解出即用"，静态 BusyBox 让 `/init` 零前提启动。
2. rootfs 极简（BusyBox + init + udhcpc + tcpdump），静态冗余可忽略。
3. 规避 glibc 版本匹配问题。

**什么时候改动态**：往 rootfs 塞几十个工具、libc 副本冗余真正划不来时。那已经是"完整发行版"路线，会顺带深入讲共享库、`ld.so`、`ldd`、PLT/GOT。

## 陷阱

- 静态链接的代价是**体积**：BusyBox 和 tcpdump 各自内嵌一份 libc 副本，这是最好的现场教学素材。
- 动态链接不是"关掉 CONFIG_STATIC"就完事，还要拷 glibc `.so` + 动态链接器，且路径（`/lib`、`ld.so.cache`）和版本必须匹配。
- tcpdump 静态链接若不禁用 crypto，会拖入 zlib/dl 依赖，链接更易失败。
