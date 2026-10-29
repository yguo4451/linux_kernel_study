# 构建环境：Ubuntu 22.04 + Linux 内核编译依赖
# 用途：Apple Silicon 上 Apple clang 无法编译内核，故在 Linux 容器内用 GNU 工具链原生编译 arm64 内核
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive

# 使用阿里云 apt 镜像加速（国内网络）
RUN sed -i 's@//archive.ubuntu.com@//mirrors.aliyun.com@g; s@//security.ubuntu.com@//mirrors.aliyun.com@g' /etc/apt/sources.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential flex bison bc libelf-dev libssl-dev \
      cpio wget xz-utils ca-certificates vim rsync \
      libnuma-dev python3 python3-dev e2fsprogs \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
