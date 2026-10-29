# TCP 传输控制协议

TCP = Transmission Control Protocol（传输控制协议），四层协议栈的传输层。

## 在协议栈中的位置

```
应用层   HTTP / DNS / ...
   ↓
传输层   TCP / UDP          ← 本主题
   ↓
网络层   IP（路由、寻址、NAT）
   ↓
链路层   以太网 / ARP
```

## 核心职责：在不可靠的 IP 之上构建可靠传输

IP 会丢包、乱序、重复，但应用需要可靠的字节流。TCP 提供：

| 能力 | 机制 |
|---|---|
| 建立连接 | 三次握手 |
| 可靠 | 确认 ACK + 超时重传 |
| 有序 | 序号 sequence number |
| 流控 | 滑动窗口 |
| 全双工 | 双向同时收发 |

对比 UDP：无连接、不确认、不重传，简单但不可靠（DNS 查询用它）。

## 三次握手

```
客户端                          服务端
  │─── ① SYN, seq=x ───────────→│
  │←── ② SYN+ACK, seq=y, ack=x+1│
  │─── ③ ACK, ack=y+1 ─────────→│
  │◄────── ESTABLISHED ─────────►│
```

| 步 | 谁发 | 确认了什么 |
|---|---|---|
| ① SYN | 客户端 | 我能发 |
| ② SYN+ACK | 服务端 | 我能收，我也能发 |
| ③ ACK | 客户端 | 我也能收 |

**为什么是三次不是两次**：防止"迟到的旧 SYN"被服务端误认为新连接请求。三次后双方都确认"自己能发 + 对方能收"。

## 四次挥手（断开连接）

```
客户端                          服务端
  │─── ① FIN ──────────────────→│   我要关了
  │←── ② ACK ───────────────────│   收到（服务端可能还有数据要发）
  │←── ③ FIN ───────────────────│   我也关了
  │─── ④ ACK ──────────────────→│   收到
```

比握手多一步：因为 TCP 是全双工，两个方向要**分别**关闭，所以是四次。

## TCP 状态机（11 个状态）

LISTEN → SYN_SENT → SYN_RECV → ESTABLISHED → FIN_WAIT_1/2、CLOSE_WAIT、LAST_ACK → TIME_WAIT → CLOSED

`/proc/net/tcp` 的 `st` 字段对应状态（十六进制）：

| st | 状态 |
|---|---|
| 0A | LISTEN |
| 02 | SYN_SENT |
| 03 | SYN_RECV |
| 01 | ESTABLISHED |
| 06 | TIME_WAIT |
| 07 | CLOSE |
| 08 | CLOSE_WAIT |
| 09 | LAST_ACK |

## 实验命令

```sh
netstat -tan           # 看 TCP 连接表
wget http://example.com  # 触发真实 TCP 连接
netstat -tan           # 看连接状态变化
cat /proc/net/tcp      # 内核原始 socket 表（st 字段）
```

## 观察 ESTABLISHED 连接的技巧

wget 太快，连接瞬间关闭看不到。用 nc 建一条保持打开的连接：

```sh
# 用 sleep 撑住 stdin（不 EOF、不读 tty），避免 nc 发 FIN 或被 SIGTTIN 停
sleep 100 | nc -l -p 12345 > /dev/null &
sleep 100 | nc 127.0.0.1 12345 > /dev/null &
netstat -tan      # 看到 ESTABLISHED
cat /proc/net/tcp # st = 01
killall -9 nc     # 清理
```

stdin 来源对 nc 的影响：

| stdin | 行为 |
|---|---|
| tty（后台） | SIGTTIN 停住 |
| /dev/null | 立即 EOF → 发 FIN 退出 |
| 保持打开的管道（sleep） | 不 EOF、不停 → 连接保持 ✅ |

## /proc/net/tcp 十六进制解码

```
  sl  local_address rem_address   st
   0: 0100007F:3039 0100007F:ACED 01
```

- **IP 是 little-endian（字节反序）**：`0100007F` → 反着读 = `127.0.0.1`
- **端口是普通十六进制**：`3039` = 12345，`ACED` = 44269
- **st 字段**：TCP 状态（01=ESTABLISHED，05=FIN_WAIT2，08=CLOSE_WAIT...）

同一连接在内核表里有**两条记录**（两端各一个 socket，本地/远端视角互换）。

## 实战：CLOSE_WAIT 泄漏

