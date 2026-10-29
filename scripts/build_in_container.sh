#!/usr/bin/env bash
# 在 Linux 容器内执行的构建脚本：编译 arm64 内核 + 静态 BusyBox + 打包 initramfs
set -euo pipefail

KERNEL_VERSION="${KERNEL_VERSION:-6.12.112}"
BUSYBOX_VERSION="${BUSYBOX_VERSION:-1.36.1}"
JOBS="$(nproc)"
ARCH="arm64"
WORK="/work"
SRC_DIR="$WORK/src"
BUILD_DIR="$WORK/build"
KBUILD_DIR="/tmp/kbuild"          # 容器本地目录，避免 bind mount 上的慢速写
KLOG="$BUILD_DIR/kernel-build.log"
BBLOG="$BUILD_DIR/busybox-build.log"
OUT_INITRAMFS="$WORK/initramfs.cpio.gz"

log() { printf '\033[1;32m[build-linux]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[build-linux]\033[0m %s\n' "$*" >&2; exit 1; }

mkdir -p "$SRC_DIR" "$BUILD_DIR"

# ---------- 1. 内核源码 ----------
KERNEL_DIR="$SRC_DIR/linux-$KERNEL_VERSION"
if [ ! -f "$KERNEL_DIR/Makefile" ]; then
  TARBALL="$SRC_DIR/linux-$KERNEL_VERSION.tar.xz"
  if [ ! -f "$TARBALL" ]; then
    MAJOR="${KERNEL_VERSION%%.*}"
    log "下载内核 $KERNEL_VERSION"
    wget -q --timeout=60 "https://mirrors.aliyun.com/linux-kernel/v${MAJOR}.x/linux-${KERNEL_VERSION}.tar.xz" -O "$TARBALL" \
      || wget -q --timeout=60 "https://cdn.kernel.org/pub/linux/kernel/v${MAJOR}.x/linux-${KERNEL_VERSION}.tar.xz" -O "$TARBALL" \
      || fail "内核下载失败，请手动放入 $TARBALL"
  fi
  log "解压内核源码（先在容器内 /tmp 解压，再 rsync 回工作区，规避挂载盘 cp 竞态）"
  # 挂载卷上 rm -rf 可能是异步返回的，循环等待目录真正删除干净
  for _ in 1 2 3 4 5; do
    rm -rf "$KERNEL_DIR" /tmp/kernel-extract
    sync
    [ ! -e "$KERNEL_DIR" ] && break
    sleep 1
  done
  [ ! -e "$KERNEL_DIR" ] || fail "内核源码目录删除未完成，请手动清理 $KERNEL_DIR 后重试"
  mkdir -p /tmp/kernel-extract "$KERNEL_DIR"
  tar -C /tmp/kernel-extract -xf "$TARBALL"
  # rsync 对大量小文件 + VirtioFS 挂载卷比 cp -a 可靠：覆盖式同步，不会因 EEXIST 失败
  rsync -a "/tmp/kernel-extract/linux-$KERNEL_VERSION/" "$KERNEL_DIR/"
  sync
  rm -rf /tmp/kernel-extract
fi

# ---------- 2. BusyBox 源码 ----------
BB_DIR="$SRC_DIR/busybox-$BUSYBOX_VERSION"
if [ ! -f "$BB_DIR/Makefile" ]; then
  TARBALL="$SRC_DIR/busybox-$BUSYBOX_VERSION.tar.bz2"
  if [ ! -f "$TARBALL" ]; then
    log "下载 BusyBox $BUSYBOX_VERSION"
    wget -q --timeout=60 "https://busybox.net/downloads/busybox-$BUSYBOX_VERSION.tar.bz2" -O "$TARBALL" \
      || fail "BusyBox 下载失败，请手动放入 $TARBALL"
  fi
  log "解压 BusyBox（先在容器内 /tmp 解压，再复制回工作区）"
  rm -rf "$BB_DIR" /tmp/bb-extract
  mkdir -p /tmp/bb-extract
  tar -C /tmp/bb-extract -xjf "$TARBALL"
  cp -a /tmp/bb-extract/busybox-$BUSYBOX_VERSION "$BB_DIR"
  rm -rf /tmp/bb-extract
fi

