---
description: "Generate structured source-reading notes for a Linux kernel subsystem or file (fs/, mm/, kernel/, net/, drivers/). Trace call chains, explain key data structures, and link to real source paths."
name: "内核源码精读"
argument-hint: "输入子系统或文件路径，例如：fs/open.c 的 open 系统调用、kernel/sched/fair.c 的 CFS"
agent: "Linux 内核专家教练"
---
精读以下 Linux 内核源码，输出结构化笔记。

输入：
- 子系统 / 文件路径：
- 关注点（可选）：

## 输出要求

1. **核心数据结构**：列出涉及的关键结构体定义（头文件路径 + 关键字段含义）
2. **调用链追踪**：从入口（如系统调用）到实现的完整调用链，标注 `文件:行号`
3. **设计意图**：解释为什么这样设计（why，而非仅 what）
4. **关键机制**：锁、引用计数、缓存、并发控制等
5. **实验验证**：给出 1-2 个可在 QEMU 中观察该机制的实验（如 `/proc`、`strace`、`ftrace`）
6. **延伸阅读**：相关源码文件与下一步深入方向

要求：每个结论落到真实源码依据，不臆测；用 ASCII 图展示对象关系与数据流。
