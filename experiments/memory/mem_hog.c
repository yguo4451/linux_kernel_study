/*
 * mem_hog.c - 分配内存并驻留，用于观察 Buddy System / 缺页异常
 *
 * 用法:
 *   mem_hog [MB] [touch] [retouch]
 *     MB      : malloc 的大小（默认 100）
 *     touch   : 1 = 逐页 touch（触发缺页，真正分配物理页，默认）
 *               0 = 只 malloc 不 touch（只占虚拟地址，不占物理页）
 *     retouch : 秒数，睡这么久后重新读一遍所有页（观察 swap-in 换入）
 *               0 = 不重访（默认，pause 永久驻留）
 *
 * 教学点:
 *   malloc 只分配虚拟地址空间（VMA），不分配物理页；
 *   只有 touch 到某页，CPU 才触发缺页异常，内核才从 Buddy 分配物理页。
 *   对比 touch=0 和 touch=1 时 /proc/buddyinfo 的变化，就是缺页异常的直观证据。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

int main(int argc, char **argv)
{
	long mb = (argc > 1) ? atol(argv[1]) : 100;
	int touch = (argc > 2) ? atoi(argv[2]) : 1;
	int retouch = (argc > 3) ? atoi(argv[3]) : 0;
	size_t size = (size_t)mb * 1024 * 1024;
	char *p = malloc(size);

	if (!p) {
		perror("malloc");
		return 1;
	}

	printf("pid=%d malloc=%ldMB vaddr=%p touch=%d\n",
	       getpid(), mb, (void *)p, touch);

	if (touch) {
		size_t i;
		for (i = 0; i < size; i += 4096)
			p[i] = (char)i;        /* 写每一页 → 触发缺页分配物理页 */
		printf("touched all pages\n");
	}

	if (retouch > 0) {
		struct timespec t0, t1;
		volatile char sink = 0;
		size_t i;

		printf("sleep %ds then re-read all pages (swap-in)...\n", retouch);
		sleep(retouch);

		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (i = 0; i < size; i += 4096)
			sink += p[i];          /* 读每一页 → 被换出的页触发主缺页换回 */
		clock_gettime(CLOCK_MONOTONIC, &t1);

		double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
			    (t1.tv_nsec - t0.tv_nsec) / 1e6;
		printf("re-read done: %ldMB in %.2f ms (sink=%d)\n",
		       mb, ms, (int)sink);
	}

	fflush(stdout);
	pause();                         /* 驻留，等待被 kill */
	return 0;
}
