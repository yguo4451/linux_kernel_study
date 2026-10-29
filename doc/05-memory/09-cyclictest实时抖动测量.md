# cyclictest 实时抖动测量：实时栈的收口与"测不准"的真相

> 内存管理子系列第 9 篇（实时栈收口篇）。前面拼好了三块拼图：`isolcpus`（CPU 隔离）、`sched_fifo`（实时调度）、`mlockall`（内存驻留）。本篇用 cyclictest 实测抖动，亲手验证这套组合拳——并揭示一个汽车行业的关键真相：**实时性必须用真机测量，QEMU 测不准。**

## 一、cyclictest 测的是什么

cyclictest 是 rt-tests 套件的核心工具，测量**唤醒延迟**：

1. 线程用 `clock_nanosleep(TIMER_ABSTIME)` 按固定周期（如 1ms）睡眠
2. 醒来立刻读当前时间，与"应该醒来的时间"做差
3. 差值就是"晚醒多少"——即**调度延迟 / 抖动**

实时系统最怕的不是"平均慢"，而是"最坏情况不可预测"。所以 cyclictest 的关键指标是 **Max（最坏延迟）**，以及延迟的**分布**。

```sh
cyclictest -t1 -p 99 -i 1000 -l 10000 -m -a 3
# T: 0 (129) P:99 I:1000 C:10000 Min:11 Act:64 Avg:90 Max:62687
```

| 参数 | 含义 |
|------|------|
| `-t1` | 1 个测量线程 |
| `-p 99` | SCHED_FIFO 优先级 99（实时最高）|
| `-i 1000` | 周期 1000μs（每 1ms 醒一次）|
| `-l 10000` | 循环 10000 次 |
| `-m` | `mlockall` 锁页（内存驻留）|
| `-a 3` | 绑定到 CPU 3（隔离核）|
| `-h 100 -q` | 输出 0~100μs 直方图，安静模式 |

## 二、编译部署

rt-tests 2.11 静态编译进 initramfs（`build_in_container.sh` 已固化此步骤）：

```sh
make -C "$RT_DIR" cyclictest LDFLAGS="-static"
```

两个踩坑：

1. **rt-tests 2.11 硬依赖 libnuma**：`rt_numa.h` 无条件 `#include <numa.h>`，且链接 `-lnuma`。必须在 builder 镜像装 `libnuma-dev`（静态库 `libnuma.a` 也由它提供）。
2. **Makefile 需要 python3**：`PYLIB ?= $(shell python3 -m get_pylib)`，缺 python3 虽不致命但报错。一并装上 `python3 python3-dev`。

Dockerfile 同步加了这两个包，保证 `build.sh` 重建镜像时可复现。

## 三、先验证隔离真的生效

测之前先确认 CPU 3 真的被隔离了，否则数据没有意义：

```sh
cat /sys/devices/system/cpu/isolated
# 3     ← 内核调度子系统维护的隔离 CPU 集合

cat /proc/cmdline
# ... isolcpus=3 nohz_full=3 rcu_nocbs=3 irqaffinity=0-2 ...

cat /proc/interrupts
# CPU3 那一列应该几乎全 0（irqaffinity=0-2 把中断都赶到 0-2）
```

三处齐备 = 隔离生效：无中断、无 tick、无普通进程，只有你手动钉上去的实时任务能上 CPU 3。

## 四、关键教训 1：TCG 软件模拟会把测量彻底毁掉

最初 `run_qemu.sh` 用 QEMU 默认的 **TCG 软件模拟**，每条 guest 指令都要翻译执行，噪声地板高达 ~900μs，把 isolcpus 那几十微秒的收益彻底淹没。

| 指标 | TCG（软件模拟）| HVF（硬件加速）|
|------|------|------|
| Min | 30μs | **10μs** |
| Avg | ~900μs | **~80μs**（↓11×）|
| Max | 4~71ms 随机 | 11~89ms 仍随机 |

修复：Apple Silicon 的 QEMU 支持 **HVF 硬件加速**（Hypervisor.framework），guest 原生执行、不再逐条翻译：

```bash
# scripts/run_qemu.sh
ACCEL="${ACCEL:-hvf}"
CPU="${CPU:-host}"      # HVF 要求 CPU 模型匹配宿主，不能用 cortex-a53
...
exec qemu-system-aarch64 -machine virt -accel "$ACCEL" -cpu "$CPU" ...
```

确认宿主机支持：

```sh
qemu-system-aarch64 -accel help
# Accelerators supported in QEMU binary:
# hvf
# tcg
```

**结论**：在虚拟化环境测实时，必须开硬件加速，否则软件模拟的噪声直接判测量无效。

## 五、关键教训 2：Max 是"宿主调度运气"，不是内核延迟

即使开了 HVF，Max 依然 11~89ms 乱跳，且与隔离无关。看这组对照（同一台机器、连续四次）：

