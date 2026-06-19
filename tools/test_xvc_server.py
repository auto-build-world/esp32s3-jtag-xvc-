#!/usr/bin/env python3
"""Minimal XVC test server — just listens and prints what Vivado sends."""
import socket

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', 2542))
s.listen(1)
print('Listening on 127.0.0.1:2542 ...')
print('Now go to Vivado -> Add Virtual Cable -> 127.0.0.1:2542')
conn, addr = s.accept()
print('Connected from', addr)
conn.settimeout(10)
try:
    data = conn.recv(1024)
    print('RECEIVED:', repr(data))
    if data:
        conn.sendall(b'xvcServer_v1.0:2048\n')
        print('Response sent')
        import time
        time.sleep(0.5)
        data2 = conn.recv(1024)
        print('RECEIVED2:', repr(data2))
except socket.timeout:
    print('TIMEOUT - no data received from Vivado')
except Exception as e:
    print('Error:', e)
finally:
    conn.close()
    s.close()
    print('Done')
