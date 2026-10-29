/*
 * watch_prio.c —— 连续采样指定线程的动态优先级
 *
 * 数据源：/proc/<tid>/stat 第 18 字段（prio，动态优先级）
 *   - tid 是内核 PID（= gettid()）；/proc/ 下每个数字目录就是一个线程(task)
 *   - 该字段 = task->prio - MAX_RT_PRIO(100)
 *   - 实时任务：99 - rt_priority - 100 = -(rt_priority + 1)
 *   - 例：FIFO 30 → -31，FIFO 80 → -81
 *   - 优先级继承(boost)会直接反映在这个字段上
 *
 * 用法: watch_prio <tid> [间隔ms，默认100]
 * 示例: watch_prio 103 100
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* 从 /proc/PID/stat 一行里解析第 18 字段（prio）
 * stat 格式: pid (comm) state ppid pgrp ... prio nice ...
 * comm 可能含空格，所以先用 strrchr 定位最后一个 ')'，其后的第 16 个字段即 prio */
static long get_prio(const char *line)
{
    char *p = strrchr(line, ')');
    if (!p) return -999;
    p += 2;                          /* 跳过 ") "，现在指向 state（第 3 字段）*/
    for (int i = 1; i < 16; i++) {   /* prio 是 state 起第 16 个字段 */
        p = strchr(p, ' ');
        if (!p) return -999;
        p++;
    }
    return atol(p);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: watch_prio <tid> [间隔ms]\n");
        return 1;
    }
    long tid = atol(argv[1]);
    int interval = argc > 2 ? atoi(argv[2]) : 100;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", tid);

    printf("采样线程 %ld 的动态优先级（第18字段），间隔 %d ms，Ctrl+C 停止\n",
           tid, interval);
    fflush(stdout);

    while (1) {
        FILE *f = fopen(path, "r");
        if (!f) {
            printf("线程 %ld 已退出，采样结束\n", tid);
            return 0;
        }
        char line[4096];
        if (fgets(line, sizeof(line), f)) {
            printf("prio=%ld\n", get_prio(line));
            fflush(stdout);
        }
        fclose(f);
        usleep(interval * 1000);
    }
    return 0;
}