# ---------- 3. 编译内核 ----------
IMAGE="$KBUILD_DIR/arch/arm64/boot/Image"
CACHED_IMAGE="$BUILD_DIR/kernel/Image"
if [ -f "$CACHED_IMAGE" ]; then
  log "复用已编译的内核 Image（跳过编译，如需重编请删除 build/kernel/Image）"
else
  if [ ! -f "$IMAGE" ]; then
    log "配置内核 (arm64 defconfig)"
    make -C "$KERNEL_DIR" O="$KBUILD_DIR" ARCH=arm64 defconfig >> "$KLOG" 2>&1
    log "启用调度器调试 (CONFIG_SCHED_DEBUG)：观测 EEVDF 的 vruntime/deadline/vlag"
    "$KERNEL_DIR/scripts/config" --file "$KBUILD_DIR/.config" \
      -e DEBUG_KERNEL -e SCHED_DEBUG -e SCHEDSTATS
    log "启用 CPU 隔离进阶 (CONFIG_NO_HZ_FULL / RCU_NOCB_CPU)：支持 nohz_full=3 rcu_nocbs=3"
    "$KERNEL_DIR/scripts/config" --file "$KBUILD_DIR/.config" \
      -e NO_HZ_FULL -e RCU_NOCB_CPU
    log "启用 PREEMPT_RT（完全可抢占实时内核）：EXPERT + PREEMPT_RT，禁用普通 PREEMPT"
    "$KERNEL_DIR/scripts/config" --file "$KBUILD_DIR/.config" \
      -e EXPERT -e PREEMPT_RT -d PREEMPT
    log "启用 CFS 带宽控制 (CONFIG_CFS_BANDWIDTH)：cgroup v2 的 cpu.max 带宽限制"
    "$KERNEL_DIR/scripts/config" --file "$KBUILD_DIR/.config" \
      -e CFS_BANDWIDTH
    log "启用 PSI (CONFIG_PSI)：/proc/pressure/memory 内存压力监控"
    "$KERNEL_DIR/scripts/config" --file "$KBUILD_DIR/.config" \
      -e PSI
    log "启用 DMABUF_HEAPS：用户态 dma-heap 分配 dmabuf"
    "$KERNEL_DIR/scripts/config" --file "$KBUILD_DIR/.config" \
      -e DMABUF_HEAPS -e DMABUF_HEAPS_SYSTEM
    # THP 与 PREEMPT_RT 互斥（mm/Kconfig: depends on !PREEMPT_RT），无法开启。
    # 实时内核用显式巨页 hugetlbfs(MAP_HUGETLB) 代替，见 doc/05-memory。
    log "跳过 THP：TRANSPARENT_HUGEPAGE 依赖 !PREEMPT_RT，实时内核用 hugetlbfs 代替"
    log "启用 zram/zswap (压缩内存)：车载/嵌入式标配"
    "$KERNEL_DIR/scripts/config" --file "$KBUILD_DIR/.config" \
      -e ZRAM -e ZSMALLOC -e ZSWAP -e ZPOOL
    make -C "$KERNEL_DIR" O="$KBUILD_DIR" ARCH=arm64 olddefconfig >> "$KLOG" 2>&1
    log "编译内核 (make Image, -j$JOBS)，日志: build/kernel-build.log"
    make -C "$KERNEL_DIR" O="$KBUILD_DIR" ARCH=arm64 -j"$JOBS" Image >> "$KLOG" 2>&1 \
      || { tail -30 "$KLOG"; fail "内核编译失败，详见 build/kernel-build.log"; }
  fi

  # 复制产物回工作区
  mkdir -p "$BUILD_DIR/kernel"
  cp "$IMAGE" "$BUILD_DIR/kernel/Image"
  [ -f "$KBUILD_DIR/.config" ] && cp "$KBUILD_DIR/.config" "$BUILD_DIR/kernel/.config"
fi

# ---------- 4. 编译 BusyBox ----------
# 动态编译：与 dropbear 共享 glibc（多工具时代静态链接的边际成本已超过动态）
# 若上次是静态编译（.config 里 CONFIG_STATIC=y），删除旧产物强制重编
if grep -q '^CONFIG_STATIC=y' "$BB_DIR/.config" 2>/dev/null; then
  rm -f "$BB_DIR/busybox"
