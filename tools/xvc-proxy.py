#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xvc-proxypy — Full PC-side Xilinx XVC protocol server bridging to ESP32 UART-JTAG adapter.

Listens on TCP 127.0.0.1:2542 for Vivado's "Add Virtual Cable" connection.
Implements the Xilinx XVC protocol (getinfo, settck, shift) on the PC side,
forwarding only raw JTAG shift data to the ESP32 over USB serial.

UART binary protocol (PC -> ESP32):
    'I' (0x49)           — initialize JTAG (ESP32 responds with 0x06 ACK)
    'S' (0x53) + ...     — shift command (n_bits u16 LE + TMS + TDI bytes)
                           ESP32 responds with framed TDO:
                             0x54 + nbytes(u16 LE) + TDO data

Usage:
    python xvc-proxypy [--port PORT] [--baud BAUD] [--listen PORT] [--list]
"""

import argparse
import logging
import struct
import socket
import sys
import time

import serial
import serial.tools.list_ports

logger = logging.getLogger("xvc-proxy")

DEFAULT_TCP_PORT = 2542
DEFAULT_BAUD = 115200
MAX_BITS = 16384  # Upper bound; actual limit from getinfo=2048 → 8192 bits per shift
MAX_BYTES = MAX_BITS // 8


def find_esp32_port():
    ports = serial.tools.list_ports.comports()
    candidates = []
    for p in ports:
        vid = p.vid or 0
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if vid == 0x303a:
            candidates.append(p.device)
            continue
        if vid == 0x10c4 or "10c4" in hwid:
            candidates.append(p.device)
            continue
        if vid == 0x1a86 or "1a86" in hwid:
            candidates.append(p.device)
            continue
        if "usb serial" in desc or "jtag" in desc:
            candidates.append(p.device)
            continue
        if "cp210" in desc or "ch34" in desc or "silicon labs" in desc:
            candidates.append(p.device)
            continue
    if not candidates:
        return None
    if len(candidates) > 1:
        logger.info("Multiple candidates: %s - using first: %s", candidates, candidates[0])
    return candidates[0]


def interactive_port_select():
    """List available serial ports and let user choose interactively."""
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        logger.error("No serial ports found.")
        sys.exit(1)
    if len(ports) == 1:
        logger.info("Single port detected: %s - %s", ports[0].device, ports[0].description)
        return ports[0].device
    print("\nAvailable serial ports:")
    for i, p in enumerate(ports, 1):
        vid_str = "VID=%04x" % (p.vid or 0)
        print("  [%d] %s - %s (%s)" % (i, p.device, p.description, vid_str))
    while True:
        try:
            choice = input("\nSelect port number, or 'q' to quit: ").strip().lower()
        except (EOFError, KeyboardInterrupt):
            print()
            sys.exit(1)
        if choice == "q":
            sys.exit(0)
        try:
            idx = int(choice)
            if 1 <= idx <= len(ports):
                return ports[idx - 1].device
            print("Invalid selection: enter a number between 1 and %d" % len(ports))
        except ValueError:
            print("Invalid input: enter a number or 'q'")


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        try:
            chunk = sock.recv(n - len(buf))
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError, OSError):
            raise EOFError
        if not chunk:
            raise EOFError
        buf.extend(chunk)
    return bytes(buf)


def send_all(sock, data):
    mv = memoryview(data)
    while mv:
        try:
            sent = sock.send(mv)
        except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError, OSError):
            raise EOFError
        if sent == 0:
            raise EOFError
        mv = mv[sent:]


def xvc_handle_getinfo(conn):
    try:
        recv_exact(conn, 6)
    except EOFError:
        return
    logger.debug("XVC: getinfo")
    # Match ESP32's actual buffer capacity (2048 bytes = 1024 TMS + 1024 TDI = 8192 bits).
    # Using a larger value causes Vivado to send chunks that exceed the ESP32's
    # USB Serial/JTAG FIFO depth (2048 bytes), risking partial-read timeouts.
    send_all(conn, b"xvcServer_v1.0:2048\n")


def xvc_handle_settck(conn):
    try:
        data = recv_exact(conn, 9)
    except EOFError:
        return
    period_bytes = data[5:9]
    logger.debug("XVC: settck (echoing 0x%s)", period_bytes.hex())
    send_all(conn, period_bytes)


def xvc_handle_shift(conn, ser):
    try:
        padding = recv_exact(conn, 4)
    except EOFError:
        return
    logger.debug("XVC: shift padding: %s", padding)
    try:
        nbits_bytes = recv_exact(conn, 4)
    except EOFError:
        return
    nbits = struct.unpack("<I", nbits_bytes)[0]
    nr_bytes = (nbits + 7) // 8
    if nbits < 1 or nr_bytes * 2 > MAX_BYTES * 2:
        logger.debug("XVC: shift invalid bit count: %d (max %d)", nbits, MAX_BITS)
        try:
            if nr_bytes > 0:
                recv_exact(conn, min(nr_bytes * 2, 65536))
        except EOFError:
            return
        send_all(conn, b'\x00' * nr_bytes)
        return
    try:
        tms_tdi = recv_exact(conn, nr_bytes * 2)
    except EOFError:
        return
    tms = tms_tdi[:nr_bytes]
    tdi = tms_tdi[nr_bytes:]
    logger.debug("XVC: shift nbits=%d nr_bytes=%d", nbits, nr_bytes)
    tdo = uart_shift(ser, nbits, tms, tdi)
    if tdo is None:
        logger.warning("XVC: shift - ESP32 not responding, sending zeros")
        tdo = b'\x00' * nr_bytes
    send_all(conn, tdo)


def uart_init(ser):
    """Send JTAG init ('I') to ESP32. Returns True on ACK, False otherwise."""
    try:
        logger.info("Sending JTAG init ('I') to ESP32...")
        ser.reset_input_buffer()
        ser.write(b'I')
        ser.flush()
        old_timeout = ser.timeout
        ser.timeout = 2.0
        ack = ser.read(1)
        ser.timeout = old_timeout
        if ack == b'\x06':
            logger.info("JTAG init OK (ACK received)")
            return True
        else:
            logger.warning("JTAG init: got 0x%s instead of ACK", ack.hex() if ack else "(none)")
            return False
    except serial.SerialTimeoutException:
        logger.warning("JTAG init write timed out (ESP32 may not be ready)")
        return False
    except serial.SerialException as e:
        logger.warning("JTAG init failed: %s", e)
        return False


def uart_shift(ser, nbits, tms, tdi):
    assert 1 <= nbits <= MAX_BITS, f"nbits {nbits} out of range"
    nr_bytes = (nbits + 7) // 8
    assert len(tms) == nr_bytes and len(tdi) == nr_bytes
    frame = bytearray()
    frame.append(0x53)
    frame.extend(struct.pack("<H", nbits))
    frame.extend(tms)
    frame.extend(tdi)
    logger.debug("UART shift: writing %d bytes (nbits=%d)", len(frame), nbits)
    try:
        ser.write(frame)
        ser.flush()
        logger.debug("UART shift: write OK")
    except (serial.SerialTimeoutException, serial.SerialException, OSError) as e:
        logger.warning("UART shift: write failed: %s", e)
        return None
    # Read framed response: 0x54 + len(u16 LE) + TDO
    logger.debug("UART shift: reading %d TDO bytes...", nr_bytes)
    # Read framed response: 2-byte length (LE) + TDO
    ser.timeout = 30.0
    tdo = _read_length_prefixed(ser)
    if tdo is None:
        return None
    if nbits >= 32 and len(tdo) >= 4:
        logger.debug("XVC: shift TDO[0:4]=%s (IDCODE)", tdo[:4].hex())
    return bytes(tdo)


def _read_length_prefixed(ser):
    """Read 2-byte LE length + TDO. Auto-resyncs on invalid length."""
    raw = b''
    while len(raw) < 2:
        b = ser.read(2 - len(raw))
        if not b:
            return None
        raw += b
    frame_len = struct.unpack("<H", raw)[0]
    if 1 <= frame_len <= MAX_BYTES:
        tdo = ser.read(frame_len)
        if len(tdo) == frame_len:
            return tdo
        return None
    # Invalid length — search for valid frame by consuming one byte at a time
    logger.warning("UART shift: bad length %d, searching for next valid frame", frame_len)
    buf = bytearray(raw)
    for _ in range(2000):
        b = ser.read(1)
        if not b:
            break
        buf.append(b[0])
        if len(buf) >= 2:
            maybe_len = struct.unpack("<H", bytes(buf[-2:]))[0]
            if 1 <= maybe_len <= MAX_BYTES:
                logger.debug("UART shift: resynced after discarding %d bytes", len(buf) - 2)
                frame_len = maybe_len
                tdo = ser.read(frame_len)
                if len(tdo) == frame_len:
                    return tdo
                return None
    logger.warning("UART shift: resync failed")
    return None



def handle_vivado_connection(conn, addr, ser):
    logger.info("Vivado connected from %s:%d", addr[0], addr[1])
    # Re-init before each Vivado connection to catch ESP32 after a reset
    if not uart_init(ser):
        logger.warning("JTAG init failed — continuing anyway (ESP32 may reboot or reinit)")
    # Drain stale USB bytes that may have accumulated before this connection
    time.sleep(0.05)
    if ser.in_waiting:
        drained = ser.read(ser.in_waiting)
        logger.debug("Drained %d stale USB byte(s): %s", len(drained), drained[:32].hex())
    try:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError:
        pass
    try:
        while True:
            try:
                prefix = recv_exact(conn, 2)
            except EOFError:
                logger.info("Vivado disconnected from %s:%d", addr[0], addr[1])
                break
            cmd = prefix.decode("ascii", errors="replace")
            logger.debug("XVC raw prefix: 0x%s = '%s'", prefix.hex(), cmd)
            if cmd == "ge":
                xvc_handle_getinfo(conn)
            elif cmd == "se":
                xvc_handle_settck(conn)
            elif cmd == "sh":
                xvc_handle_shift(conn, ser)
            else:
                logger.warning("Unknown XVC command prefix: 0x%s", prefix.hex())
                break
    except (socket.error, socket.timeout, ConnectionResetError, ConnectionAbortedError,
            BrokenPipeError, OSError, EOFError) as exc:
        if isinstance(exc, socket.timeout):
            logger.info("Vivado connection timed out")
        else:
            logger.info("Vivado disconnected (%s:%d)", addr[0], addr[1])
    finally:
        try:
            conn.close()
        except OSError:
            pass

# ---------------------------------------------------------------------------
# USB data-path diagnostic: echo test (no JTAG involved)
# ---------------------------------------------------------------------------

def _test_echo(ser, payload):
    """Send a bytearray to ESP32 and verify it echoes back correctly.
    Returns (success, details) tuple."""
    frame = bytearray([0x55])  # custom echo command
    frame.extend(struct.pack("<H", len(payload)))
    frame.extend(payload)
    try:
        ser.reset_input_buffer()
        ser.write(frame)
        ser.flush()
        old_to = ser.timeout
        ser.timeout = 5.0
        raw = ser.read(2)
        if len(raw) < 2:
            ser.timeout = old_to
            return False, "no length prefix"
        reply_len = struct.unpack("<H", raw)[0]
        reply = ser.read(reply_len)
        ser.timeout = old_to
        if len(reply) != reply_len:
            return False, f"got {len(reply)} of {reply_len} TDO bytes"
        if reply != payload:
            mismatch = sum(1 for a, b in zip(reply, payload) if a != b)
            return False, f"data mismatch: {mismatch}/{len(payload)} bytes differ"
        return True, f"OK ({len(payload)} bytes echo matched)"
    except Exception as e:
        return False, str(e)


def run_diagnose(ser):
    """Run USB data-path diagnosis: send known patterns and verify echo."""
    import random
    logger.info("=== USB data-path diagnosis ===")
    patterns = [
        b"",
        b"\x00",
        b"\xff",
        b"\xAA",
        b"\x55",
        b"\x00\x00\x00\x00",
        b"\xff\xff\xff\xff",
        bytes(range(256)),
        bytes([0xAA, 0x55] * 32),  # 64 bytes checkerboard
        bytes(random.getrandbits(8) for _ in range(256)),
        bytes(random.getrandbits(8) for _ in range(1024)),
        bytes(random.getrandbits(8) for _ in range(2048)),
    ]
    for p in patterns:
        ok, msg = _test_echo(ser, p)
        status = "PASS" if ok else "FAIL"
        logger.info("  [%s] len=%d: %s", status, len(p), msg)
        if not ok and len(p) > 0:
            logger.warning("  → Data-path corrupted at %d bytes!", len(p))
    logger.info("=== diagnosis complete ===")


def main():
    parser = argparse.ArgumentParser(description="XVC protocol server - bridge Vivado to ESP32 UART-JTAG")
    parser.add_argument("--port", help="Serial port (auto-detect if omitted)")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="Baud rate (default: %(default)s)")
    parser.add_argument("--listen", type=int, default=DEFAULT_TCP_PORT, help="TCP listen port (default: %(default)s)")
    parser.add_argument("--list", action="store_true", help="List available serial ports and exit")
    parser.add_argument("--interactive", "-i", action="store_true", help="Force interactive serial port selection")
    parser.add_argument("--debug", action="store_true", help="Enable DEBUG-level logging")
    parser.add_argument("--diagnose", action="store_true", help="Run USB data-path echo test (no Vivado needed)")
    args = parser.parse_args()
    log_level = logging.DEBUG if args.debug else logging.INFO
    logging.basicConfig(level=log_level, format="%(asctime)s [%(levelname)s] %(message)s", datefmt="%H:%M:%S")
    if args.list:
        ports = serial.tools.list_ports.comports()
        print("Available serial ports:")
        for p in ports:
            print("  %-20s VID=%04x PID=%04x  %s" % (p.device, p.vid or 0, p.pid or 0, p.description))
        return
    if args.port:
        port = args.port
    else:
        port = interactive_port_select()
    logger.info("Opening serial port %s @ %d...", port, args.baud)
    try:
        ser = serial.Serial(port, args.baud, timeout=1.0, write_timeout=1.0)
    except serial.SerialException as e:
        logger.error("Failed to open %s: %s", port, e)
        sys.exit(1)
    logger.info("Port %s opened successfully.", port)
    uart_init(ser)

    if args.diagnose:
        run_diagnose(ser)
        ser.close()
        return

    listen_addr = ("127.0.0.1", args.listen)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(listen_addr)
    srv.listen(1)
    logger.info("Listening on TCP %s:%d - add Virtual Cable in Vivado", listen_addr[0], listen_addr[1])
    try:
        while True:
            conn, addr = srv.accept()
            handle_vivado_connection(conn, addr, ser)
    except KeyboardInterrupt:
        logger.info("Shutting down...")
    finally:
        ser.close()
        srv.close()


if __name__ == "__main__":
    main()
