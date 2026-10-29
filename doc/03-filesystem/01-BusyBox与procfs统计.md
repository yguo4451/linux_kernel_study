# 14. BusyBox netstat 为什么没有 -s：procfs 统计的真相

> 网络子系统（TCP 收尾）配套篇。回答一个问题：**BusyBox 能否"配置上" `netstat -s` 统计功能？**

## 结论先行

**不能。** BusyBox 的 `netstat` 从头到尾就没实现 `-s`，也没有对应的编译开关——这不是"被裁剪掉的功能"，而是"从未存在过的功能"。你在桌面 Linux 上用的 `netstat -s` 来自 **net-tools** 包，不是 BusyBox。

## 概念：两种 netstat

| | BusyBox netstat | net-tools netstat |
|---|---|---|
| 定位 | 最小可用、静态编译、省体积 | 功能完整、动态链接 |
| 能力 | 只列举 socket / 路由 | 列举 + 统计汇总（`-s`） |
| `-s` 选项 | 无 | 有 |
| 编译开关 | 仅 `NETSTAT` / `FEATURE_NETSTAT_WIDE`(-W) / `FEATURE_NETSTAT_PRG`(-p) | 无（全量） |

## 原理：统计数据在哪？

统计数据的**真实源头**是内核 procfs 的两个文件，工具只是"读取 + 格式化"的薄壳：

- `/proc/net/snmp` —— 标准 SNMP MIB 计数器（Ip / Icmp / IcmpMsg / Tcp / Udp / UdpLite）
- `/proc/net/netstat` —— Linux 扩展计数器（`TcpExt:`，含 TCPDelivered、TCPOrigDataSent 等 130+ 项）

内核里这两处都靠 `NET_INC_STATS` / `NET_ADD_STATS` 宏累加（`include/net/snmp.h:127`），并在 `net/ipv4/proc.c` 通过 `proc_create_net_single` 注册成文件（`snmp_seq_show`、`snmp4_tcp_list`）。

## 源码关键点：BusyBox 为什么没有 -s

`src/busybox-1.36.1/networking/netstat.c` 的 config 段只有三个选项：

```c
//config:config NETSTAT
//config:config FEATURE_NETSTAT_WIDE   → -W 宽列
//config:config FEATURE_NETSTAT_PRG    → -p PID/程序名
//（不存在 FEATURE_NETSTAT_STAT）
```

选项串把能接受的字母写死了：

```c
#define NETSTAT_OPTS "laentuwx" \
	IF_ROUTE(               "r") \
	IF_FEATURE_NETSTAT_WIDE("W") \
	IF_FEATURE_NETSTAT_PRG( "p")
```

`l a e n t u w x r W p` 里没有 `s`。所以 `netstat -s` 必然报 `invalid option -- 's'`，改多少次 `.config` 都没用。

## 实验验证

（已在 QEMU 中实测）

```sh
# 报错：没有 -s
~ # netstat -s
netstat: invalid option -- 's'
Usage: netstat [-ral] [-tuwx] [-enWp]

# 但数据源头一直在
~ # head -2 /proc/net/snmp
Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens ...
Tcp: 1 200 120000 -1 34 9 3 2 1 1234 ...
```

## 替代方案：scripts/netstat_stat.sh

用 awk 解析上述两个文件，模拟 `netstat -s`。已打进 rootfs 的 `/usr/bin/netstat_stat.sh`。

用法（QEMU 内）：

```sh
netstat_stat.sh          # 全部协议
netstat_stat.sh tcp      # 只看 TCP
netstat_stat.sh tcpext   # 只看 TcpExt（Linux 扩展计数器）
```

核心逻辑：`/proc/net/snmp` 每个协议是"标题行 + 值行"两行一组，脚本把标题行存进关联数组，值行对应打印 `字段 = 值`。

## 陷阱

- 别再试图 `make menuconfig` 打开 netstat 的 `-s`——没有这个开关，重编译一万次也不会有。
- 计数器是**开机至今的累积值**，要看"增量"需两次采样做差（如测 2MB 传输：传前读一次、传后读一次）。
- `/proc/net/snmp` 的 `Tcp:` 段（标准 MIB）和 `/proc/net/netstat` 的 `TcpExt:` 段（Linux 扩展）是两处，别混淆。
