# 20. 动态链接与 SSH/SCP 集成：不再为每个工具重启系统

> 工具链升级篇。把系统从"全静态"切换到"BusyBox 动态 + glibc 运行时"，并集成 OpenSSH 提供 ssh/scp/sftp，从而可以在不重编译内核、不重启 QEMU 的情况下，随时往运行中的系统里传文件（.ko 模块、实验程序、工具）。

## 一、动机：静态链接的"边际成本"在上升

最初选静态 BusyBox 的理由是"根文件系统零动态库依赖、启动路径最简"。这在那时是对的——系统里只有 BusyBox 一个二进制。

但随着工具增多，局面反转了：

| 工具 | 链接方式 |
|------|----------|
| busybox | 静态（内嵌一份 libc） |
| tcpdump + libpcap | 静态（内嵌一份 libc） |
| cyclictest | 静态（内嵌一份 libc） |
| mem_hog/mlock_demo… | 静态（内嵌一份 libc） |
| OpenSSH(sshd/scp) | 如果继续静态，又内嵌一份 libc |

**每加一个工具都要跟 `-static` 搏斗一次**，而且 glibc 程序没有"真静态"：

> glibc 的 `getpwnam()`（读 `/etc/passwd` 做登录认证）和 `getaddrinfo()`（DNS 解析）即使静态链接，运行时仍会 `dlopen` 加载 `/lib/libnss_files.so.2`。所以 dropbear 静态编译后照样需要动态库。

结论：**静态在"单工具"时代是赢家，在"多工具"时代是负债**。切换到动态后，6 个二进制共享同一份 `libc.so.6`，体积反而更小，也更接近真实发行版的形态。

## 二、原理：动态链接的三块拼图

一个动态链接的 ELF 可执行文件，运行时需要三样东西各就各位：

```
① 动态链接器 /lib/ld-linux-aarch64.so.1
   内核加载 ELF 时读到 PT_INTERP 段，这个绝对路径是写死在 ELF 里的。
   内核先加载它，再由它去加载真正的程序。

② 共享库 libc.so.6 / libm.so.6 / ...
   ELF 的 PT_NEEDED 段记录依赖名（如 libc.so.6），
   动态链接器按搜索路径（/lib/aarch64-linux-gnu 等）去找到它们。

③ NSS 后端 libnss_files.so.2 / libnss_dns.so.2
   运行时才 dlopen，负责把 getpwnam/getaddrinfo 落到 /etc/passwd、DNS。
   漏了它，登录认证和域名解析会静默失败。
```

用 `readelf -d` 能直接看到这两段：

```bash
readelf -d /work/build/rootfs/bin/busybox | grep NEEDED
# NEEDED: [libm.so.6] [libresolv.so.2] [libc.so.6] [ld-linux-aarch64.so.1]

readelf -d /work/build/rootfs/usr/sbin/sshd | grep NEEDED
# NEEDED: [libcrypt.so.1] [libcrypto.so.3] [libz.so.1] [libc.so.6] [ld-linux-aarch64.so.1]
```

## 三、SSH 服务选型：从 dropbear 到 OpenSSH

一开始选 dropbear 的理由是"最小系统、体积小、依赖少"。但实际用下来踩了两个兼容性坑：

1. **dropbear 不支持 SFTP 子系统**：macOS 新版 scp（OpenSSH 9.0+）默认走 SFTP，连 dropbear 报 `sh: /usr/libexec/sftp-server: not found`，必须 `scp -O` 强制回退传统 SCP。
2. **dropbear 的 scp 是独立程序**：它不随默认 `PROGRAMS` 编译，且误把它当 dbclient 软链接后，远端 sink 模式直接报 `Bad hostname`。

既然系统已经切成动态链接、加两个 `.so` 的代价可接受，干脆换 OpenSSH，一劳永逸：

