# 10 PREEMPT_RT 内核升级（6.6 → 6.12）

## 目标

在 arm64 内核上真正开启 `CONFIG_PREEMPT_RT`，拿到完全可抢占实时内核，为 cyclictest 实测和后续汽车实时方向打底。

## 核心发现：6.6 主线里 arm64 开不了 PREEMPT_RT

第一次尝试在 6.6.44 上执行：

```bash
scripts/config -e EXPERT -e PREEMPT_RT -d PREEMPT
make olddefconfig
```

编译"成功"了，但 `.config` 里实际是：

```
CONFIG_PREEMPT_NONE=y        ← 被静默回退
（完全没有 CONFIG_PREEMPT_RT）
```

**根因链**：

1. `PREEMPT_RT` 的 Kconfig 依赖（`kernel/Kconfig.preempt:72`）：
   ```c
   config PREEMPT_RT
       depends on EXPERT && ARCH_SUPPORTS_RT
   ```
2. `ARCH_SUPPORTS_RT` 是空开关，必须由架构主动 `select` 它。
3. 全树搜索 `select ARCH_SUPPORTS_RT` → **空**。6.6.44 里没有任何架构 select 它。

**历史事实**：PREEMPT_RT 的完整主线化（包括 arm64 的 `ARCH_SUPPORTS_RT`）是在 **Linux 6.12**（2024-11）才合入的。6.6 里 `CONFIG_PREEMPT_RT` 符号虽然存在，但 arm64 无法真正开启。

## 升级到 6.12.112

| 维度 | 6.6.44 | 6.12.112 |
|------|--------|----------|
| 类型 | LTS | LTS |
| arm64 PREEMPT_RT | ❌ 不支持 | ✅ 原生支持（首个） |
| 汽车行业采用 | 量产主流之一 | 正在成为下一代基线 |

改动：`scripts/build.sh` 和 `scripts/build_in_container.sh` 的 `KERNEL_VERSION` 从 `6.6.44` 改为 `6.12.112`。

## 踩坑记录

### 坑 1：olddefconfig 的依赖校验会静默回退

`scripts/config -e` 只是改 `.config` 文本，`make olddefconfig` 会重新校验 `depends on` 依赖。依赖不满足时**不报错、只回退**——这是为什么第一次"编译成功"但 RT 没生效。

**教训**：改完 config 后必须 `grep` 最终 `.config` 确认，不能只看编译日志。

### 坑 2：VirtioFS 挂载卷上 `cp -a` 大量小文件报 File exists

升级 6.12 后，源码解压阶段 `cp -a /tmp/kernel-extract/linux-6.12.112 "$KERNEL_DIR"` 反复报：

```
cp: cannot create regular file '.../include/uapi/linux/netfilter/xt_connmark.h': File exists
```

**排查过程**（三个假设逐一排除）：

1. ❌ 目录残留竞态 —— 换 PID 唯一临时目录 + 原子 mv 后依然报错
2. ❌ 硬链接 —— `tar -tvJf` 查无 `link to` 条目；6.6.44 源码里 `links > 1` 的文件为 0
3. ❌ 符号链接 —— tar 归档里这些文件全是普通文件 `-rw-rw-r--`
4. ✅ **结论**：`cp -a` 向 macOS Docker 的 VirtioFS 挂载卷写几万个小文件时，`O_EXCL` 语义撞上 VirtioFS 的 inode 缓存不一致，偶发 `EEXIST`

**解决**：`cp -a` → `rsync -a`（覆盖式同步，不会因 EEXIST 失败，专为大量文件可靠传输设计）。

改动：
- `scripts/Dockerfile`：apt 安装列表加 `rsync`
- `scripts/build_in_container.sh`：内核源码复制改用 `rsync -a`，并保留 `rm -rf` 后的 `sync` + 循环验证删除干净

## 验证结果

```
# CONFIG_PREEMPT_NONE is not set
CONFIG_PREEMPT_RT=y            ← 真正开启
CONFIG_ARCH_SUPPORTS_RT=y      ← arm64 6.12 原生支持
```

`Image` 46MB（12:02 生成），比 6.6 的 42MB 略大（PREEMPT_RT 的 rt_mutex 化锁、线程化中断带来的代码膨胀）。

## 下一步

重启 QEMU，验证 `uname -v` 出现 `PREEMPT_RT`，再用 cyclictest 对比 PREEMPT_RT 与普通内核的延迟差异。
