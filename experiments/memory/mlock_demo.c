/*
 * mlock_demo.c - 演示 mlock 锁页，观察 VmLck 与 Unevictable
 *
 * 用法:
 *   mlock_demo [MB] [mode]
 *     MB   : malloc 大小（默认 64）
 *     mode : 0 = 只 touch 不锁（对照组）
 *            1 = mlock 指定范围（默认）
 *            2 = mlockall(MCL_CURRENT | MCL_FUTURE)（实时任务标准做法）
 *            3 = 先 mlockall(MCL_FUTURE)，再 malloc 新内存（验证后续分配也被锁）
 *            4 = mlockall(FUTURE) + malloc + free（验证 free 释放 mlocked 页）
 *
 * 教学点:
 *   mlock 不只是"打标记"：它先把页物理驻留（__mm_populate），再标记为
 *   unevictable，从此 kswapd 不回收、swap 不换出，访问不再缺页。
 *   对比 mode=0 与 mode=1/2 时 /proc/PID/status 的 VmLck，
 *   以及 /proc/meminfo 的 Unevictable，就是锁页的直观证据。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

static void print_vmlck(const char *label)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "VmLck:", 6) == 0) {
			printf("[%s] %s", label, line);
			break;
		}
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	long mb = (argc > 1) ? atol(argv[1]) : 64;
	int mode = (argc > 2) ? atoi(argv[2]) : 1;
	size_t size = (size_t)mb * 1024 * 1024;
	char *p = malloc(size);

	if (!p) {
		perror("malloc");
		return 1;
	}

	memset(p, 0x5a, size);   /* touch：先触发缺页，让页物理驻留 */

	switch (mode) {
	case 0:
		printf("pid=%d mode=0 touch-only %ldMB (no mlock)\n",
		       getpid(), mb);
		break;
	case 1:
		if (mlock(p, size) != 0) {
			perror("mlock");
			return 1;
		}
		printf("pid=%d mode=1 mlock %ldMB\n", getpid(), mb);
		break;
	case 2:
		if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
			perror("mlockall");
			return 1;
		}
		printf("pid=%d mode=2 mlockall(MCL_CURRENT|MCL_FUTURE)\n",
		       getpid());
		break;
	case 3:
		/* 先 mlockall(MCL_FUTURE)，再 malloc，验证后续分配也被锁 */
		if (mlockall(MCL_FUTURE) != 0) {
			perror("mlockall");
			return 1;
		}
		p = malloc(size);
		if (!p) {
			perror("malloc-after-mlockall");
			return 1;
		}
		memset(p, 0x5a, size);   /* touch 新分配的内存 */
		printf("pid=%d mode=3 mlockall(FUTURE) then malloc+touch %ldMB\n",
		       getpid(), mb);
		print_vmlck("after-malloc");
		break;
	case 4:
		/* mlockall(FUTURE) + malloc + free，验证 free 释放 mlocked 页 */
		if (mlockall(MCL_FUTURE) != 0) {
			perror("mlockall");
			return 1;
		}
		p = malloc(size);
		if (!p) {
			perror("malloc-after-mlockall");
			return 1;
		}
		memset(p, 0x5a, size);   /* touch，新分配被锁 */
		print_vmlck("locked ");
		free(p);                 /* 释放！锁随 VMA 销毁解除 */
		print_vmlck("freed  ");
		break;
	default:
		fprintf(stderr, "bad mode %d\n", mode);
		return 1;
	}

	fflush(stdout);
	pause();   /* 驻留，等待观察/被 kill */
	return 0;
}