| | dropbear | OpenSSH |
|---|---|---|
| 提供 | dropbear + dbclient + scp | sshd + ssh + scp + sftp + sftp-server |
| scp 兼容 | 需 `-O`，sink 有坑 | 完全兼容（SCP+SFTP） |
| sftp / rsync | ❌ | ✅ |
| 依赖 | libc + libz + libcrypt | 再 + libcrypto/libssl（多 4.7MB） |
| 版本选择 | — | 9.6p1（Ubuntu 24.04 LTS 同款） |

结论：**动态链接松绑了"最小依赖"约束后，OpenSSH 是更省心的选择**。OpenSSH 没有官方 LTS，但 9.6p1 是 Ubuntu 24.04 LTS 的版本，社区会长期 backport 安全补丁。

## 四、集成要点（`build_in_container.sh`）

### 1. BusyBox 切动态

```bash
# 若上次是静态编译（.config 里 CONFIG_STATIC=y），删除旧产物强制重编
if grep -q '^CONFIG_STATIC=y' "$BB_DIR/.config" 2>/dev/null; then
  rm -f "$BB_DIR/busybox"
fi
# 动态编译：defconfig 默认非静态，sed 只是防御性兜底
sed -i 's/^CONFIG_STATIC=y/# CONFIG_STATIC is not set/' "$BB_DIR/.config"
```

**关键坑**：只改脚本不够——如果 `$BB_DIR/busybox` 二进制已存在，`if [ ! -f ... ]` 会跳过重编。必须检测 `.config` 里的 `CONFIG_STATIC=y` 并删除旧产物。

### 2. 复制 glibc 运行时（最小集合）

容器是 arm64 原生，glibc 真实库在 `/usr/lib/aarch64-linux-gnu/`（Ubuntu usr-merge）：

```bash
LIBS_DEST="$ROOTFS/lib/aarch64-linux-gnu"
for lib in ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libresolv.so.2 \
           libcrypt.so.1.1.0 libnss_files.so.2 libnss_dns.so.2 libz.so.1.2.11; do
  [ -f "/usr/lib/aarch64-linux-gnu/$lib" ] && cp -a "/usr/lib/aarch64-linux-gnu/$lib" "$LIBS_DEST/"
done
ln -sf libcrypt.so.1.1.0 "$LIBS_DEST/libcrypt.so.1"
ln -sf libz.so.1.2.11    "$LIBS_DEST/libz.so.1"
# 动态链接器必须出现在 ELF 硬编码的绝对路径
ln -sf aarch64-linux-gnu/ld-linux-aarch64.so.1 "$ROOTFS/lib/ld-linux-aarch64.so.1"
```

两个关键点：

- **`libcrypt.so.1`、`libz.so.1` 是符号链接**，指向带版本的真实文件；复制时要连同真实文件一起，再补符号链接。`libc.so.6` 等在这个镜像里本身就是真实文件（无 `libc-2.35.so` 版本化名字），直接复制即可。
- **动态链接器路径是硬编码的**：ELF 的 `PT_INTERP` 写死 `/lib/ld-linux-aarch64.so.1`，guest 里这个绝对路径必须能解析到真实文件，否则内核报 `No such file or directory`。

### 3. 空密码登录（实验环境免密）

```bash
cat > "$ROOTFS/etc/passwd" <<'EOF'
root::0:0:root:/root:/bin/sh      # 第二字段空 = 空密码
EOF
cat > "$ROOTFS/etc/shadow" <<'EOF'
root::0:0:99999:7:::
EOF
```

`/init` 里生成/恢复 host key 并启动 sshd：
- `PermitEmptyPasswords yes`：允许空密码登录（配合空密码 shadow）
- host key 是服务器的"身份证"，initramfs 易失，**若不持久化，每次重启 key 都会变，宿主机 `known_hosts` 会报 `Host key verification failed`**

```sh
mkdir -p /etc/ssh /run /var/empty
HOSTKEY=/etc/ssh/ssh_host_ed25519_key
if [ -f /mnt/ssh_host_ed25519_key ]; then
	cp /mnt/ssh_host_ed25519_key "$HOSTKEY"
else
	ssh-keygen -t ed25519 -f "$HOSTKEY" -N "" >/dev/null 2>&1
	cp "$HOSTKEY" /mnt/ssh_host_ed25519_key 2>/dev/null
fi
/usr/sbin/sshd &
```

