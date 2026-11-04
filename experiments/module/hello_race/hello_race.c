/*
 * hello_race.c — 竞态条件演示：裸++ vs atomic_t vs spinlock
 *
 * 创建 NR_THREADS 个内核线程，各自对同一个共享变量执行 ITERATIONS 次自增。
 * 三种写法对比：
 *   shared_plain++          裸 ++（load/add/store），竞态 → 丢更新
 *   atomic_inc(&shared_atomic)  原子操作 → 正确
 *   spin_lock + shared_locked++  加锁 → 正确
 *
 * 预期：plain < 期望值，atomic/locked == 期望值
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/completion.h>

#define ITERATIONS	1000000
#define NR_THREADS	4

static int shared_plain;			/* 裸 ++，竞态 */
static atomic_t shared_atomic = ATOMIC_INIT(0);	/* 原子操作 */
static int shared_locked;			/* spinlock 保护 */
static DEFINE_SPINLOCK(my_lock);

static struct completion all_done;
static atomic_t finished = ATOMIC_INIT(0);

static int worker_fn(void *data)
{
	long id = (long)data;
	int i;

	for (i = 0; i < ITERATIONS; i++) {
		shared_plain++;			/* 竞态：load/add/store 三步非原子 */

		atomic_inc(&shared_atomic);	/* 原子：单条 LDXR/STXR 循环 */

		spin_lock(&my_lock);		/* 加锁：临界区串行 */
		shared_locked++;
		spin_unlock(&my_lock);
	}

	pr_info("hello_race: worker %ld done\n", id);

	/* 最后一个完成的 worker 唤醒主线程 */
	if (atomic_inc_return(&finished) == NR_THREADS)
		complete(&all_done);

	return 0;	/* 直接退出，task_struct 由内核回收 */
}

static int __init hello_race_init(void)
{
	int i;
	int expected = NR_THREADS * ITERATIONS;

	pr_info("hello_race: === start, expected = %d ===\n", expected);

	init_completion(&all_done);
	atomic_set(&finished, 0);

	for (i = 0; i < NR_THREADS; i++) {
		struct task_struct *t = kthread_run(worker_fn, (void *)(long)i,
						    "race_worker%d", i);
		if (IS_ERR(t)) {
			pr_err("hello_race: create thread %d failed\n", i);
			if (atomic_inc_return(&finished) == NR_THREADS)
				complete(&all_done);
		}
	}

	/* 等所有 worker 完成 */
	wait_for_completion(&all_done);

	pr_info("hello_race: plain  = %d  (lost %d)\n",
		shared_plain, expected - shared_plain);
	pr_info("hello_race: atomic = %d\n", atomic_read(&shared_atomic));
	pr_info("hello_race: locked = %d\n", shared_locked);
	pr_info("hello_race: === done ===\n");

	return 0;
}

static void __exit hello_race_exit(void)
{
	pr_info("hello_race: bye\n");
}

module_init(hello_race_init);
module_exit(hello_race_exit);
MODULE_LICENSE("GPL");
