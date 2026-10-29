# PID 与线程模型：/proc 到底展示进程还是线程

## 一、核心结论

Linux 内核管理和调度的单元统一是 `task_struct`（线程），`/proc/` 下每个数字目录对应**一个线程**。优先级、锁、调度全部挂在 task 上，"进程"只是共享同一 `mm`/`tgid` 的一组线程（线程组）。

## 二、三重 ID

| 名称 | 含义 | 用户态获取 |
|------|------|-----------|
| 内核 PID | 线程 ID（每个 task 一个，全局唯一）| `gettid()` / `syscall(SYS_gettid)` |
| TGID | 线程组 ID（传统意义的"进程号"）| `getpid()` |
| 主线程 tid | 恰好等于 tgid | —— |

`/proc/` 目录名用的是**内核 PID（= TID）**，不是 TGID。主线程的 tid 恰好等于 tgid，才让人误以为 `/proc/<pid>` 是"进程目录"。

## 三、/proc 文件的两类语义

`/proc/<tid>/` 目录对应一个线程，但目录下文件分两类：

| 文件 | 语义 | 原因 |
|------|------|------|
| `stat`、`sched`、`io`、`syscall` | **线程级** | 读单个 task_struct |
| `fd`、`maps`、`exe`、`cwd`、`root` | **进程级（线程组共享）** | 读 `task->files` / `task->mm` |

验证：多线程进程里，各线程 `/proc/<tid>/fd` 相同（共享 fd 表），但 `/proc/<tid>/stat` 的 prio/state/utime 不同（线程级）。

源码：`fs/proc/base.c` 的 `get_proc_task()` 用 `get_pid_task(pid, PIDTYPE_PID)` 定位到线程；`fs/proc/array.c` 的 `do_task_stat(struct task_struct *task)` 参数就是线程。

## 四、PID 分配机制（kernel/pid.c）

```c
// alloc_pidmap() 简化：last_pid 游标向后找第一个空闲位
pid = last_pid + 1;
while (test_and_set_bit(pid)) pid++;
set_last_pid(pid);
```

三个性质：

1. **全局唯一**：同一时刻一个 PID 只对应一个 task
2. **单调递增 + 复用**：游标只前进，到 pid_max 回绕；task 退出后 PID 释放可复用
3. **并发交错**：进程和线程从同一池子取号，谁先 clone 谁先拿

实测现象：连续两次后台运行 `pi_demo`（PREP_MS=3000 制造窗口），第一次 L=102，第二次 L=104——因为第一次还没创建 H/M，103/104 空闲，被第二次的 main/L 抢先分配。两次运行的 PID 交错，但同一时刻仍唯一，**不冲突**。

## 五、跨操作系统对比

| 系统 | 进程/线程模型 | 查看方式 |
|------|--------------|---------|
| Linux | task_struct 统一，内核 PID = 线程 ID | `/proc` |
| Windows | EPROCESS/ETHREAD 严格分离，PID/TID 两个命名空间 | tasklist、任务管理器 |
| macOS | Mach thread + BSD pid 两层 | `ps -M`（默认无 /proc）|
| FreeBSD | 有 procfs 但结构与字段差异大 | `/proc` 内容不同 |

`/proc` + "PID=线程 ID" 是 Linux 特有设计，所有 Linux 变体（Ubuntu/Android/嵌入式/车载）一致，但不是通用 OS 规律。
