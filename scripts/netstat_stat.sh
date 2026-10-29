#!/bin/sh
# netstat_stat.sh —— 用 awk 解析 /proc/net/snmp 与 /proc/net/netstat，
# 模拟 net-tools 中 `netstat -s` 的统计输出。
#
# 背景：统计数据的真实源头是内核 procfs 的这两个文件，工具只是"读取+格式化"。
#       BusyBox 的 netstat 刻意不实现 -s（也没有编译开关），故用本脚本替代。
#       详见 doc/03-filesystem/01-BusyBox与procfs统计.md
#
# 用法：
#   netstat_stat.sh          # 显示全部协议统计
#   netstat_stat.sh tcp      # 只看 TCP
#   netstat_stat.sh tcpext   # 只看 TcpExt（Linux 扩展计数器）
#   netstat_stat.sh ip|icmp|icmpmsg|udp|udplite
#
# 环境：QEMU 内 BusyBox（sh + awk）；也可在任何有 awk 的 Linux 上使用。

filter="${1:-all}"

dump() {
    awk -v want="$filter" '
    /^[A-Za-z][A-Za-z0-9]*:/ {
        name = substr($1, 1, length($1) - 1)   # 去掉结尾冒号，如 "Tcp:" -> "Tcp"
        key  = tolower(name)                    # 过滤比较用小写
        if (!(name in seen)) {
            # 标题行：把字段名存起来（下标 = 协议名:列号）
            for (i = 2; i <= NF; i++) header[name ":" i] = $i
            seen[name] = 1
        } else {
            # 值行：按过滤条件输出
            if (want == "all" || want == key) {
                print ""
                print "== " name " =="
                for (i = 2; i <= NF; i++)
                    printf "  %-30s %s\n", header[name ":" i], $i
            }
        }
    }' "$1"
}

dump /proc/net/snmp
dump /proc/net/netstat
