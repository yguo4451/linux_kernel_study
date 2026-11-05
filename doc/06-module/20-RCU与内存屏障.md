# 40. RCU 与内存屏障：读侧零开销的延迟释放

> 阶段五第二十课。理解内核最独特的同步机制 RCU（Read-Copy-Update）：读侧零开销、写侧复制替换、宽限期延迟释放。实测 `synchronize_rcu()` 宽限期 **17~25ms**，揭示 RCU 在实时系统中的代价。顺带打通内存屏障（`smp_store_release`/`smp_load_acquire`）——RCU 正确性的地基。

## 一、RCU 解决什么问题

场景：**读极多、写极少**（路由表、设备列表、内核配置、`/proc` 数据）。

```
spinlock：读也要抢锁 → 读多时锁竞争爆炸 ❌
RCU：     读完全无锁  → 只在写时付出代价  ✅
```

## 二、RCU 三要素

```
① 读者（零开销）        ② 写者                    ③ 宽限期
rcu_read_lock()         复制 old → new            等所有"旧读者"退出
p = rcu_dereference()   修改 new
读 p                    rcu_assign_pointer(gp,new)
rcu_read_unlock()       synchronize_rcu()  ←──────┘
                        kfree(old)
```

**宽限期（Grace Period）**：

```
时间 ─────────────────────────────────────────►
读者A: [====读旧====]
读者B:        [====读旧====]
读者C:                     [==读新==]
写者:  替换指针 ────────────────────────► synchronize_rcu() 返回
                                        ↑ 旧版本确定无人引用 → 可 kfree
```

## 三、RCU API

### 读者侧（零开销）

```c
rcu_read_lock();                                  /* 不是锁！只是标记进入读临界区 */
struct config *cfg = rcu_dereference(g_config);   /* 安全取指针（含获取屏障） */
if (cfg)
    use(cfg->a, cfg->b, cfg->c);
rcu_read_unlock();
```

`rcu_read_lock()` **不阻塞、不睡眠、不等待**——只是标记当前任务在 RCU 读临界区（实质是操作 preempt_count）。读者侧零开销。

### 写者侧

```c
/* 1. 复制 */
new = kmalloc(sizeof(*new), GFP_KERNEL);
*new = *old;

/* 2. 改副本 */
new->field = new_value;

/* 3. 原子发布（含内存屏障） */
rcu_assign_pointer(g_config, new);

/* 4. 等宽限期 */
synchronize_rcu();

/* 5. 释放旧版本 */
kfree(old);
```

异步释放（不阻塞写者）：`call_rcu(&old->rcu_head, free_cb);`

### 写者读指针：`rcu_dereference_protected`

```c
struct config *old = rcu_dereference_protected(g_config, 1);
```

写者不用 `rcu_read_lock`，改用 `rcu_dereference_protected`（第二个参数 `1` 是 lockdep 条件，表示"我确定此刻安全"）。

## 四、内存屏障：RCU 的地基

```c
new->field = value;              /* ① 改副本 */
rcu_assign_pointer(gp, new);     /* ② 发布指针 */
```

若 CPU/编译器重排成"先 ② 再 ①"，读者看到指针但字段未写完 → 读到垃圾。

**`rcu_assign_pointer` / `rcu_dereference` 内部各含一道屏障**：

```
rcu_assign_pointer  →  smp_store_release()   发布屏障
rcu_dereference     →  smp_load_acquire()    获取屏障
```

保证：**指针发布前的所有写入，对拿到指针的读者一定可见**。

### 屏障速查

| 屏障 | 作用 |
|---|---|
| `smp_mb()` | 全屏障（读写都不重排） |
| `smp_rmb()` / `smp_wmb()` | 只约束读 / 只约束写 |
| `smp_store_release()` | 发布：之前的写对 acquire 方可见 |
| `smp_load_acquire()` | 获取：之后的读不提前到屏障前 |
| `barrier()` | 只阻止编译器重排 |

