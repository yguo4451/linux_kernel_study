#!/usr/bin/env python3
"""通过宿主机 2323 端口（QEMU hostfwd -> guest telnetd:23）执行 guest 命令。

用法: python3 scripts/guest_exec.py "命令"
每次调用建立一个新的 telnet 会话（-l /bin/sh），无状态。
"""
import socket
import sys
import time

PORT = 2323


def strip_iac(raw: bytes) -> bytes:
    """去掉 telnet 协商字节：0xff 后跟 2 字节（0xff 0xff 则是字面 0xff）"""
    out = bytearray()
    i = 0
    while i < len(raw):
        if raw[i] == 0xff:
            if i + 1 < len(raw) and raw[i + 1] == 0xff:
                out.append(0xff)
                i += 2
            else:
                i += 3
        else:
            out.append(raw[i])
            i += 1
    return bytes(out)


def read_until_idle(s, idle=0.5, timeout=0.3):
    """读到一段时间无新数据为止"""
    s.settimeout(timeout)
    buf = b""
    last = time.time()
    while True:
        try:
            d = s.recv(8192)
            if not d:
                break
            buf += d
            last = time.time()
        except socket.timeout:
            if time.time() - last > idle:
                break
    return buf


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "echo hello"
    # 第二个参数：发送命令后等待多少秒再读结果（默认 0.5）
    idle = float(sys.argv[2]) if len(sys.argv) > 2 else 0.5
    s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    # 读掉初始 telnet 协商
    read_until_idle(s, idle=0.3)
    s.sendall((cmd + "\n").encode())
    resp = read_until_idle(s, idle=idle)
    s.close()
    print(strip_iac(resp).decode(errors="replace"), end="")


if __name__ == "__main__":
    main()
