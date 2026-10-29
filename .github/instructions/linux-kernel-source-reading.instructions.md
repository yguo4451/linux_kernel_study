---
description: "Use when reading or analyzing Linux kernel source code (arch/, fs/, kernel/, mm/, net/, include/linux/, drivers/), tracing a syscall call chain, explaining a kernel subsystem, or writing kernel modules. Covers data-structure-first reading order, subsystem boundaries, call-chain tracing, and output conventions."
name: "Linux 内核源码阅读规范"
applyTo:
  - "linux/**"
  - "linux-*/**"
  - "kernel-src/**"
  - "src/linux/**"
---
# Linux 内核源码阅读规范

## 阅读顺序（数据优先）

1. 先找核心数据结构（`task_struct`、`inode`、`sk_buff`、`page`、`vm_area_struct`），再读操作函数
2. 先看头文件定义（`include/linux/`），再看实现（`fs/` `mm/` `kernel/` `net/` `drivers/`）
3. 沿"结构体定义 → 初始化 → 关键路径"追踪，用 ctags/cscope 或 IDE 跳转

## 子系统边界

- `fs/` 文件系统与 VFS；`mm/` 内存管理；`kernel/` 调度与进程；`net/` 协议栈；`drivers/` 设备驱动
- 跨子系统交互（如页缓存位于 mm 与 fs 之间）必须指出数据流动方向与职责归属

## 分析方法

- 每个关键函数标注：入参含义、返回语义、持有的锁、调用上下文（进程上下文/中断上下文/软中断）
- 追踪一条完整调用链，例如 `read()` → `sys_read` → `vfs_read` → `file->f_op->read` → 页缓存
- 区分"设计意图"与"实现细节"，优先解释 why 而非只描述 what

## 输出约定

- 用 ASCII 结构图展示对象关系与数据流
- 引用真实文件路径与行号（如 `fs/open.c:123`）
- 每个结论落到源码依据，禁止臆测