- CLOSE_WAIT = 对端已发 FIN，但本端应用**忘了 close()**
- 这是生产环境最常见的 TCP 泄漏 bug：连接永久残留，耗尽文件描述符
- 清理被 SIGTTIN 停止的进程要用 `killall -9`（SIGKILL），普通 SIGTERM 对停止进程无效

## 实战：CLOSE_WAIT 排查定位（inode 反查法）

排查回答两问：**① 是哪条连接（五元组）② 是哪个进程（inode → fd 反查）**。

桥梁是 **inode**：`/proc/net/tcp` 每行最后一列是 socket 的 inode，而进程打开该 socket 时 `/proc/<pid>/fd/<n>` 是软链接指向 `socket:[inode]`。把内核态连接和用户态进程串起来。

### 方法一：完整工具（标准 Linux）

```bash
ss -tanp | grep CLOSE-WAIT      # -p 直接带进程名/PID
netstat -tanp | grep CLOSE_WAIT
lsof -i tcp -nP | grep CLOSE_WAIT
lsof -p <pid>                   # 看该进程打开的所有 fd
```

### 方法二：裸 /proc（无 ss/lsof，任何 Linux 通用）

```bash
# ① 锁定 st=08 的行，解码五元组（最后一列是 inode）
awk 'NR==1 || $4=="08"' /proc/net/tcp
#   1: 0100007F:3039 0100007F:88ED 08 ... 422
#       127.0.0.1:12345 ← 127.0.0.1:35053   inode=422

# ② 用 inode 反查进程（遍历所有 fd 软链接）
for fd in /proc/[0-9]*/fd/*; do
  [ -L "$fd" ] && [ "$(readlink "$fd")" = "socket:[422]" ] && echo "$fd"
done
# /proc/61/fd/3

# ③ 确认进程身份
cat /proc/61/cmdline    # nc -l -p 12345
```

### 本次实战记录（QEMU 内实测）

| 步骤 | 现象 |
|---|---|
| 服务端 `nc -l -p 12345` + 客户端 `nc 127.0.0.1 12345` | 两条 st=01（ESTABLISHED），inode 422/423 |
| kill 客户端（主动 FIN） | 服务端 st=08（CLOSE_WAIT），inode=422；客户端 st=05（FIN_WAIT2） |
| inode 422 反查 | `/proc/61/fd/3` → PID 61 = `nc -l -p 12345` ✅ |
| kill 服务端清理 | 客户端孤儿 socket 05 → 06（TIME_WAIT） |

### 排查要点

- CLOSE_WAIT 卡在**被动关闭方**，漏 `close()` 的是**本端应用**，不是对端
- **inode 判据**：inode 非 0 = 有进程持有，可反查定位；inode = 0 = 进程已退出、fd 全关，socket 成孤儿（只剩 TCP 状态机引用，如 FIN_WAIT2/TIME_WAIT 等超时善后）
- 找到进程后看它为什么没 close：`read()` 返回 0（对端关闭）后没 break？异常路径漏了 `close()`？还是循环里忘了退出条件

### 延伸：pipe 也能用 inode 反查

`ls -l /proc/<pid>/fd` 里除了 `socket:[inode]`，还会看到 `pipe:[inode]`。pipe 和 socket 本质一样——都是文件、都有 inode，用同一套"遍历 `/proc/*/fd`"手法反查两端。

区别在"全局表"：

| 对象 | 文件系统 | 全局表 | 反查方式 |
|---|---|---|---|
| socket | sockfs | `/proc/net/tcp`、`/proc/net/udp` | 查表 + 遍历 fd |
| pipe | pipefs | **无**（纯内存） | 只能遍历 `/proc/*/fd` |

实测（`sleep 100 | nc -l -p 12345`）：

```bash
# 反查 pipe 两端
PIPE_INODE=$(readlink /proc/158/fd/0 | sed 's/.*\[//;s/\]//')   # 949
for fd in /proc/[0-9]*/fd/*; do
  [ -L "$fd" ] && [ "$(readlink "$fd")" = "pipe:[$PIPE_INODE]" ] && echo "$fd"
done
# /proc/157/fd/1  ← 写端（sh，O_WRONLY）
# /proc/158/fd/0  ← 读端（nc 的 stdin，O_RDONLY）

# fdinfo 看详情
cat /proc/158/fdinfo/0   # pos:0  flags:00(读)  mnt_id:13  ino:949
cat /proc/157/fdinfo/1   # pos:0  flags:01(写)  mnt_id:13  ino:949
```

两个彩蛋：