fi
if [ ! -f "$BB_DIR/busybox" ]; then
  log "编译 BusyBox (动态，与 dropbear 共享 glibc)，日志: build/busybox-build.log"
  make -C "$BB_DIR" defconfig >> "$BBLOG" 2>&1
  sed -i 's/^CONFIG_STATIC=y/# CONFIG_STATIC is not set/' "$BB_DIR/.config"
  make -C "$BB_DIR" -j"$JOBS" >> "$BBLOG" 2>&1 \
    || { tail -30 "$BBLOG"; fail "BusyBox 编译失败"; }
fi

# ---------- 4.5 编译静态 tcpdump（BusyBox 1.36 已移除 tcpdump，此处源码编译补上） ----------
LIBPCAP_VERSION="${LIBPCAP_VERSION:-1.10.4}"
TCPDUMP_VERSION="${TCPDUMP_VERSION:-4.99.4}"
TCPDUMP_LOG="$BUILD_DIR/tcpdump-build.log"
LIBPCAP_DIR="$SRC_DIR/libpcap-$LIBPCAP_VERSION"
TCPDUMP_DIR="$SRC_DIR/tcpdump-$TCPDUMP_VERSION"

fetch_tcpdump_src() {
  # $1=包名  $2=版本  $3=源码目录（先解压到 /tmp 再 cp 回工作区，规避挂载盘 tar 竞态）
  local name="$1" ver="$2" dir="$3" tarball="$SRC_DIR/$1-$2.tar.gz"
  if [ ! -f "$dir/configure" ]; then
    if [ ! -f "$tarball" ]; then
      log "下载 $name $ver"
      wget -q --timeout=60 "https://www.tcpdump.org/release/$name-$ver.tar.gz" -O "$tarball" \
        || fail "$name 下载失败，请手动放入 $tarball"
    fi
    log "解压 $name（先在 /tmp 解压，再复制回工作区）"
    rm -rf "$dir" "/tmp/$name-extract"
    mkdir -p "/tmp/$name-extract"
    tar -C "/tmp/$name-extract" -xzf "$tarball"
    cp -a "/tmp/$name-extract/$name-$ver" "$dir"
    rm -rf "/tmp/$name-extract"
  fi
}

fetch_tcpdump_src libpcap "$LIBPCAP_VERSION" "$LIBPCAP_DIR"
fetch_tcpdump_src tcpdump "$TCPDUMP_VERSION" "$TCPDUMP_DIR"

if [ ! -f "$TCPDUMP_DIR/tcpdump" ]; then
  log "编译静态 libpcap + tcpdump（日志: build/tcpdump-build.log）"
  # libpcap：静态库 + pcap-config
  ( cd "$LIBPCAP_DIR" && \
    ./configure --disable-shared --enable-static >> "$TCPDUMP_LOG" 2>&1 && \
    make -j"$JOBS" >> "$TCPDUMP_LOG" 2>&1 ) \
    || { tail -30 "$TCPDUMP_LOG"; fail "libpcap 编译失败，详见 build/tcpdump-build.log"; }
  # tcpdump：静态链接 libpcap（通过 PATH 找到 pcap-config）
  ( cd "$TCPDUMP_DIR" && \
    PATH="$LIBPCAP_DIR:$PATH" ./configure --disable-shared --enable-static \
      --without-crypto LDFLAGS="-static" >> "$TCPDUMP_LOG" 2>&1 && \
    make -j"$JOBS" >> "$TCPDUMP_LOG" 2>&1 ) \
    || { tail -30 "$TCPDUMP_LOG"; fail "tcpdump 编译失败，详见 build/tcpdump-build.log"; }
fi

# ---------- 4.6 编译静态 cyclictest（实时性抖动测量，PREEMPT_RT 验证标配） ----------
RT_TESTS_VERSION="${RT_TESTS_VERSION:-2.11}"
RT_DIR="$SRC_DIR/rt-tests-$RT_TESTS_VERSION"
RT_LOG="$BUILD_DIR/rt-tests-build.log"

