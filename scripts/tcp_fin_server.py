#!/usr/bin/env python3
"""制造"标准四次挥手"的 TCP server：
收到 client 数据后故意 sleep 3 秒再 close，
让内核先自动回 ACK（应用未 close），3 秒后才发 FIN。"""
import socket
import time

PORT = 12346

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", PORT))
s.listen(1)
print(f"[server] listening on :{PORT}", flush=True)

conn, addr = s.accept()
print(f"[server] accepted from {addr}", flush=True)

data = conn.recv(1024)
print(f"[server] recv {len(data)}B, holding connection 3s (不 close，内核会先回 ACK)", flush=True)
time.sleep(3)

conn.close()
s.close()
print("[server] closed -> 现在才发 FIN", flush=True)