sshd 还需要 `/etc/ssh/sshd_config`（`PermitRootLogin yes` + `PermitEmptyPasswords yes` + `Subsystem sftp`）和一个 `sshd` 用户（privilege separation 的非特权用户）。

### 4. QEMU 端口转发（`run_qemu.sh`）

```bash
SSH_PORT="${SSH_PORT:-2222}"
-netdev user,id=net0,hostfwd=tcp::${TELNET_PORT}-:23,hostfwd=tcp::${SSH_PORT}-:22
```

宿主机 2222 → guest 22。

## 五、使用方法

```bash
# 宿主机登录（空密码，密码提示直接回车；首次连接确认指纹输入 yes）
ssh -p 2222 root@127.0.0.1

# 宿主机传文件进运行中的系统（大写 -P 指定端口）
scp -P 2222 mymodule.ko root@127.0.0.1:/tmp/

# 从运行中的系统取文件回来
scp -P 2222 root@127.0.0.1:/var/log/messages ./
```

这正是阶段四"内核模块/驱动开发"的关键基础设施：**编译好的 `.ko` 和实验程序不再需要打进 initramfs 重启，直接 `scp` 进运行中的系统即可**。

## 六、验证

构建后先在容器里用 `chroot` 冒烟测试，提前排除"库路径错导致系统起不来"：

```bash
chroot /work/build/rootfs /bin/busybox echo "busybox-dynamic-OK"
chroot /work/build/rootfs /lib/ld-linux-aarch64.so.1 --list /usr/sbin/sshd   # 验证库解析
chroot /work/build/rootfs /bin/busybox whoami   # 走 NSS 解析 /etc/passwd → root
```

`whoami` 能返回 `root`，说明 `libnss_files.so.2` 路径正确、NSS 后端可用——这正是登录认证的底层依赖。

## 七、陷阱记录

1. **切动态后忘了强制重编 BusyBox**：`busybox` 二进制已存在会跳过编译，必须检测 `.config` 的 `CONFIG_STATIC=y` 并删旧产物。
2. **符号链接断链**：容器里 `libcrypt.so.1 → libcrypt.so.1.1.0`，只复制符号链接不复制真实文件，guest 里就是断链。
3. **动态链接器路径**：`ld-linux-aarch64.so.1` 的路径是 ELF 硬编码的，不能用别的路径。
4. **NSS 库漏拷**：静态 BusyBox 用不到 NSS，但动态后 `whoami`/`getpwnam` 都依赖它，漏拷会导致登录认证静默失败。
5. **usb-merge 陷阱**：Ubuntu 容器里 `/lib` 是 `/usr/lib` 的符号链接，`readlink -f` 解析出的真实路径是 `/usr/lib/...`，复制时要以真实文件为准。
6. **host key 每次重启变化**：initramfs 是内存盘，host key 重启即丢，宿主机 `known_hosts` 会报 `Host key verification failed`。根治：host key 持久化到 `/mnt` 数据盘（见第三节），客户端侧用 `ssh-keygen -R '[127.0.0.1]:2222'` 清除旧条目。
7. **dropbear scp 协议不兼容**：macOS 新版 scp 默认 SFTP，dropbear 报 `sftp-server not found`；且 dropbear 的 scp 需单独编译（默认 `PROGRAMS` 不含 scp）。换 OpenSSH 后全部消失，scp/sftp 开箱即用。

## 八、对比：静态 vs 动态的取舍

| 维度 | 静态（旧） | 动态（新） |
|------|-----------|-----------|
| rootfs 依赖 | 零动态库 | 需 glibc 运行时（~2.7MB） |
| 新增工具成本 | 逐个 `-static`，且 glibc 假静态 | 直接 `scp` 进去就能跑 |
| 体积 | 每个二进制内嵌 libc | 共享一份 libc，总量更小 |
| 启动路径 | 最简 | 多一步动态链接器 |
| 贴近真实系统 | 否 | 是（发行版都是动态的） |
