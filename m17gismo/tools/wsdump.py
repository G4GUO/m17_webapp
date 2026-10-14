#!/usr/bin/env python3
"""Print what a browser is really sent, and why it may reject it.

Connects to the station's WebSocket, prints the first state message whole (or a
window around the point where JSON parsing fails), and counts the messages.
Useful when the page sits frozen: a state message that does not parse is
ignored by the browser, and this shows exactly where it breaks.

    tools/wsdump.py                    # first state message
    tools/wsdump.py --port 8086        # another instance
    tools/wsdump.py --all              # print every message, briefly
"""
import argparse
import base64
import json
import os
import socket
import sys
import time

OP_TEXT, OP_CLOSE, OP_PING, OP_PONG = 1, 8, 9, 10


def connect(host, port, path="/ws", timeout=10):
    s = socket.create_connection((host, port), timeout=timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n" % (path, host, port, key)).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = s.recv(4096)
        if not d:
            raise EOFError("connection closed during the handshake")
        buf += d
    head, rest = buf.split(b"\r\n\r\n", 1)
    first = head.split(b"\r\n")[0].decode(errors="replace")
    if "101" not in first:
        raise RuntimeError("server refused the upgrade: %s" % first)
    return s, rest


def frames(s, rest, deadline):
    """Yield (opcode, payload) until the deadline passes."""
    buf = rest

    def want(n):
        nonlocal buf
        while len(buf) < n:
            d = s.recv(65536)
            if not d:
                raise EOFError
            buf += d
        out, buf = buf[:n], buf[n:]
        return out

    while time.time() < deadline:
        s.settimeout(max(0.5, deadline - time.time()))
        try:
            h = want(2)
        except (EOFError, socket.timeout, OSError):
            return
        op = h[0] & 0x0F
        ln = h[1] & 0x7F
        masked = h[1] & 0x80
        if ln == 126:
            ln = int.from_bytes(want(2), "big")
        elif ln == 127:
            ln = int.from_bytes(want(8), "big")
        mask = want(4) if masked else None
        data = want(ln)
        if mask:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        yield op, data


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--all", action="store_true", help="print everything, briefly")
    args = ap.parse_args()

    s, rest = connect(args.host, args.port)
    print("connected to ws://%s:%d/ws" % (args.host, args.port))
    counts, seen_state, bad = {}, 0, 0
    for op, data in frames(s, rest, time.time() + args.seconds):
        if op == OP_CLOSE:
            print("server closed the connection")
            break
        if op == OP_PING:
            continue
        if op != OP_TEXT:
            counts["binary"] = counts.get("binary", 0) + 1
            continue
        text = data.decode("utf-8", "replace")
        try:
            msg = json.loads(text)
            counts[msg.get("t", "?")] = counts.get(msg.get("t", "?"), 0) + 1
        except Exception as e:
            bad += 1
            counts["bad-json"] = counts.get("bad-json", 0) + 1
            print("BAD JSON (%d bytes): %s" % (len(text), e))
            pos = getattr(e, "pos", None) or 0
            print("  ...%s..." % text[max(0, pos - 160):pos + 160].replace("\n", "\\n"))
            continue
        if args.all:
            print("[%s] %s" % (msg.get("t"), text[:150]))
        elif msg.get("t") == "state":
            seen_state += 1
            if seen_state == 1:
                print("first state message: %d bytes" % len(text))
                print("  " + text)
    print("counts:", counts)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
