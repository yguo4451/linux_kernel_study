# 20. 从 lsof 看 /proc 与文件描述符

> 工具原理篇，也是 doc/17（/proc vs /sys）的姊妹篇。doc/17 讲 /proc 的**内核变量面**（/proc/sys），本篇讲 /proc 的**进程信息面**（/proc/PID/fd），两篇拼成 /proc 的全貌。

## 一句话原理

**lsof 不侵入内核，它就是个"读 /proc 的程序"**——遍历所有进程的 `/proc/PID/fd/` 目录，把每个文件描述符的符号链接目标读出来，再对 socket 类 fd 去 `/proc/net/*` 按 inode 匹配出地址端口。

## 核心机制一：/proc/PID/fd 是内核动态生成的

源码 `fs/proc/fd.c:175`：

```c
static int proc_fd_link(struct dentry *dentry, struct path *path)
{
    task = get_proc_task(d_inode(dentry));   // 拿到 task_struct
    fd   = proc_fd(d_inode(dentry));         // 拿到 fd 号
    fd_file = fget_task(task, fd);           // 取出 struct file
    *path = fd_file->f_path;                 // 符号链接指向 file->f_path
}
```

内核把每个进程的 `task_struct → files_struct → fdtable → file` 链，通过 procfs 暴露成 `/proc/PID/fd/N` 的符号链接。这就是"一切皆文件"和"文件描述符"的内核实拍。

## 核心机制二：socket 没有路径，靠 inode 二次匹配

- 普通文件：`/proc/PID/fd/3 → /home/user/a.txt`，直接读路径即可
- 网络 socket：`/proc/PID/fd/3 → socket:[417]`，`417` 是 socket 的 inode 号

socket 没有磁盘路径，lsof 看到 `socket:[inode]` 后，再去 `/proc/net/tcp`（源码 `net/ipv4/tcp_ipv4.c:2747` `tcp4_seq_show` 生成）按 inode 匹配，得到四元组（源 IP:端口 → 目的 IP:端口）和状态。

## 完整工作流程

```
lsof -i :2323
  │
  ├─ ① 遍历 /proc 下所有数字目录（= 所有进程 PID）
  │
  ├─ ② 对每个 PID 读 /proc/PID/fd/* 的符号链接
  │       /proc/19827/fd/11 → socket:[inode]
  │
  ├─ ③ 判断类型：普通文件直接读；socket 记下 inode
  │
  ├─ ④ 去 /proc/net/tcp|udp|unix 按 inode 匹配 → 地址、端口、状态
  │
  └─ ⑤ 过滤端口 == 2323 → 输出
```

## lsof 参数（`-nP -i` 黄金组合）

| 参数 | 含义 |
|------|------|
| `-n` | 不反向解析 IP → 主机名（跳过 DNS，快） |
| `-P` | 端口不转服务名（显示数字） |
| `-i` | 只列网络 socket，语法 `[协议][@地址][:端口]` |

```sh
lsof -i :2323              # 所有协议 2323 端口
lsof -i tcp:2323           # 只 TCP
lsof -i tcp@127.0.0.1:22   # 指定本地地址
```

输出列：`COMMAND PID USER FD TYPE NODE NAME`，其中 `NAME` 的 `*:2323 (LISTEN)` 表示监听所有地址的 2323。

## 动手验证（QEMU 内实拍）

```sh
nc -l -p 9999 &
PID=$(pidof nc)
ls -l /proc/$PID/fd
```

```
0 -> /dev/ttyAMA0     ← 标准输入（串口）
1 -> /dev/ttyAMA0     ← 标准输出
2 -> /dev/ttyAMA0     ← 标准错误
3 -> socket:[417]     ← 监听 socket（inode 417）
```

三个发现：

1. **fd 0/1/2 全指向 `/dev/ttyAMA0`**：QEMU console 能交互的原因——标准流接到串口。
2. **fd 3 是 `socket:[417]` 不是路径**：socket 只有 inode，需二次匹配。
3. **inode 417 匹配到 `/proc/net/tcp` 的 `00000000:270F 状态 0A`**：

```
270F = 9999（十六进制端口）
0A   = 10  = TCP_LISTEN（内核常量）
```

所以 lsof 的真相就是这几步手动操作：`/proc/PID/fd/3 → socket:[417] ──inode匹配──▶ /proc/net/tcp 的 0.0.0.0:9999 LISTEN`，lsof 只是把它自动化 + 遍历所有进程。

## 为什么 lsof 慢、ss 快

lsof 要遍历**所有进程 × 所有 fd**，每个 fd 一次 readlink，极重。查网络用 `ss` 更快（直接读 `/proc/net/*`，跳过 fd 遍历）。lsof 的杀手锏是"什么都能查"（文件、socket、管道通吃）。

## 与 doc/17 的关系

```
/proc 的两个面：
├─ 内核变量面：/proc/sys/*      → doc/17（sysctl，ctl_table）
└─ 进程信息面：/proc/PID/*      → doc/20（fd，file 结构）
```

本篇的 `/proc/PID/fd` 属于"进程信息面"。CFS 调度学习会大量读 `/proc/PID/sched`、`/proc/PID/status`，都是同一族的读法。
