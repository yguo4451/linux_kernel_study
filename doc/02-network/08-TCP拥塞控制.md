# 10 TCP 拥塞控制：慢启动与拥塞避免

TCP 的灵魂。核心问题：发送方不知道网络带宽，只能靠"丢包"这个信号自己探测能发多快。

## 一、两个窗口

```
发送量 = min(rwnd, cwnd)
         rwnd：接收窗口，对端广播（流量控制）
         cwnd：拥塞窗口，发送方探测（拥塞控制）
```

## 二、三阶段

| 阶段 | cwnd 增长 | 进入条件 |
|---|---|---|
| 慢启动 | 指数（每 RTT 翻倍） | cwnd < ssthresh |
| 拥塞避免 | 线性（每 RTT +1） | cwnd ≥ ssthresh |
| 快速恢复 | 砍半后重来 | 丢包（3 dup ACK / 超时） |

## 三、核心源码（Reno，net/ipv4/tcp_cong.c）

**① 慢启动 tcp_slow_start（:458）**：cwnd += acked，不超过 ssthresh

```c
u32 cwnd = min(tcp_snd_cwnd(tp) + acked, tp->snd_ssthresh);
acked -= cwnd - tcp_snd_cwnd(tp);   // 超出的留给拥塞避免
tcp_snd_cwnd_set(tp, min(cwnd, tp->snd_cwnd_clamp));
```

**② 拥塞避免 tcp_cong_avoid_ai（:473）**：攒满一个 RTT 加 1

```c
tp->snd_cwnd_cnt += acked;
if (tp->snd_cwnd_cnt >= w) {          // w = cwnd
	u32 delta = tp->snd_cwnd_cnt / w;
	tp->snd_cwnd_cnt -= delta * w;
	tcp_snd_cwnd_set(tp, tcp_snd_cwnd(tp) + delta);
}
```

**③ Reno 决策 tcp_reno_cong_avoid（:498）**：先慢启动后拥塞避免

```c
if (tcp_in_slow_start(tp)) {
	acked = tcp_slow_start(tp, acked);
	if (!acked) return;
}
tcp_cong_avoid_ai(tp, tcp_snd_cwnd(tp), acked);
```

**④ 丢包乘法减 tcp_reno_ssthresh（:517）**：ssthresh = cwnd/2

```c
return max(tcp_snd_cwnd(tp) >> 1U, 2U);
```

**⑤ 入口 tcp_cong_control（tcp_input.c:3539）**：加窗 or 减窗

```c
if (tcp_in_cwnd_reduction(sk)) {
	tcp_cwnd_reduction(sk, ...);          // 丢包恢复 → 减窗
} else if (tcp_may_raise_cwnd(sk, flag)) {
	tcp_cong_avoid(sk, ack, acked_sacked); // 正常 → 加窗
}
```

## 四、AIMD

**Additive Increase（线性加）+ Multiplicative Decrease（乘法减）**：大家线性加、丢包砍半，保证公平收敛。

## 五、为什么

- 慢启动指数涨：不知带宽，快速探测
- 拥塞避免线性涨：接近瓶颈，谨慎试探
- 丢包砍半：快速响应拥塞

## 建议阅读顺序

```
① net/ipv4/tcp_cong.c      tcp_slow_start / tcp_cong_avoid_ai / tcp_reno_ssthresh
② net/ipv4/tcp_input.c     tcp_cong_control / tcp_may_raise_cwnd（入口）
③ include/net/tcp.h        tcp_in_slow_start / tcp_snd_cwnd / TCP_INIT_CWND
④ net/ipv4/tcp_cubic.c     CUBIC（进阶，下篇）
```
