#!/usr/bin/env python3
"""
xfer_server.py - tiny file server for the Pico-modem + retro-laptop setup.

Run this on your modern PC. The laptop "dials" it through the Pico
(ATDT <pc-ip>:2323), then uses simple text commands followed by XMODEM-CRC
transfers (the classic 1977 file transfer protocol every old terminal speaks).

Commands the laptop can send (each ends with a carriage return):
    LIST                 -> lines of "name size", then a line with just "."
    GET name             -> "OK size", then the server sends via XMODEM-CRC
    PUT name size        -> "OK", then the server receives via XMODEM-CRC
    QUIT                 -> server hangs up

Safety: only files directly inside the share folder are visible, names with
path tricks are refused, and PUT never overwrites an existing file. There is
NO password, so run it on a trusted home network only.

Usage:   python3 xfer_server.py
Options: XFER_DIR=/some/folder XFER_PORT=2323 python3 xfer_server.py
"""
import os
import socket
import threading
import time

# XMODEM control bytes (ASCII control characters from the 1970s)
SOH = 0x01   # "Start Of Header": begins a 128-byte block
EOT = 0x04   # "End Of Transmission": no more blocks
ACK = 0x06   # "Acknowledge": block received fine
NAK = 0x15   # "Negative Acknowledge": block was bad, resend it
CAN = 0x18   # "Cancel": abort the transfer
CRC_REQ = ord("C")   # receiver sends 'C' to request the CRC variant
PAD = 0x1A   # Ctrl-Z: classic filler for the last short block

SHARE_DIR = os.path.abspath(os.environ.get("XFER_DIR", "share"))
PORT = int(os.environ.get("XFER_PORT", "2323"))
MAX_PUT = 16 * 1024 * 1024   # refuse uploads bigger than 16 MB


class Closed(Exception):
    """The laptop hung up."""


class Aborted(Exception):
    """A transfer failed or was cancelled."""


