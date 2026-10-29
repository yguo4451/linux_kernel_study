#!/usr/bin/env python3
"""端到端验证：宿主机 2323 -> guest telnetd(23) 的会话可达。
过滤 telnet 协商字节(IAC 0xff 开头三字节)，发送命令读响应。"""
import socket
import time

def read_until_idle(s, idle=0.4):
    """读到一段时间无新数据为止，返回原始字节"""
    s.settimeout(0.3)
    buf = b""
    last = time.time()
    while True:
        try:
            d = s.recv(4096)
            if not d:
                break
            buf += d
            last = time.time()
        except socket.timeout:
            if time.time() - last > idle:
                break
    return buf

def strip_iac(raw: bytes) -> bytes:
    """去掉 telnet 协商字节：0xff 后跟 2 字节（或 0xff 0xff 两字节）"""
    out = bytearray()
    i = 0
    while i < len(raw):
        if raw[i] == 0xff:
            if i + 1 < len(raw) and raw[i + 1] == 0xff:
                out.append(0xff)
                i += 2
            else:
                i += 3  # IAC + cmd + opt
        else:
            out.append(raw[i])
            i += 1
    return bytes(out)

s = socket.create_connection(("127.0.0.1", 2323), timeout=5)
print("[client] 已连接 127.0.0.1:2323")

# 1. 读掉初始 telnet 协商
initial = read_until_idle(s)
print(f"[client] 初始协商 {len(initial)}B: {initial.hex()}")

# 2. 发命令
s.sendall(b"hostname\r\n")
s.sendall(b"echo GUEST_ALIVE\r\n")
resp = read_until_idle(s)

clean = strip_iac(resp).decode(errors="replace")
print("=" * 40)
print(clean)
print("=" * 40)
s.close()
