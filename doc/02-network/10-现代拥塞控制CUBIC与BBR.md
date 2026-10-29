# 12 现代拥塞控制：CUBIC 与 BBR

Linux 默认算法 CUBIC，替代 Reno；Google BBR 是另一流派。

## 一、CUBIC 核心：三次函数

```
cwnd = C × (t - K)³ + W_max
```

| 参数 | 含义 | 源码 |
|---|---|---|
| C | 缩放因子 ≈0.4 | bic_scale=41 |
| t | 距上次丢包时间 | epoch_start |
| K | 到达 W_max 时间 | cubic_root(...) |
| W_max | 上次丢包窗口 | last_max_cwnd |

## 二、三段曲线（tcp_cubic.c:214 bictcp_update）

```
cwnd
 │          ╱ 凹区域（缓慢探测新带宽）
 │      ╱
 │  ╱ 平台区（≈W_max，几乎不涨）
 │╱ 凸区域（快速恢复）
 └──────────────────→ t
     t<K     t=K    t>K
```

```c
offs = |t - bic_K|;
delta = (cube_rtt_scale * offs^3) >> ...;
if (t < bic_K)
	bic_target = bic_origin_point - delta;  // 凸：W_max - delta
else
	bic_target = bic_origin_point + delta;  // 凹：W_max + delta
cnt = cwnd / (bic_target - cwnd);
```

## 三、丢包处理（cubictcp_recalc_ssthresh :341）

```c
if (cwnd < last_max_cwnd && fast_convergence)
	last_max_cwnd = cwnd * (BICTCP_BETA_SCALE + beta) / (2*BICTCP_BETA_SCALE);
else
	last_max_cwnd = cwnd;
return max(cwnd * beta / BICTCP_BETA_SCALE, 2);   // ssthresh = cwnd×0.7
```

β = 0.7（beta=717/1024），比 Reno 的 0.5 温和。

## 四、CUBIC vs Reno

| | Reno | CUBIC |
|---|---|---|
| 增长依据 | ACK 计数（每 RTT +1） | 时间（三次函数） |
| 恢复 | 线性慢 | 三次曲线快 |
| 减窗 | β=0.5 | β=0.7 |
| 高 BDP | 收敛慢 | 收敛快 |
| RTT 依赖 | 依赖 | 不依赖 |

## 五、配套机制

- TCP friendliness：不低于 Reno 吞吐
- hystart：混合慢启动，检测 ACK 间隔/RTT 变化提前退出

## 六、BBR（Google 2016）

基于模型而非丢包：测量瓶颈带宽 + 最小 RTT，维持 inflight ≈ BDP。

| | 丢包驱动（Reno/CUBIC） | BBR |
|---|---|---|
| 依据 | 丢包 | 带宽+RTT 测量 |
| 核心 | 丢包减窗 | inflight ≈ BDP |
| 解决 | 高 BDP 收敛慢 | bufferbloat |

## 建议阅读顺序

```
① net/ipv4/tcp_cubic.c   bictcp_update（三次函数核心）
② net/ipv4/tcp_cubic.c   cubictcp_recalc_ssthresh（丢包）
③ net/ipv4/tcp_cubic.c   hystart_update（混合慢启动）
④ net/ipv4/tcp_bbr.c     BBR（进阶）
```

## 附：CUBIC 的可配置性（三层）

### ① CUBIC 模块参数（全局，/sys/module/tcp_cubic/parameters/）

| 参数 | 默认 | 含义 |
|---|---|---|
| beta | 717 | 乘法减因子（717/1024≈0.7） |
| bic_scale | 41 | 三次函数缩放（C≈0.4） |
| fast_convergence | 1 | 快速收敛 |
| tcp_friendliness | 1 | TCP 友好性 |
| hystart | 1 | 混合慢启动 |
| hystart_detect | 3 | 检测机制（1 ack train / 2 delay / 3 both） |
| hystart_low_window | 16 | hystart 触发 cwnd 下界 |
| hystart_ack_delta_us | 2000 | ACK train 间隔 |
| initial_ssthresh | 0 | 初始 ssthresh |

改法：`echo 600 > /sys/module/tcp_cubic/parameters/beta`

### ② 选择算法（3 粒度）

```bash
echo cubic > /proc/sys/net/ipv4/tcp_congestion_control   # 全局默认
setsockopt(fd, TCP_CONGESTION, "bbr")                     # per-socket
ip route add ... congctl cubic                            # 路由级
```

可用算法看 `/proc/sys/net/ipv4/tcp_available_congestion_control`（本内核 = reno cubic）。

### ③ 通用 TCP sysctl（/proc/sys/net/ipv4/tcp_*）

影响所有算法（sack、timestamps、syn_retries 等）。

**核心认知**：模块参数 = 调算法行为（β/hystart）；sysctl = 选算法（cubic/reno/bbr）。先选算法，再调参数。
