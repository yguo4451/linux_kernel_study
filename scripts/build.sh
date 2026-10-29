#!/usr/bin/env bash
# 使用 Docker 构建最小 Linux 系统（arm64），产物输出到工作区 build/ 与 initramfs.cpio.gz
# 原因：Apple Silicon 上的 Apple clang 无法编译 Linux 内核，需在 Linux 容器内用 GNU 工具链构建
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE_NAME="build-linux-builder"
LOG_FILE="$ROOT_DIR/build/build.log"

command -v docker >/dev/null || { echo "缺少 docker，请先启动 Docker Desktop" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "Docker 未运行，请先启动 Docker Desktop" >&2; exit 1; }

mkdir -p "$ROOT_DIR/build"

echo "==> 构建 builder 镜像（首次较慢，之后有缓存）"
docker build -q -t "$IMAGE_NAME" "$SCRIPT_DIR"

echo "==> 在容器内编译内核 + BusyBox + initramfs（日志: build/build.log）"
docker run --rm \
  -v "$ROOT_DIR":/work \
  -e KERNEL_VERSION="${KERNEL_VERSION:-6.12.112}" \
  -e BUSYBOX_VERSION="${BUSYBOX_VERSION:-1.36.1}" \
  "$IMAGE_NAME" \
  /work/scripts/build_in_container.sh 2>&1 | tee "$LOG_FILE"