if [ ! -f "$RT_DIR/cyclictest" ]; then
  TARBALL="$SRC_DIR/rt-tests-$RT_TESTS_VERSION.tar.xz"
  if [ ! -f "$TARBALL" ]; then
    log "下载 rt-tests $RT_TESTS_VERSION"
    wget -q --timeout=60 "https://www.kernel.org/pub/linux/utils/rt-tests/rt-tests-$RT_TESTS_VERSION.tar.xz" -O "$TARBALL" \
      || fail "rt-tests 下载失败，请手动放入 $TARBALL"
  fi
  log "解压 rt-tests（先在 /tmp 解压，再复制回工作区）"
  rm -rf "$RT_DIR" /tmp/rt-extract
  mkdir -p /tmp/rt-extract
  tar -C /tmp/rt-extract -xf "$TARBALL"
  cp -a "/tmp/rt-extract/rt-tests-$RT_TESTS_VERSION" "$RT_DIR"
  rm -rf /tmp/rt-extract
  log "编译静态 cyclictest（日志: build/rt-tests-build.log）"
  make -C "$RT_DIR" cyclictest LDFLAGS="-static" >> "$RT_LOG" 2>&1 \
    || { tail -30 "$RT_LOG"; fail "cyclictest 编译失败，详见 build/rt-tests-build.log"; }
fi

# ---------- 4.7 编译 OpenSSH（动态链接 glibc，提供 sshd/ssh/scp/sftp） ----------
# 选 OpenSSH 而非 dropbear：scp/sftp 协议完全兼容（macOS 新版 scp 默认 SFTP，dropbear 不支持）
OPENSSH_VERSION="${OPENSSH_VERSION:-9.6p1}"
OPENSSH_DIR="$SRC_DIR/openssh-$OPENSSH_VERSION"
OPENSSH_LOG="$BUILD_DIR/openssh-build.log"

if [ ! -f "$OPENSSH_DIR/sshd" ]; then
  TARBALL="$SRC_DIR/openssh-$OPENSSH_VERSION.tar.gz"
  if [ ! -f "$TARBALL" ]; then
    log "下载 OpenSSH $OPENSSH_VERSION（清华镜像）"
    wget -q --timeout=60 "https://mirrors.tuna.tsinghua.edu.cn/OpenBSD/OpenSSH/portable/openssh-$OPENSSH_VERSION.tar.gz" -O "$TARBALL" \
      || fail "OpenSSH 下载失败，请手动放入 $TARBALL"
  fi
  log "解压 OpenSSH（先在 /tmp 解压，再复制回工作区）"
  rm -rf "$OPENSSH_DIR" /tmp/openssh-extract
  mkdir -p /tmp/openssh-extract
  tar -C /tmp/openssh-extract -xzf "$TARBALL"
  cp -a "/tmp/openssh-extract/openssh-$OPENSSH_VERSION" "$OPENSSH_DIR"
  rm -rf /tmp/openssh-extract
  log "编译 OpenSSH（动态链接 glibc，日志: build/openssh-build.log）"
  ( cd "$OPENSSH_DIR" && \
    ./configure --prefix=/usr --sysconfdir=/etc/ssh >> "$OPENSSH_LOG" 2>&1 && \
    make -j"$JOBS" >> "$OPENSSH_LOG" 2>&1 ) \
    || { tail -30 "$OPENSSH_LOG"; fail "OpenSSH 编译失败，详见 build/openssh-build.log"; }
fi

# ---------- 5. 根文件系统 ----------
ROOTFS="$BUILD_DIR/rootfs"
log "构建根文件系统 -> build/rootfs"
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS"/{bin,sbin,etc,proc,sys,dev,tmp,root,mnt,usr/bin,usr/sbin}
make -C "$BB_DIR" CONFIG_PREFIX="$ROOTFS" install >> "$BBLOG" 2>&1

# 动态链接运行时：从容器 glibc sysroot 复制最小 .so 集合（BusyBox/OpenSSH 动态链接 + NSS 用户解析）
LIBS_DEST="$ROOTFS/lib/aarch64-linux-gnu"
mkdir -p "$ROOTFS/lib" "$LIBS_DEST"
SYSROOT_LIB="/usr/lib/aarch64-linux-gnu"
for lib in ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libresolv.so.2 \
           libcrypt.so.1.1.0 libnss_files.so.2 libnss_dns.so.2 libz.so.1.2.11 \
           libcrypto.so.3 libssl.so.3; do
  [ -f "$SYSROOT_LIB/$lib" ] && cp -a "$SYSROOT_LIB/$lib" "$LIBS_DEST/"
