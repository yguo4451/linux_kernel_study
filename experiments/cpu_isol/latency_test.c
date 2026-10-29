/*
 * latency_test.c —— 实时任务延迟抖动测试（cyclictest 精简版）
 *
 * 原理：SCHED_FIFO 实时任务周期性睡 1ms，醒来后测量
 *       "实际醒来时间 - 预期醒来时间" 的偏差，统计 min/avg/max。
 *       max 就是最坏情况延迟（实时系统最关心的指标）。
 *
 * 用法: latency_test <cpu> [prio=80] [loops=5000]
 * 示例: latency_test 3 80 5000
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: latency_test <cpu> [prio=80] [loops=5000]\n");
        return 1;
    }
    int cpu   = atoi(argv[1]);
    int prio  = argc > 2 ? atoi(argv[2]) : 80;
    int loops = argc > 3 ? atoi(argv[3]) : 5000;
    const long period_ns = 1000000L;   /* 周期 1ms */

    /* 1. pin 到指定 CPU */
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        return 1;
    }

    /* 2. 设为 SCHED_FIFO 实时任务 */
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        perror("sched_setscheduler");
        return 1;
    }

    /* 3. 周期性 sleep + 测延迟 */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long max_lat = 0, min_lat = LONG_MAX, sum_lat = 0;

    for (int i = 0; i < loops; i++) {
        ts.tv_nsec += period_ns;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_nsec -= 1000000000L;
            ts.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long lat = (now.tv_sec - ts.tv_sec) * 1000000000L
                 + (now.tv_nsec - ts.tv_nsec);
        if (lat > max_lat) max_lat = lat;
        if (lat < min_lat) min_lat = lat;
        sum_lat += lat;
    }

    printf("CPU%d  prio=%d  loops=%d  周期=1ms\n", cpu, prio, loops);
    printf("  最小延迟: %5ld us\n", min_lat / 1000);
    printf("  平均延迟: %5ld us\n", sum_lat / loops / 1000);
    printf("  最大延迟: %5ld us   ← 实时系统最关心的最坏情况\n", max_lat / 1000);
    return 0;
}