arm64 是**弱内存序**，重排比 x86 常见，屏障格外重要。

## 五、实验：RCU 保护配置结构体

```c
struct config {
	int version;
	int a, b, c;          /* 三者应始终等于 version */
};
static struct config __rcu *g_config;

/* 写者：复制-改-发布-等宽限期-释放 */
static void update_config(int ver)
{
	struct config *old = rcu_dereference_protected(g_config, 1);
	struct config *new = kmalloc(sizeof(*new), GFP_KERNEL);
	ktime_t t0, t1;

	*new = *old;
	new->version = ver;
	new->a = new->b = new->c = ver;

	rcu_assign_pointer(g_config, new);

	t0 = ktime_get();
	synchronize_rcu();
	t1 = ktime_get();
	pr_info("update ver=%d, grace period = %lld us\n",
		ver, ktime_to_us(ktime_sub(t1, t0)));

	kfree(old);
}

/* 读者：零开销 */
static void read_config(void)
{
	struct config *cfg;

	rcu_read_lock();
	cfg = rcu_dereference(g_config);
	if (cfg)
		pr_info("reader: ver=%d a=%d b=%d c=%d\n",
			cfg->version, cfg->a, cfg->b, cfg->c);
	rcu_read_unlock();
}
```

### 实测输出

```
reader: ver=0 a=0 b=0 c=0
update ver=1, grace period = 25287 us      ← 宽限期 25.3 ms
reader: ver=1 a=1 b=1 c=1                  ← 读者永远读到一致数据
update ver=2, grace period = 17044 us      ← 宽限期 17.0 ms
reader: ver=2 a=2 b=2 c=2
```

**宽限期在 17~25ms 之间波动**——这就是 RCU 的代价。

## 六、宽限期为什么这么长（PREEMPT_RT + QEMU）

boot log 线索：

```
rcu: RCU priority boosting: priority 1 delay 500 ms
rcu: RCU_SOFTIRQ processing moved to rcuc kthreads
NO_HZ: Full dynticks CPUs: 3
rcu: Offload RCU callbacks from CPUs: 3
```

宽限期要等**所有 CPU 报告 quiescent state**：
- CPU3 被 `nohz_full=3 isolcpus=3` 隔离，tick 停止，报告 QS 慢
- RCU 软中断移到 `rcuc` 线程，要等调度
- QEMU 虚拟化额外开销

**实时系统启示**：`synchronize_rcu()` 的十几毫秒延迟是实时任务的杀手，要么用 `call_rcu` 异步释放，要么重构设计。

## 七、RCU 的约束与代价

| 侧 | 约束/代价 |
|---|---|
| 读者 | 不能睡眠、不能阻塞、临界区要短 |
| 写者 | 要复制（内存开销）+ `synchronize_rcu` 等十几 ms |
| 内存 | 宽限期内旧版本一直占着 |

**只适合**：读极多、写极少、读者临界区极短。

## 八、清理顺序（exit 里必须这样）

```c
struct config *cfg = rcu_dereference_protected(g_config, 1);
if (cfg) {
	RCU_INIT_POINTER(g_config, NULL);   /* ① 先摘指针 */
	synchronize_rcu();                   /* ② 等老读者退出 */
	kfree(cfg);                          /* ③ 再释放 */
}
```

顺序错了就是 use-after-free。

## 九、要点回顾

```
RCU        ──不改正在被读的对象，改副本再替换指针──>  读者零开销
宽限期     ──等所有旧读者退出──>  synchronize_rcu（实测 17~25ms）
内存屏障   ──rcu_assign_pointer / rcu_dereference 内含──>  发布/获取语义
kzalloc    ──kmalloc + __GFP_ZERO──>  全局初始配置用它
kmalloc    ──不初始化──>  马上 *new = *old 全覆盖时用它
RCU 代价   ──写侧延迟十几 ms + 旧版本内存占用──>  只适合读多写少
```