- `ps` 里看不到 `sleep 100`：BusyBox ash 把 `sleep` 内联进子 shell 执行了，那个 shell 的 stdout 就是 pipe 写端
- 管道两端永远是**两个 fd**（读端 + 写端），分属不同进程（或同一进程）

### 反查只剩一端：EOF / SIGPIPE 判据

pipe 反查的结果本身就是诊断信息——**找到几个 fd，说明管道还剩几端**：

| 反查结果 | 含义 | 后果 |
|---|---|---|
| 2 个 fd（读+写） | 两端进程都活着 | 正常 |
| 只剩读端 | 写端已关闭（写进程退出） | 读端 `read()` 返回 0（**EOF**） |
| 只剩写端 | 读端已关闭 | 写端 `write()` 触发 **SIGPIPE** |

实测（`sleep 1 | nc -l -p 12345`）：

```bash
# sleep 1 还活着时：两端齐全
#   /proc/220/fd/1 -> pipe:[1170]   (写端)
#   /proc/221/fd/0 -> pipe:[1170]   (读端)

# sleep 1 到期后：只剩读端
#   /proc/221/fd/0 -> pipe:[1170]
```

这串起了前面 `sleep 100` 的用意：一旦写端（sleep）退出，nc 的 stdin 读到 EOF，nc 会关闭 socket 写方向、发 FIN——又回到 CLOSE_WAIT / TIME_WAIT 那套状态机了。

## TIME_WAIT 过多：机制 vs 工程问题

TIME_WAIT 是协议正确行为（主动关闭方等 2MSL 善后），本身不是 bug。但在"高并发 + 短连接 + 主动关闭方固定"场景下，60 秒等待 × 高连接速率 = 资源瓶颈（端口耗尽、fd 占用）。

### 解决方案（治本 → 治标 → 危险）

| 方案 | 思路 | 风险 |
|---|---|---|
| ① 长连接复用 | keep-alive / HTTP2 / 连接池，少关连接 | 无 |
| ② 让对端主动关闭 | TIME_WAIT 转移到对端 | 需改应用逻辑 |
| ③ 内核参数 | tcp_tw_reuse、扩大端口范围 | 低（治标） |
| ④ SO_LINGER+RST | 发 RST 跳过 TIME_WAIT | 高（可能丢数据） |

```bash
# ③ 内核参数示例
sysctl -w net.ipv4.tcp_tw_reuse=1
sysctl -w net.ipv4.tcp_timestamps=1
sysctl -w net.ipv4.ip_local_port_range="10000 65000"
```

注意：`tcp_tw_recycle` 已废弃（Linux 4.12 移除，NAT 下有问题）。

最佳姿势：**不破坏机制，而是少制造关闭**——用长连接和连接池从源头减少连接生命周期次数。

## 内核源码位置

- `net/ipv4/tcp_input.c`：收包处理（含三次握手的状态机）
- `net/ipv4/tcp_output.c`：发包处理
- `net/ipv4/tcp_ipv4.c`：IPv4 的 TCP 实现、socket 层
- `include/net/tcp_states.h`：状态定义
- `include/linux/skbuff.h`：sk_buff 数据结构

## 内核如何关联 fd 与 /proc/net/tcp（inode 桥梁）

一句话：**fd 和 `/proc/net/tcp` 通过 sockfs 的 inode 号对上**，两边读的是同一个 inode 的 `i_ino`。

### 核心设计：socket 和 inode 是同一块内存

`include/net/sock.h`：

```c
struct socket_alloc {
	struct socket socket;    // 网络端点
	struct inode  vfs_inode; // 文件 inode（"socket 是文件"的体现）
};
static inline struct inode *SOCK_INODE(struct socket *socket)
{
	return &container_of(socket, struct socket_alloc, socket)->vfs_inode;
}
```

分配时（`net/socket.c:302` `sock_alloc_inode`）两者同属一个 `kmem_cache` 对象。

### 链 A：fd 侧 → `socket:[ino]`

- `net/socket.c:462` `sock_alloc_file()`：`file->private_data = sock`，`alloc_file_pseudo(SOCK_INODE(sock), ...)`，file 的 inode 就是 socket 的 inode
- `fs/proc/base.c:1788` `proc_pid_readlink()` → `do_proc_readlink()` → `d_path()`
- `net/socket.c:360` `sockfs_dname()`：`"socket:[%lu]" = d_inode(dentry)->i_ino` ← `readlink` 显示的名字来源

### 链 B：/proc/net/tcp 侧 → inode 列

