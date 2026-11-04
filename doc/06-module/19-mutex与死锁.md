# 39. mutex 与死锁：ABBA 场景与安全探测

> 阶段五第十九课。理解 mutex（能睡眠的锁）与 spinlock 的本质区别、选择标准；构造经典的 ABBA 死锁格局，用 `mutex_trylock` **安全探测**死锁而不真卡死。这是多核并发的核心一课。

## 一、mutex：能睡眠的锁

```c
#include <linux/mutex.h>

static DEFINE_MUTEX(my_mutex);

mutex_lock(&my_mutex);
    msleep(10);                     /* ✅ 可以睡眠 */
    copy_to_user(...);              /* ✅ 可以 */
    kmalloc(..., GFP_KERNEL);       /* ✅ 可以 */
mutex_unlock(&my_mutex);
```

抢不到锁时 **mutex 让出 CPU 睡眠**，而不是原地空转。

## 二、mutex vs spinlock

| 维度 | spinlock | mutex |
|---|---|---|
| 抢不到锁时 | **自旋**（空转占 CPU） | **睡眠**（让出 CPU） |
| 临界区长度 | 极短（几条指令） | 可以长 |
| 能睡眠吗 | ❌ | ✅ |
| 中断上下文 | ✅ 可用 | ❌ 不可用 |
| 实现 | 原子指令 + 自旋 | 原子操作 + 等待队列 |

### 为什么 spinlock 临界区不能睡眠

```
CPU0: spin_lock(L)
CPU0: msleep()          ← 睡了
      此时本 CPU 中断来了，中断也想 spin_lock(L)
      → 持锁者睡着不释放 → 永远等不到 → 死锁
```

在 spinlock 临界区里睡眠，内核会直接报 `BUG: scheduling while atomic`。

## 三、选择标准（决策树）

```
能在中断/软中断上下文运行吗？
  ├─ 能 → spinlock（中断里不能睡）
  └─ 不能（进程上下文）→
        临界区会睡眠吗（copy_to_user / msleep / GFP_KERNEL）？
          ├─ 会 → mutex
          └─ 不会 → 临界区极短？是→spinlock（更快）；否→mutex
```

## 四、死锁：ABBA 经典场景

```
CPU0 (线程 A)                CPU1 (线程 B)
mutex_lock(&A)  ✅            mutex_lock(&B)  ✅
mutex_lock(&B)  ⏳ 等 B       mutex_lock(&A)  ⏳ 等 A
        └────────── 互相等待，永久死锁 ──────────┘
```

### 死锁四条件（Coffman）

| 条件 | ABBA 中的体现 |
|---|---|
| 互斥 | mutex 独占 |
| 持有并等待 | A 持 A 等 B，B 持 B 等 A |
| 不可剥夺 | 锁不能被强抢 |
| 循环等待 | A→B 与 B→A 成环 |

**打破任一条件即可**。最实用的是打破"循环等待"——**统一加锁顺序**（所有代码都按 A→B 拿锁）。

## 五、lockdep：内核的死锁检测器

内核有 **lockdep**，运行时记录"谁拿了谁"，发现潜在循环依赖就报警：

```
WARNING: possible circular locking dependency detected
```

但本内核 **没开**（`CONFIG_PROVE_LOCKING is not set`）——lockdep 开销大（每次锁操作记录调用栈），只在调试内核开，生产内核默认关闭。所以死锁实验要**安全地演示**。

## 六、实验：用 `mutex_trylock` 安全探测

```c
static DEFINE_MUTEX(lock_a);
static DEFINE_MUTEX(lock_b);

/* 线程 1：A → B */
static int thread_ab(void *data)
{
	mutex_lock(&lock_a);
	msleep(200);                        /* 制造窗口：让线程 2 拿到 B */

	if (!mutex_trylock(&lock_b)) {      /* 探测：拿不到就说明会死锁 */
		pr_info("trylock B FAILED -> would deadlock\n");
		mutex_unlock(&lock_a);
		return 0;                        /* 安全退出，不死锁 */
	}
	mutex_unlock(&lock_b);
	mutex_unlock(&lock_a);
	return 0;
}

/* 线程 2：B → A（相反顺序） */
static int thread_ba(void *data)
{
	mutex_lock(&lock_b);
	msleep(200);

	if (!mutex_trylock(&lock_a)) {
		pr_info("trylock A FAILED -> would deadlock\n");
		mutex_unlock(&lock_b);
		return 0;
	}
	mutex_unlock(&lock_a);
	mutex_unlock(&lock_b);
	return 0;
}
```

`mutex_trylock` 抢不到立即返回 false（不睡眠），用它探测死锁条件，探测到就释放退出。

### 实测输出

```
deadlock: [A] lock A
deadlock: [B] lock B                                    ← ABBA 格局形成
deadlock: [B] try lock A
deadlock: [B] trylock A FAILED -> would deadlock        ← 探测到！
deadlock: [A] try lock B
deadlock: done (no real deadlock)
```

**关键证据**：`trylock A FAILED`。如果那行是 `mutex_lock(&lock_a)`，线程 B 就永久卡在这里——这就是 ABBA 死锁。

只有"先动手 trylock 的那个线程"会失败，因为它失败后释放了自己的锁，格局被打破。这也体现了并发时序的不确定性。

## 七、思考题：spinlock + msleep 会怎样

如果把 mutex 换成 spinlock，在临界区里 `msleep(200)`：

```
BUG: scheduling while atomic
```

因为 spinlock 持有期间 `preempt_count` 增加、禁止抢占，持锁者睡眠会导致其他 CPU 无限自旋、本 CPU 中断也等不到。**这就是 mutex 存在的意义**：临界区要睡眠就用 mutex。

## 八、要点回顾

```
mutex      ──睡眠等待──>  可长临界区、可睡眠、进程上下文
spinlock   ──自旋等待──>  极短临界区、不能睡眠、中断可用
选择       ──中断/是否睡眠/临界区长短──>  决策树
死锁四条件 ──互斥/持有等待/不可剥夺/循环等待──>  打破任一即可
ABBA       ──相反顺序拿两把锁──>  循环等待
lockdep    ──运行时死锁检测──>  CONFIG_PROVE_LOCKING（本内核未开）
trylock    ──不等待探测──>  安全演示死锁而不卡死
```