done
# 符号链接：补齐 .so.1/.so.2 版式名（PT_NEEDED 记录的依赖名）
ln -sf libcrypt.so.1.1.0 "$LIBS_DEST/libcrypt.so.1"
ln -sf libz.so.1.2.11    "$LIBS_DEST/libz.so.1"
# 动态链接器：ELF 的 PT_INTERP 硬编码 /lib/ld-linux-aarch64.so.1，必须出现在此绝对路径
ln -sf aarch64-linux-gnu/ld-linux-aarch64.so.1 "$ROOTFS/lib/ld-linux-aarch64.so.1"

# 用户数据库：whoami/su/login 等工具靠 /etc/passwd、/etc/group 把 uid/gid 翻译成名字
# 密码字段留空（第二字段空 = 空密码），sshd PermitEmptyPasswords 允许空密码登录（实验环境免密）
# sshd 用户：OpenSSH 的 privilege separation 需要非特权用户
cat > "$ROOTFS/etc/passwd" <<'EOF'
root::0:0:root:/root:/bin/sh
sshd:x:22:22:sshd:/var/empty:/bin/false
EOF
cat > "$ROOTFS/etc/group" <<'EOF'
root:x:0:
sshd:x:22:
EOF
# shadow 的 lastchg（第3字段）必须非 0：OpenSSH auth-shadow.c 里 sp_lstchg==0 会判定"密码已过期"
LASTCHG=$(( $(date +%s) / 86400 ))
cat > "$ROOTFS/etc/shadow" <<EOF
root::${LASTCHG}:0:99999:7:::
sshd:*:${LASTCHG}:0:99999:7:::
EOF

# 静态 tcpdump 抓包工具（源码编译产物）
if [ -f "$TCPDUMP_DIR/tcpdump" ]; then
  cp "$TCPDUMP_DIR/tcpdump" "$ROOTFS/usr/bin/tcpdump"
fi

# 静态 cyclictest 实时性抖动测量工具（源码编译产物）
if [ -f "$RT_DIR/cyclictest" ]; then
  cp "$RT_DIR/cyclictest" "$ROOTFS/usr/bin/cyclictest"
fi

# OpenSSH：sshd 服务端 + ssh/scp/sftp 客户端 + sftp-server 子系统（scp 是独立程序，不是软链接）
if [ -f "$OPENSSH_DIR/sshd" ]; then
  cp "$OPENSSH_DIR/sshd" "$ROOTFS/usr/sbin/"
  cp "$OPENSSH_DIR/ssh" "$OPENSSH_DIR/scp" "$OPENSSH_DIR/sftp" "$OPENSSH_DIR/ssh-keygen" "$ROOTFS/usr/bin/"
  mkdir -p "$ROOTFS/usr/libexec"
  cp "$OPENSSH_DIR/sftp-server" "$ROOTFS/usr/libexec/"
fi

# sshd 配置：允许 root 空密码登录 + sftp 子系统（实验环境免密）
mkdir -p "$ROOTFS/etc/ssh"
cat > "$ROOTFS/etc/ssh/sshd_config" <<'EOF'
PermitRootLogin yes
PermitEmptyPasswords yes
HostKey /etc/ssh/ssh_host_ed25519_key
Subsystem sftp /usr/libexec/sftp-server
EOF

