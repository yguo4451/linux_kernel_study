#!/usr/bin/env bash
# 进入 build-linux-builder 容器（常驻，退出后不删除）
# 首次运行自动创建容器；之后每次运行直接进入
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE_NAME="build-linux-builder"
CONTAINER_NAME="linux-builder"

command -v docker >/dev/null || { echo "缺少 docker，请先启动 Docker Desktop" >&2; exit 1; }

# 镜像不存在则先构建
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  echo "==> 镜像 $IMAGE_NAME 不存在，先构建..."
  docker build -t "$IMAGE_NAME" "$SCRIPT_DIR"
fi

# 容器不存在则创建（后台常驻，挂载工作区到 /work）
if ! docker container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
  echo "==> 创建常驻容器 $CONTAINER_NAME ..."
  docker run -d --name "$CONTAINER_NAME" \
    -v "$ROOT_DIR":/work -w /work \
    "$IMAGE_NAME" sleep infinity
fi

# 已停止则启动
docker start "$CONTAINER_NAME" >/dev/null 2>&1 || true

# 进入
echo "==> 进入 ${CONTAINER_NAME}（退出按 Ctrl+D 或 exit，容器会保留）"
docker exec -it "$CONTAINER_NAME" bash
