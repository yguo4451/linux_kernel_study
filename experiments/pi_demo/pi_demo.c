/*
 * pi_demo.c —— 优先级反转实测：普通 mutex vs PI mutex
 *
 * 场景（三个线程都 pin 在 CPU0，单核竞争）：
 *   L（低优先级 30）：持锁，慢速临界区 50ms
 *   M（中优先级 50）：忙等 300ms，抢占 L
 *   H（高优先级 80）：尝试获取同一把锁
 *
 * 顺序至关重要：H 必须先阻塞在锁上，M 再忙等抢占 L（教科书标准顺序）
 * 无 PI：L 被 M 抢占 → 锁无法释放 → H 被 M 间接阻塞 ≈ 300ms
 * 有 PI：H 等锁 → L 继承 H 的优先级 → L 不被 M 抢占 → 快速释放 ≈ 50ms
 *
 * 注：M 忙等取 300ms，是为避开 RT throttling（默认预算 950ms/1s，
 *     两轮各忙等一次 2×300=600ms < 950ms，不会触发节流）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <sys/syscall.h>

static pthread_mutex_t lock;
static long h_latency_ms = -1;
static int hold_ms = 50;   /* L 持锁时长（HOLD_MS 环境变量可调）*/
static int mid_ms  = 300;  /* M 忙等时长（MID_MS 环境变量可调）*/
static int prep_ms = 10;   /* L 拿锁到 H 抢锁的准备时间（PREP_MS 可调，须 < HOLD_MS）*/

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* 设置当前线程为 SCHED_FIFO 实时优先级 */
static void set_rt_prio(int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        perror("pthread_setschedparam");
        exit(1);
    }
}

/* 高优先级线程：抢锁并计时 */
static void *high_task(void *arg)
{
    (void)arg;
    set_rt_prio(80);
    long t0 = now_ms();
    pthread_mutex_lock(&lock);   /* 在这里等锁 */
    long t1 = now_ms();
    h_latency_ms = t1 - t0;
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* 低优先级线程：持锁 + 慢速临界区 */
static void *low_task(void *arg)
{
    (void)arg;
    set_rt_prio(30);
    pthread_mutex_lock(&lock);
    printf("[L] tid=%ld 已持锁 %d ms（另开终端观察 /proc/%ld/stat 第18字段）\n",
           (long)syscall(SYS_gettid), hold_ms, (long)syscall(SYS_gettid));
    fflush(stdout);
    usleep(hold_ms * 1000L);
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* 中优先级线程：忙等 300ms，抢占低优先级线程 */
static void *mid_task(void *arg)
{
    (void)arg;
    set_rt_prio(50);
    long t0 = now_ms();
    volatile unsigned long x = 0;
    while (now_ms() - t0 < mid_ms)
        x++;                     /* 忙等 mid_ms */
    (void)x;
    return NULL;
}

/* 跑一轮，返回高优先级任务的等待延迟 */
static long run_round(void)
{
    pthread_t tl, tm, th;
    h_latency_ms = -1;

    /* 顺序至关重要：H 必须先阻塞在锁上，M 再忙等抢占 L。
     * 若先创建 M，主线程(SCHED_NORMAL)会被 M 饿死，H 创建太晚导致测到 0ms */
    pthread_create(&tl, NULL, low_task, NULL);   /* ① L 先拿锁 */
    usleep(prep_ms * 1000L);                      /* 等 L 拿锁 + 给观测者留时间（PREP_MS）*/
    pthread_create(&th, NULL, high_task, NULL);  /* ② H 抢锁 → 阻塞（H 睡眠不占 CPU）*/
    usleep(10000);                                /* 等 H 阻塞在锁上 */
    pthread_create(&tm, NULL, mid_task, NULL);   /* ③ M 忙等 → 抢占 L，L 无法释放锁 */

    pthread_join(th, NULL);
    pthread_join(tm, NULL);
    pthread_join(tl, NULL);
    return h_latency_ms;
}

int main(void)
{
    const char *e;
    int skip_no_pi = 0;
    long lat1 = -1, lat2 = -1;
    if ((e = getenv("HOLD_MS"))    && atoi(e) > 0) hold_ms = atoi(e);
    if ((e = getenv("MID_MS"))     && atoi(e) > 0) mid_ms  = atoi(e);
    if ((e = getenv("PREP_MS"))    && atoi(e) > 0) prep_ms = atoi(e);
    if ((e = getenv("SKIP_NO_PI")) && atoi(e) > 0) skip_no_pi = 1;
    printf("HOLD_MS=%d MID_MS=%d PREP_MS=%d SKIP_NO_PI=%d\n",
           hold_ms, mid_ms, prep_ms, skip_no_pi);

    /* 关键：把所有线程 pin 到 CPU0，制造真正的单核竞争 */
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        return 1;
    }

    /* 第一轮：普通 mutex（无 PI），SKIP_NO_PI=1 时跳过 */
    if (!skip_no_pi) {
        pthread_mutex_init(&lock, NULL);
        printf("=== 普通 mutex（无优先级继承）===\n");
        lat1 = run_round();
        printf("高优先级任务等待锁的延迟: %ld ms\n\n", lat1);
        pthread_mutex_destroy(&lock);
    }

    /* 第二轮：PI mutex（PTHREAD_PRIO_INHERIT）*/
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&lock, &attr);
    pthread_mutexattr_destroy(&attr);

    printf("=== PI mutex（优先级继承）===\n");
    lat2 = run_round();
    printf("高优先级任务等待锁的延迟: %ld ms\n\n", lat2);
    pthread_mutex_destroy(&lock);

    if (!skip_no_pi) {
        printf("=== 对比 ===\n");
        printf("无 PI: %ld ms  （被中优先级任务阻塞，≈ M 的忙等时间）\n", lat1);
        printf("有 PI: %ld ms  （≈ L 的临界区时间 50ms）\n", lat2);
        printf("差距: %ld ms\n", lat1 - lat2);
    }
    return 0;
}