# 内存实验程序（mem_hog/mlock_demo 等，静态编译进 rootfs）
if [ -d "$WORK/experiments/memory" ]; then
  for src in "$WORK/experiments/memory"/*.c; do
    name="$(basename "$src" .c)"
    gcc -static -O2 -o "$ROOTFS/usr/bin/$name" "$src" 2>/dev/null \
      && log "编译实验程序 $name" || true
  done
fi

cat > "$ROOTFS/init" <<'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
# 调度调试信息（EEVDF deadline/vlag）在 debugfs：/sys/kernel/debug/sched/
mkdir -p /sys/kernel/debug
mount -t debugfs debugfs /sys/kernel/debug
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts
# 数据磁盘：挂载 ext4 到 /mnt，并启用 swapfile（页缓存/swap 实验用）
mkdir -p /mnt
mount -t ext4 /dev/vda /mnt 2>/dev/null
if [ -f /mnt/swapfile ]; then
	swapon /mnt/swapfile
fi
# POSIX 共享内存（shm_open）需要 /dev/shm，cyclictest 依赖它
mkdir -p /dev/shm
mount -t tmpfs tmpfs /dev/shm
mount -t tmpfs tmpfs /tmp
# cgroup v2：资源隔离（CPU/内存/cpuset 统一层级），ECU 功能分区的基础
mkdir -p /sys/fs/cgroup
mount -t cgroup2 cgroup /sys/fs/cgroup
# cgroup v2 需显式启用控制器，子 cgroup 才有 cpu.max/cpuset.cpus 等文件
echo "+cpu +cpuset +memory +pids" > /sys/fs/cgroup/cgroup.subtree_control
# 日志落盘：syslogd 接收日志写文件，klogd 转发内核日志
mkdir -p /var/log
# lastlog 登录历史：dropbear 登录时会写，缺失会告警，touch 空文件消除告警
touch /var/log/lastlog
syslogd -O /var/log/messages &
klogd &
# 网络自动配置：先拉起网卡，再 DHCP 获取 IP/路由/DNS
ifconfig lo up
ifconfig eth0 up
udhcpc -i eth0 &
# SSH 服务：宿主机 ssh -p 2222 / scp -P 2222 root@127.0.0.1
# host key 是服务器的"身份证"：initramfs 易失，持久化到 /mnt（ext4 数据盘）避免每次重启 key 变化
mkdir -p /etc/ssh /run /var/empty
HOSTKEY=/etc/ssh/ssh_host_ed25519_key
if [ -f /mnt/ssh_host_ed25519_key ]; then
	cp /mnt/ssh_host_ed25519_key "$HOSTKEY"
else
	ssh-keygen -t ed25519 -f "$HOSTKEY" -N "" >/dev/null 2>&1
	cp "$HOSTKEY" /mnt/ssh_host_ed25519_key 2>/dev/null
fi
/usr/sbin/sshd &
# 网络登录会话：宿主机 telnet 127.0.0.1:2323 进入本系统（多终端共用一套系统）
telnetd -l /bin/sh &
echo "Welcome to your own Linux!"
setsid cttyhack sh
EOF
chmod +x "$ROOTFS/init"

# 统计脚本：替代 BusyBox netstat 缺失的 -s 功能（解析 /proc/net/snmp、/proc/net/netstat）
if [ -f "$WORK/scripts/netstat_stat.sh" ]; then
  cp "$WORK/scripts/netstat_stat.sh" "$ROOTFS/usr/bin/netstat_stat.sh"
  chmod +x "$ROOTFS/usr/bin/netstat_stat.sh"
fi

# DHCP 租约应用脚本（udhcpc 拿到租约后调用它设置 IP/路由/DNS）
mkdir -p "$ROOTFS/usr/share/udhcpc"
cat > "$ROOTFS/usr/share/udhcpc/default.script" <<'SCRIPT'
#!/bin/sh
[ -n "$1" ] || exit 1
RESOLV_CONF="/etc/resolv.conf"
case "$1" in
	deconfig)
		ifconfig "$interface" 0.0.0.0
		;;
	bound|renew)
		ifconfig "$interface" "$ip" netmask "$subnet"
		[ -n "$router" ] && route add default gw "$router" dev "$interface"
		> "$RESOLV_CONF"
		for i in $dns; do echo "nameserver $i" >> "$RESOLV_CONF"; done
		;;
esac
exit 0
SCRIPT
chmod +x "$ROOTFS/usr/share/udhcpc/default.script"

# ---------- 6. initramfs ----------
log "打包 initramfs -> initramfs.cpio.gz"
( cd "$ROOTFS" && find . | cpio -o -H newc 2>/dev/null | gzip > "$OUT_INITRAMFS" )

log "完成：内核 Image -> $BUILD_DIR/kernel/Image"
log "完成：initramfs -> $OUT_INITRAMFS"
log "下一步：在宿主机运行 scripts/run_qemu.sh 启动"
