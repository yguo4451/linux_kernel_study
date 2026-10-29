/*
 * dl_demo.c - SCHED_DEADLINE 调度实验程序
 *
 * 用 sched_setattr 系统调用把进程设为 SCHED_DEADLINE，
 * 然后模拟一个"周期任务"：每个周期 busy-loop 模拟计算，
 * clock_nanosleep 精确睡到下一个周期。
 *
 * 用法：
 *   dl_demo [runtime_us] [deadline_us] [period_us] [loops]
 *   默认：runtime=5000us deadline=10000us period=10000us loops=1000
 *
 * 约束：runtime <= deadline <= period
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/syscall.h>

#define SCHED_DEADLINE 6

/*
 * 自定义 struct sched_attr（与内核 include/uapi/linux/sched/types.h 布局一致）。
 * glibc 2.35 未封装 sched_setattr，故手动声明并用 syscall 调用。
 */
struct sched_attr {
	unsigned int size;
	unsigned int sched_policy;
	unsigned long long sched_flags;
	int sched_nice;
	unsigned int sched_priority;
	unsigned long long sched_runtime;
	unsigned long long sched_deadline;
	unsigned long long sched_period;
};

static long long diff_ns(struct timespec a, struct timespec b)
{
	return (long long)(a.tv_sec - b.tv_sec) * 1000000000LL
		+ (a.tv_nsec - b.tv_nsec);
}

int main(int argc, char **argv)
{
	unsigned long long runtime  = 5000;   /* 每周期最多运行 5ms   */
	unsigned long long deadline = 10000;  /* 相对截止时间 10ms     */
	unsigned long long period   = 10000;  /* 周期 10ms             */
	int loops = 1000;
	struct sched_attr attr;
	struct timespec next, start, now;
	int i;

	if (argc >= 4) {
		runtime  = (unsigned long long)atoi(argv[1]);
		deadline = (unsigned long long)atoi(argv[2]);
		period   = (unsigned long long)atoi(argv[3]);
	}
	if (argc >= 5)
		loops = atoi(argv[4]);

	if (runtime > deadline || deadline > period) {
		fprintf(stderr, "约束不满足: runtime <= deadline <= period\n");
		return 1;
	}

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.sched_policy = SCHED_DEADLINE;
	attr.sched_runtime = runtime * 1000;   /* us -> ns */
	attr.sched_deadline = deadline * 1000;
	attr.sched_period = period * 1000;

	if (syscall(SYS_sched_setattr, 0, &attr, 0) < 0) {
		perror("sched_setattr(SCHED_DEADLINE)");
		fprintf(stderr, "提示: 需 root 权限，且总带宽(runtime/period)不能超系统上限\n");
		return 1;
	}

	printf("pid=%d 已设为 SCHED_DEADLINE: runtime=%lluus deadline=%lluus period=%lluus (带宽 %.0f%%)\n",
	       getpid(), runtime, deadline, period,
	       100.0 * runtime / period);

	clock_gettime(CLOCK_MONOTONIC, &next);

	for (i = 0; i < loops; i++) {
		/* 模拟计算：busy-loop 约 runtime 的一半时间 */
		clock_gettime(CLOCK_MONOTONIC, &start);
		do {
			clock_gettime(CLOCK_MONOTONIC, &now);
		} while (diff_ns(now, start) < (long long)(runtime / 2) * 1000);

		/* 精确睡到下一个周期（绝对时间） */
		next.tv_nsec += (long)period * 1000;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_sec += 1;
			next.tv_nsec -= 1000000000L;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		if (i % 100 == 0)
			printf("循环 %d 完成\n", i);
	}

	printf("全部 %d 个周期完成，进程退出\n", loops);
	return 0;
}