- `net/ipv4/tcp_ipv4.c:2747` `tcp4_seq_show()` 遍历 hash 表里的 `struct sock`，打印 `sock_i_ino(sk)`（2715 行）
- `net/core/sock.c:2623` `__sock_i_ino()`：`ino = sk->sk_socket ? SOCK_INODE(sk->sk_socket)->i_ino : 0`

两条链殊途同归，落到同一个 `socket_alloc` 的 inode 上，数字自然相同。

### 为什么孤儿 socket 的 inode = 0

`__sock_i_ino()` 里 `sk->sk_socket ? ... : 0`：进程退出 → fd 全关 → file/socket 释放 → `sk->sk_socket = NULL` → 返回 0。但 `struct sock` 还被 TCP 状态机引用（FIN_WAIT2/TIME_WAIT 善后），所以 `/proc/net/tcp` 仍有记录、inode 却是 0。**这就是"孤儿 socket"的判定依据**，也是排查时"inode 非 0 才反查"的原理。

### inode 唯一性：只在"单个文件系统内"

inode 号（`i_ino`）**只在同一个文件系统内唯一**，跨文件系统可以重复：

```sh
stat /tmp    # Device: 17h/23d  Inode: 1
stat /proc   # Device: 15h/21d  Inode: 1  ← 都是 1，但 Device 不同
```

- 真正全局唯一的标识是 **`(st_dev, st_ino)` 二元组**：设备号（哪个文件系统）+ inode 号（文件系统内哪个文件）
- **硬链接**：一个 inode 可对应多个文件名（`ln` 后 `ls -i` 同号，`Links` 计数）
- **inode 号会复用**：文件删除后其 inode 号可被新文件占用

#### (st_dev, st_ino) 唯一性的源码依据

`/tmp` vs `/proc` 撞 inode 1 的示例只证明了"dev 的必要性"，未证明组合的唯一性。唯一性由内核数据结构设计保证，看 `fs/stat.c` 的 `generic_fillattr()`：

```c
// fs/stat.c:52
stat->dev = inode->i_sb->s_dev;   // st_dev ← super_block 的设备号
stat->ino = inode->i_ino;         // st_ino ← inode 自己的编号
```

论证链：

```
① 每个 inode 属于且仅属于一个 super_block（inode->i_sb）
② i_ino 在这一个 super_block 内唯一          ← inode 分配器保证
③ s_dev 在整个系统内唯一标识这个 super_block  ← 设备号分配机制保证
∴ (st_dev, st_ino) 全局唯一
```

本质是 `(文件系统实例的全局 ID, 实例内文件编号)` 两级坐标。注意隐含前提：唯一性限定在**同一时刻**（inode 号会复用，卸载后设备号也可能被重新分配）。

**为什么 `socket:[625]` 能放心反查**：sockfs 是一个独立文件系统（`net/socket.c` 里 `init_pseudo(fc, SOCKFS_MAGIC)` 创建的全局匿名 super block），所有 socket 的 inode 都来自同一个 sockfs，所以在 sockfs 内 `i_ino` 唯一。fd 的 `socket:[ino]` 与 `/proc/net/tcp` 的 inode 列读的是同一个 sockfs inode，因此能对上。inode 反查成立的前提正是"限定在 sockfs 这一个文件系统内"。

### fd 与 inode：谁才是"资源标识"

同一份资源，不同进程的 fd 可以不同，但 inode（同文件系统内）相同：

```sh
# 三个进程打开同一文件 /tmp/res（inode 3）
#   /proc/53/fd/3  /proc/53/fd/4        ← shell 自己
#   /proc/285/fd/0 /proc/285/fd/3 ...   ← 进程 285
#   /proc/286/fd/0 /proc/286/fd/3 ...   ← 进程 286
# fd 号五花八门（0/3/4），inode 全是 3
```

| | fd | inode |
|---|---|---|
| 作用域 | 进程私有（每进程一张 fd 表） | 文件系统级（标识文件本身） |
| 同一资源 | 各进程 fd 可同可不同 | 号相同（同文件系统内） |
| 类比 | 借阅卡编号 | 藏书唯一编号 |

- **fd 相同 ≠ 同一资源**（进程 A 的 fd 3 和进程 B 的 fd 3 可指向不同文件）
- **inode 相同 = 同一资源**（同文件系统内）
- 同一进程两次 `open` 同一文件 → 两个独立 `struct file`（各自 offset），同一 inode
- **inode 号会复用**：文件删除后其号可给新文件，故 inode 适合短时标识、不适合持久化存储