| 场景 | Min | Avg | Max |
|------|-----|-----|-----|
| CPU3 无负载 | 11 | 90 | 62687 |
| CPU0 无负载 | 10 | 78 | 89012 |
| CPU3 加压 | 11 | 79 | 73454 |
| CPU0 加压 | 9 | 37 | 11725 |

**反常点**：理论上隔离核该更稳，结果 CPU0 加压的 Max（11ms）反而最好。原因——**Max 是 10000 次采样里的单次离群点**，完全取决于那 10 秒内 macOS 什么时候恰好抢占 vCPU 线程：

- guest 的 vCPU 只是 macOS 上的普通线程
- macOS 随时能把 vCPU 抢占 10~80ms
- 这是 guest 内核**看不见也管不着**的抖动

> **汽车行业金科玉律**：实时性保证必须来自真实硬件。虚拟化层的宿主调度抖动会直接击穿 guest 内所有 isolcpus/PREEMPT_RT 的努力。这就是为什么车载 ECU 跑在裸机/专用 SoC，而不是共享宿主的 VM。

## 六、直方图：base 与 tail 的两段式真相

Max 被污染，但**分布的主体**能反映真实结构。用直方图看（100000 次采样）：

```sh
cyclictest -t1 -p 99 -i 1000 -l 100000 -m -a 3 -h 100 -q
cyclictest -t1 -p 99 -i 1000 -l 100000 -m -a 0 -h 100 -q
```

实测：

| | CPU3（隔离）| CPU0（普通）|
|---|---|---|
| Min | 8μs | 8μs |
| Avg | 52μs | 43μs |
| 峰值（众数）| 17μs（5113 次）| 16μs（6461 次）|
| Max | 81126μs | 74866μs |
| 溢出（>100μs）| 6186 次（6.2%）| 2080 次（2.1%）|

直方图揭示**两段式结构**：

1. **主体峰（8~30μs，峰值 ~16μs）**：这是 HVF 下 guest 定时器唤醒路径的真实成本——hrtimer 触发 → 中断 vCPU → 唤醒 SCHED_FIFO 任务 → 读时间戳。**两颗核完全一致**，因为走的是同一条 VM 定时器投递路径。这正是 HVF 把噪声地板从 900μs 压到 ~16μs 的成果。

2. **长尾（>100μs，2%~6% 的样本）**：这是 macOS 宿主抢占 vCPU 造成的。溢出次数 6186 vs 2080 的差异，是两次 100 秒测量窗口里宿主调度"运气"的差异，**不是 isolcpus 的信号**。

**为什么 isolcpus 的收益在直方图里看不见**：

- isolcpus 的真实收益在真机上是"去掉 10~50μs 的中断/tick 干扰"（微秒级）
- 而 VM 的定时器投递基础成本已高达 ~16μs，宿主的 ms 级抢占尖峰又叠在上面
- 信号（几 μs）被噪声（16μs base + ms 级尖峰）完全淹没

## 七、最终结论

实时栈三块拼图全部正确构建并验证：

```
isolcpus（CPU 隔离）✅ → sched_fifo（实时调度）✅ → mlockall（内存驻留）✅
```

本次测量收获了两条比数字更重要的工程结论：

1. **虚拟化测实时，硬件加速是底线**：TCG 的 900μs 噪声 vs HVF 的 16μs 基础成本，差 50 倍。
2. **实时性的最终裁判是真机**：QEMU 的宿主调度抖动（ms 级）永远高于 isolcpus 的收益（μs 级）。要拿到可信的 Max 数据，需要裸机 arm64 板子（树莓派 / 车载 SoC）跑同一套 `isolcpus + sched_fifo + mlockall + cyclictest`。

## 八、参数速查表

| 参数 | 含义 |
|------|------|
| `-t N` | N 个测量线程 |
| `-p N` | SCHED_FIFO 优先级 N |
| `-i N` | 周期 N 微秒 |
| `-l N` | 循环 N 次 |
| `-m` | mlockall 锁页 |
| `-a CPU` | 绑定到 CPU（配合 isolcpus 用）|
| `-n` | 用 clock_nanosleep（默认即此）|
| `-b N` | busy-loop 模式，测"忙等+计时"延迟 |
| `-h N` | 输出 N 格直方图（每格 1μs）|
| `-q` | 安静模式，只输出直方图 |
| `-D N` | 测试时长 N 秒（替代 -l）|

## 九、衔接下一步

实时栈的内核侧拼图已经完整。往后两个方向：

1. **真机实测**：把 `initramfs.cpio.gz` + 内核烧到树莓派/ARM 板，跑同一套 cyclictest，拿到可信 Max。
2. **PREEMPT_RT 深度对比**：同一块板子上关掉 PREEMPT_RT 重测，量化"完全可抢占"对最坏延迟的改善（这是 rt-tests 诞生的原始目的）。
