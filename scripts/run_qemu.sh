#!/usr/bin/env bash
# 在 QEMU 中启动 build.sh 构建出的最小 Linux 系统（arm64）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE="${IMAGE:-$ROOT_DIR/build/kernel/Image}"
INITRAMFS="${INITRAMFS:-$ROOT_DIR/initramfs.cpio.gz}"
MEM="${MEM:-512M}"
CPU="${CPU:-host}"
ACCEL="${ACCEL:-hvf}"
SMP="${SMP:-4}"
ISOLCPUS="${ISOLCPUS:-3}"
PCAP="${PCAP:-$ROOT_DIR/build/dump.pcap}"
TELNET_PORT="${TELNET_PORT:-2323}"
SSH_PORT="${SSH_PORT:-2222}"
BOOT_LOG="${BOOT_LOG:-$ROOT_DIR/build/boot.log}"
DISK="${DISK:-$ROOT_DIR/build/disk.img}"

command -v qemu-system-aarch64 >/dev/null || {
  echo "缺少 qemu-system-aarch64，请先 brew install qemu" >&2
  exit 1
}

[ -f "$IMAGE" ] || { echo "未找到内核 Image: $IMAGE (先运行 scripts/build.sh)" >&2; exit 1; }
[ -f "$INITRAMFS" ] || { echo "未找到 initramfs: $INITRAMFS (先运行 scripts/build.sh)" >&2; exit 1; }

echo "启动 QEMU (arm64 virt, $SMP 核, 加速: $ACCEL, 隔离 CPU: $ISOLCPUS)，退出请按 Ctrl+A 再按 X"
echo "网络抓包将写入: $PCAP"
echo "启动日志备份到: $BOOT_LOG"
echo "telnet 端口转发: 宿主机 $TELNET_PORT -> guest 23 (在 guest 内先启动 telnetd)"
echo "ssh 端口转发:   宿主机 $SSH_PORT -> guest 22 (ssh -p $SSH_PORT root@127.0.0.1)"
echo "数据磁盘: $DISK (guest 内 /dev/vda，可 mount 到 /mnt 做页缓存实验)"
rm -f "$PCAP"
: > "$BOOT_LOG"
exec qemu-system-aarch64 \
  -machine virt \
  -accel "$ACCEL" \
  -cpu "$CPU" \
  -smp "$SMP" \
  -nographic \
  -no-reboot \
  -m "$MEM" \
  -kernel "$IMAGE" \
  -initrd "$INITRAMFS" \
  -drive file="$DISK",format=raw,if=virtio \
  -append "console=ttyAMA0 isolcpus=${ISOLCPUS} nohz_full=${ISOLCPUS} rcu_nocbs=${ISOLCPUS} irqaffinity=0-2" \
  -netdev user,id=net0,hostfwd=tcp::${TELNET_PORT}-:23,hostfwd=tcp::${SSH_PORT}-:22 \
  -device virtio-net-pci,netdev=net0 \
  -object filter-dump,id=dump0,netdev=net0,file="$PCAP" 2>&1 | tee "$BOOT_LOG"
