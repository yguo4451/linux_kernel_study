# 30. poll/epoll：多设备事件监控

> 阶段四第十课。实现 `.poll` 回调，让设备支持 `poll()`/`select()`/`epoll()`：一个线程同时监控多个设备，哪个可读就处理哪个。这是阻塞 IO 的升级——从"一个线程等一个设备"到"一个线程等 N 个设备"。

## 一、为什么需要 poll

阻塞 read 的局限：**一个线程只能阻塞等一个设备**。

```c
cat /dev/hello_dev0   // 进程卡在 read 上，无法同时等 hello_dev1
```

真实系统一个进程要盯几十个设备（传感器、串口、socket）。poll/epoll 解决：**一个线程监控 N 个设备，哪个有数据处理哪个**。

## 二、驱动的 `.poll` 回调

```c
#include <linux/poll.h>   // poll_wait + poll_table + __poll_t + EPOLLIN

static __poll_t hello_poll(struct file *file, struct poll_table_struct *wait) {
    struct hello_dev *dev = file->private_data;
    __poll_t mask = 0;

    poll_wait(file, &dev->wq, wait);   // ① 把 poll 进程挂到等待队列

    if (dev->has_data)
        mask |= EPOLLIN | EPOLLRDNORM;  // ② 有数据可读 → 返回 POLLIN

    return mask;
}

// fops 里：
.poll = hello_poll,
```

两个关键点：

1. **`poll_wait(file, &dev->wq, wait)`**：把 poll 的进程挂到 `dev->wq` 上。这样 `write` 里的 `wake_up_interruptible(&dev->wq)` 就能唤醒 poll 的进程——**复用阻塞 IO 的等待队列**。
2. **返回 mask**：告诉用户态"现在可读吗"（`EPOLLIN`）、"可写吗"（`EPOLLOUT`）。

## 三、用户态测试程序

```c
#include <stdio.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

int main(void) {
    int fd0 = open("/dev/hello_dev0", O_RDWR);
    int fd1 = open("/dev/hello_dev1", O_RDWR);

    struct pollfd fds[2] = {
        { fd0, POLLIN, 0 },
        { fd1, POLLIN, 0 },
    };

    printf("polling 2 devices, waiting...\n");
    int ret = poll(fds, 2, -1);      // 无限等
    if (ret < 0) { perror("poll"); return 1; }

    for (int i = 0; i < 2; i++) {
        if (fds[i].revents & POLLIN) {
            char buf[128] = {0};
            read(fds[i].fd, buf, sizeof(buf));
            printf("device %d readable: %s\n", i, buf);
        }
    }
    return 0;
}
```

## 四、完整数据流

```
用户态 poll(fds, 2, -1)
  → 内核遍历 2 个 fd，分别调 hello_poll
      ├─ poll_wait 把进程挂到 dev0.wq 和 dev1.wq
      └─ 都返回 0（无数据）→ 进程睡眠

终端 B: echo "hi" > /dev/hello_dev1
  → hello_write: has_data=1 + wake_up_interruptible(&dev1.wq)
  → 唤醒 poll 进程

poll 返回 → 遍历 revents 发现 fd1 可读 → read 读出 "hi"
```

## 五、关键理解

1. **`.poll` 是设备"参与事件循环"的入口**——epoll/select 都建立在它之上。
2. **`poll_wait` 不是阻塞**：它只是把进程挂到队列上，真正的睡眠由 poll 的核心机制完成。`.poll` 被调用时只负责"报告当前状态 + 登记等待队列"。
3. **`EPOLLIN` 与 `POLLIN`**：内核用 `EPOLLIN`（mask 位），用户态 `struct pollfd` 用 `POLLIN`，数值上对应。
4. **epoll 是 poll 的升级**：`poll()` 每次要遍历所有 fd，O(n)；`epoll()` 用红黑树 + 就绪队列，O(1) 唤醒。但设备侧的 `.poll` 回调完全一样。

## 六、与阻塞 IO 的关系

```
阻塞 IO：read 直接 wait_event 睡眠（单设备）
poll：.poll 回调 poll_wait 登记 + 返回 mask（多设备，由 poll 核心统一睡眠）
```

两者共用同一个 `wait_queue_head_t` 和 `wake_up_interruptible`——这就是为什么 poll 能和 write 的唤醒完美衔接。

## 七、踩坑

1. **`poll_wait` 漏调**：没调 poll_wait，write 的 wake_up 就唤不醒 poll 进程，poll 会永远阻塞。
2. **返回 0 无数据**：`.poll` 返回 0 表示"当前没数据，但已登记等待"，不是错误。返回负值才是错误。
3. **mask 位**：可读用 `EPOLLIN | EPOLLRDNORM`，可写用 `EPOLLOUT | EPOLLWRNORM`。
