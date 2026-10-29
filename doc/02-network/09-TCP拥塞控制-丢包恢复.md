# 11 TCP 拥塞控制：丢包恢复（快速重传与快速恢复）

拥塞控制的后半：丢包怎么发现、怎么恢复。

## 一、丢包检测三种信号

| 信号 | 机制 | 速度 | 可靠性 |
|---|---|---|---|
| RTO 超时 | 重传定时器超时 | 秒级 | 最可靠 |
| 快速重传 | 3 dup ACK（TCP_FASTRETRANS_THRESH=3, tcp.h:80） | RTT 级 | 较可靠 |
| SACK/RACK | 对端明确告知哪些段收到 | 最快最准 | 最准 |

## 二、拥塞状态机（tcp_input.c 注释）

```
Open(正常) → Disorder(乱序) → Recovery(快速恢复) / Loss(RTO)
```

- Open：fast path
- Disorder：看到 dup ACK/SACK，可能只是乱序
- CWR：ECN 拥塞通知，已减窗
- Recovery：3 dup ACK 触发，快速恢复
- Loss：RTO 超时

## 三、快速重传 + 快速恢复

**tcp_time_to_recover（:2348）**：

```c
if (tp->lost_out) return true;                       // 丢包已证实
if (!tcp_is_rack(sk) &&
    tcp_dupack_heuristics(tp) > tp->reordering)      // dup ACK 超阈值
	return true;
```

tcp_dupack_heuristics（:2246）= sacked_out + 1，reordering 默认 3。

**tcp_enter_recovery（:2845）**：ssthresh = cwnd/2（乘法减），进 Recovery。

Reno 经典公式：

```
ssthresh = cwnd / 2
cwnd = ssthresh + 3*MSS        （+3 补偿已离网段）
每个额外 dup ACK → cwnd += MSS
新 ACK → cwnd = ssthresh（退出）
```

## 四、RTO 超时（Loss）

**tcp_enter_loss（:2162）**：ssthresh=cwnd/2，cwnd→1 重新慢启动，状态 Loss。

| | 3 dup ACK | RTO 超时 |
|---|---|---|
| cwnd 降到 | ssthresh+3MSS | ~1 |
| 含义 | 个别包丢 | 网络大问题 |
| 恢复 | 快速 | 慢启动重来 |

## 五、误判撤销（undo）

Linux 精妙处：丢包判断可能错（只是乱序）。撤销函数：

```
tcp_try_undo_recovery  ← 快速恢复误判
tcp_try_undo_loss      ← Loss 误判（F-RTO）
tcp_try_undo_dsack     ← D-SACK 证明重传多余
```

原理：重传的数据后来证明没丢（D-SACK/时间戳），把 cwnd 减小的部分加回。

## 六、全貌

```
丢包信号
  ├─ 3 dup ACK → Recovery（cwnd=ssthresh+3MSS）
  ├─ RTO 超时  → Loss（cwnd→1）
  └─ SACK/RACK → 精确定位，tcp_mark_head_lost

恢复完成 → 新 ACK → Open
误判     → tcp_try_undo_* → 撤销
```

## 建议阅读顺序

```
① include/net/tcp.h       TCP_CA_* 状态 / TCP_FASTRETRANS_THRESH
② tcp_input.c             tcp_time_to_recover / tcp_enter_recovery / tcp_enter_loss
③ tcp_input.c             tcp_fastretrans_alert（状态机主入口）
④ tcp_input.c             tcp_try_undo_*（误判撤销）
```
