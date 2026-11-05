

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/ktime.h>



struct config {
	int version;
	int a, b, c;
};


static struct config __rcu* g_config;


static void update_config(int ver) {
	struct config* old = rcu_dereference_protected(g_config, 1);

	struct config* new = kmalloc(sizeof(*new), GFP_KERNEL);
	ktime_t t0, t1;
	if (!new) {
		return;
	}

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

static void read_config(void) {
	struct config* cfg;

	rcu_read_lock();
	cfg = rcu_dereference(g_config);
	if (cfg) {
		pr_info("reader: ver=%d a=%d b=%d c=%d\n",
            cfg->version, cfg->a, cfg->b, cfg->c);
	}
	rcu_read_unlock();
}


static int __init hello_rcu_init(void) {
	g_config = kzalloc(sizeof(struct config), GFP_KERNEL);
	if (!g_config) {
		return -ENOMEM;
	}

	g_config->a = g_config->b = g_config->c = 0;

	pr_info("====== start ======\n");

	read_config();
	update_config(1);
	read_config();
	update_config(2);
	read_config();

	pr_info("=== done ===\n");

	return 0;
}

static void __exit hello_rcu_exit(void) {
	struct config *cfg = rcu_dereference_protected(g_config, 1);

    if (cfg) {
        RCU_INIT_POINTER(g_config, NULL);   /* 先摘指针 */
        synchronize_rcu();                  /* 等读者退出 */
        kfree(cfg);                         /* 再释放 */
    }
	pr_info("deadlock: bye\n");
}

module_init(hello_rcu_init);
module_exit(hello_rcu_exit);

MODULE_LICENSE("GPL");