def crc16(data):
    """CRC-16/XMODEM: polynomial 0x1021, starting value 0."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
            crc &= 0xFFFF
    return crc


def recv_exact(conn, n, timeout):
    """Read exactly n bytes, or return None if the timeout hits first."""
    conn.settimeout(timeout)
    buf = b""
    while len(buf) < n:
        try:
            chunk = conn.recv(n - len(buf))
        except socket.timeout:
            return None
        if not chunk:
            raise Closed()
        buf += chunk
    return buf


def drain(conn):
    """Throw away anything in flight until the line has been quiet 0.3 s."""
    conn.settimeout(0.3)
    try:
        while True:
            if not conn.recv(1024):
                raise Closed()
    except socket.timeout:
        pass


def wait_response(conn, timeout):
    """Wait for ACK/NAK/CAN, ignoring stray bytes such as repeated 'C's."""
    deadline = time.time() + timeout
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            return None
        r = recv_exact(conn, 1, remaining)
        if r is None:
            return None
        if r[0] in (ACK, NAK, CAN):
            return r[0]


def xmodem_send(conn, data):
    """Send `data` to the laptop using XMODEM-CRC."""
    deadline = time.time() + 90                 # give the laptop 90 s to get ready
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            raise Aborted("receiver never started")
        b = recv_exact(conn, 1, min(remaining, 5))
        if b is None:
            continue
        if b[0] == CRC_REQ:
            break
        if b[0] == CAN:
            raise Aborted("cancelled by receiver")

    block = 1
    for off in range(0, len(data), 128):
        chunk = data[off:off + 128].ljust(128, bytes([PAD]))
        pkt = bytes([SOH, block, 255 - block]) + chunk + crc16(chunk).to_bytes(2, "big")
        for _attempt in range(10):
            conn.sendall(pkt)
            r = wait_response(conn, 10)
            if r == ACK:
                break
            if r == CAN:
                raise Aborted("cancelled by receiver")
        else:
            raise Aborted("too many retries")
        block = (block + 1) & 0xFF              # block numbers wrap 255 -> 0

    for _attempt in range(10):
        conn.sendall(bytes([EOT]))
        if wait_response(conn, 5) == ACK:
            return
    raise Aborted("no ACK for EOT")


def xmodem_recv(conn, max_size):
    """Receive a file from the laptop using XMODEM-CRC; returns its bytes."""
    first = None
    for _ in range(15):                          # poke the sender with 'C' every 3 s
        conn.sendall(bytes([CRC_REQ]))
        r = recv_exact(conn, 1, 3)
        if r is not None:
            first = r[0]
            break
    if first is None:
        raise Aborted("sender never started")

    out = bytearray()
    expected = 1
    errors = 0
    b = first
    while True:
        if b == EOT:
            conn.sendall(bytes([ACK]))
            return bytes(out)
        if b == CAN:
            raise Aborted("cancelled by sender")
        if b == SOH:
            rest = recv_exact(conn, 2 + 128 + 2, 3)
            valid = False
            if rest is not None:
                blk, cpl = rest[0], rest[1]
                data = rest[2:130]
                crc = int.from_bytes(rest[130:132], "big")
                valid = (blk + cpl == 255) and (crc16(data) == crc)
            if valid and blk == expected:
                out += data
                expected = (expected + 1) & 0xFF
                errors = 0
                conn.sendall(bytes([ACK]))
                if len(out) > max_size + 128:
                    conn.sendall(bytes([CAN, CAN]))
                    raise Aborted("file larger than announced")
            elif valid and blk == ((expected - 1) & 0xFF):
                conn.sendall(bytes([ACK]))       # duplicate: our ACK got lost, say it again
            else:
                errors += 1
                drain(conn)
                conn.sendall(bytes([NAK]))
        else:                                    # line noise or timeout
            errors += 1
            drain(conn)
            conn.sendall(bytes([NAK]))
        if errors > 10:
            conn.sendall(bytes([CAN, CAN]))
            raise Aborted("too many errors")
        r = recv_exact(conn, 1, 10)
        b = r[0] if r else None


def send_line(conn, text):
    conn.sendall(text.encode("latin-1") + b"\r\n")


def read_line(conn, timeout=600):
    """Read one command line (ends at CR or LF); blank lines are skipped."""
    line = bytearray()
    conn.settimeout(timeout)
    while True:
        try:
            ch = conn.recv(1)
        except socket.timeout:
            raise Closed()
        if not ch:
            raise Closed()
        if ch in (b"\r", b"\n"):
            if line:
                return line.decode("latin-1")
            continue
        line += ch
        if len(line) > 200:
            raise Closed()


def safe_name(name):
    """Only plain file names directly inside the share folder are allowed."""
    if not name or name.startswith(".") or len(name) > 100:
        return None
    if any(c in name for c in '/\\:\0'):
        return None
    return name


def handle(conn, addr):
    print(f"[{addr[0]}] connected")
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # don't delay tiny ACK bytes
    try:
        send_line(conn, "XFER 1 READY")
        while True:
            parts = read_line(conn).split()
            if not parts:
                continue
            cmd = parts[0].upper()

            if cmd == "LIST":
                for name in sorted(os.listdir(SHARE_DIR)):
                    path = os.path.join(SHARE_DIR, name)
                    if safe_name(name) and os.path.isfile(path):
                        send_line(conn, f"{name} {os.path.getsize(path)}")
                send_line(conn, ".")

            elif cmd == "GET" and len(parts) == 2:
                name = safe_name(parts[1])
                path = os.path.join(SHARE_DIR, name) if name else None
                if not path or not os.path.isfile(path):
                    send_line(conn, "ERR no such file")
                    continue
                with open(path, "rb") as f:
                    data = f.read()
                print(f"[{addr[0]}] GET {name} ({len(data)} bytes)")
                send_line(conn, f"OK {len(data)}")
                try:
                    xmodem_send(conn, data)
                    print(f"[{addr[0]}] sent {name}")
                except Aborted as e:
                    print(f"[{addr[0]}] GET failed: {e}")

            elif cmd == "PUT" and len(parts) == 3:
                name = safe_name(parts[1])
                try:
                    size = int(parts[2])
                except ValueError:
                    size = -1
                if not name or size < 0 or size > MAX_PUT:
                    send_line(conn, "ERR bad name or size")
                    continue
                path = os.path.join(SHARE_DIR, name)
                if os.path.exists(path):
                    send_line(conn, "ERR file exists")
                    continue
                print(f"[{addr[0]}] PUT {name} ({size} bytes)")
                send_line(conn, "OK")
                try:
                    data = xmodem_recv(conn, size)
                except Aborted as e:
                    print(f"[{addr[0]}] PUT failed: {e}")
                    continue
                if len(data) < size:
                    send_line(conn, "ERR short file")
                    continue
                tmp = path + ".part"
                with open(tmp, "wb") as f:
                    f.write(data[:size])         # cut off the Ctrl-Z padding
                os.replace(tmp, path)
                send_line(conn, f"OK saved {size} bytes")
                print(f"[{addr[0]}] stored {name}")

            elif cmd == "QUIT":
                break
            else:
                send_line(conn, "ERR unknown command")
    except (Closed, ConnectionError, OSError):
        pass
    finally:
        conn.close()
        print(f"[{addr[0]}] disconnected")


def lan_ip():
    """Best-effort guess at this PC's LAN address, to show on screen."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))         # no packets are actually sent
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    os.makedirs(SHARE_DIR, exist_ok=True)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(1)
    print(f"Sharing {SHARE_DIR}")
    print(f"On the laptop, dial:  ATDT {lan_ip()}:{PORT}")
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    main()
