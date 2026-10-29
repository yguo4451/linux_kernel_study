# CPU 隔离与实时：isolcpus

> 配套程序：`experiments/cpu_isol/latency_test.c`（部署 `/usr/bin/latency_test`）
> QEMU 启动参数：`scripts/run_qemu.sh` 已加 `isolcpus=${ISOLCPUS:-3}`

## 一、为什么优先级最高还不够

RT 任务（SCHED_FIFO）优先级最高，能防住普通任务和低优先级 RT 任务，但防不住：

| 打断来源 | 优先级能防住吗 |
|----------|--------------|
| SCHED_NORMAL 任务 | ✅ |
| 低优先级 RT 任务 | ✅ |
| 内核线程（kworker/kthreadd）| ❌ |
| 中断（hardirq）| ❌ |
| 调度 tick / 负载均衡 | ❌ |
| RCU / 软中断 | ❌ |

**CPU 隔离（isolcpus）解决优先级管不到的那部分**：把一颗核从调度器公共池摘出，不让内核主动放任务、做均衡。

> 优先级控制"谁能抢"，CPU 隔离控制"这颗核上还跑不跑别人"。

## 二、两种隔离方式

| 方式 | 时机 | 特点 |
|------|------|------|
| `isolcpus=3` | 内核启动参数 | 静态，需重启 |
| cgroup cpuset | 运行时 | 动态，可随时调整 |

## 三、isolcpus 的源码机制（6.6）

```c
// 1. 解析启动参数 kernel/sched/core.c
__setup("isolcpus=", isolated_cpus_setup);   // 设置 isolcpus bitmap

// 2. housekeeping 框架 kernel/sched/isolation.c
//    隔离核从 housekeeping_cpumask(HK_TYPE_DOMAIN) 移除

// 3. init 进程 affinity 启动时设为 housekeeping CPU
//    → 所有 fork 后代默认 affinity 不含隔离核

// 4. load_balance (kernel/sched/fair.c) 只在 housekeeping domain 搬任务
//    → 普通任务不会被均衡到隔离核

// 5. sched_setaffinity 不受 isolcpus 限制
//    → RT 任务可显式 pin 到隔离核（实时标准用法）
```

## 四、实验验证

### 验证隔离生效

```sh
cat /proc/cmdline                      # → console=ttyAMA0 isolcpus=3
cat /sys/devices/system/cpu/isolated   # → 3
cat /sys/devices/system/cpu/present    # → 0-3
```

### 验证机制（确定性，不受 QEMU 时钟噪声影响）

```sh
cat /proc/self/status | grep Cpus_allowed_list
# → Cpus_allowed_list: 0-2   （默认 affinity 排除隔离核）

latency_test 3 80 1000        # 显式 pin 到隔离核 → 允许（隔离≠禁用）
```

结论：新任务默认 affinity 排除隔离核 + load_balance 不搬过去，但显式 `sched_setaffinity` 可 pin。

### 完整实时四件套验证（开启 NO_HZ_FULL / RCU_NOCB_CPU 后）

启动参数：`isolcpus=3 nohz_full=3 rcu_nocbs=3 irqaffinity=0-2`

```sh
cat /proc/cmdline                       # → console=ttyAMA0 isolcpus=3 nohz_full=3 rcu_nocbs=3 irqaffinity=0-2
cat /sys/devices/system/cpu/nohz_full   # → 3（需 CONFIG_NO_HZ_FULL）
cat /sys/devices/system/cpu/isolated    # → 3
cat /proc/interrupts                    # 看 arch_timer 与 virtio 中断分布
```

实测 `/proc/interrupts`（关键证据）：

```
arch_timer:  CPU0=8265  CPU1=212  CPU2=262  CPU3=22   ← nohz_full：CPU3 tick 几乎停止
virtio0:     全部在 CPU0，CPU3=0                        ← irqaffinity：外设中断绕开 CPU3
IPI0(Resched): CPU3=0                                  ← 隔离：无调度 IPI 打扰 CPU3
```

四件套分工：

| 参数 | 隔离什么 |
|------|---------|
| `isolcpus=3` | 任务（普通任务不放 CPU3）|
| `nohz_full=3` | 调度 tick（CPU3 单任务时关 tick）|
| `rcu_nocbs=3` | RCU 回调（不落 CPU3）|
| `irqaffinity=0-2` | 外设中断（不去 CPU3）|

## 五、QEMU 测性能的局限（重要踩坑）

实测 `latency_test` 在 QEMU 里的数据：

| 场景 | CPU0 max | CPU3 max |
|------|---------|---------|
| 无负载 | 14706 us | 10042 us |
| 有负载 | 7638 us | 26036 us |

结果"反直觉"（隔离核反而更抖），原因是：

1. **平均延迟 885us**：不是调度延迟，是 QEMU 时钟模拟精度只有毫秒级（真实硬件上 hrtimer 是微秒级）
2. **max 随机跳 7~26ms**：QEMU 是宿主机上一个进程，被 macOS 暂停时所有 vCPU 一起冻结，时钟跳变

**isolcpus 隔离不了虚拟化层**：QEMU 的时钟模拟、宿主机对 QEMU 进程的调度、virtio 中断，都远超 isolcpus 的效应范围。

> 结论：QEMU 适合验证 isolcpus 的**机制**（affinity、load_balance），测**性能**需真实硬件或 KVM。

## 六、isolcpus 的边界

```
isolcpus=3 之后：
  ├─ 普通任务默认 affinity：排除 3   ✅
  ├─ load_balance：不搬任务到 3      ✅
  ├─ 内核线程：绑定 housekeeping CPU  ✅
  ├─ 中断：默认仍可能到 3            ⚠️ 需 IRQ affinity 额外配置
  └─ 显式 pin 到 3：允许              ✅ 实时用法
```

## 七、通往下一课

isolcpus 只解决"任务放置"。要完整实时还需：cpuset（动态隔离）、cyclictest（最坏延迟测量）、PREEMPT_RT（内核可抢占）。
