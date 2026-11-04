
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/completion.h>


static DEFINE_MUTEX(lock_a);
static DEFINE_MUTEX(lock_b);


static struct completion done;
static atomic_t finished = ATOMIC_INIT(0);


static int thread_ab(void *data) {
	pr_info("deadlock: [A] lock A\n");
	mutex_lock(&lock_a);

	msleep(200); 

	pr_info("deadlock: [A] try lock B\n");
	if (!mutex_trylock(&lock_b)) {
		pr_info("deadlock: [A] trylock B FAILED -> would deadlock!\n");
		mutex_unlock(&lock_a);
		goto out;
	}

	mutex_unlock(&lock_b);
	mutex_unlock(&lock_a);
out:
	if (atomic_inc_return(&finished) == 2) {
		complete(&done);
	}

	return 0;
}

static int thread_ba(void* data) {
	pr_info("deadlock: [B] lock B\n");

	mutex_lock(&lock_b);
	msleep(200);

	pr_info("deadlock: [B] try lock A\n");
	if (!mutex_trylock(&lock_a)) {
		pr_info("deadlock: [B] trylock A FAILED -> would deadlock\n");
		mutex_unlock(&lock_b);
		goto out;
	}
	mutex_unlock(&lock_a);
	mutex_unlock(&lock_b);
out:
	if (atomic_inc_return(&finished) == 2) {
		complete(&done);
	}

	return 0;
}


static int __init hello_deadlock_init(void) {
	init_completion(&done);
	atomic_set(&finished, 0);

	kthread_run(thread_ab, NULL, "dl_A");
	kthread_run(thread_ba, NULL, "dl_B");

	wait_for_completion(&done);
	pr_info("deadlock: done (no real deadlock)\n");

	return 0;
}

static void __exit hello_deadlock_exit(void) {
	pr_info("deadlock: bye\n");
}

module_init(hello_deadlock_init);
module_exit(hello_deadlock_exit);

MODULE_LICENSE("GPL");