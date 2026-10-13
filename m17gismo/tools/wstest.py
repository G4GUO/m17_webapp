#!/usr/bin/env python3
"""Headless WebSocket test client for m17web (standard library only).

Examples
--------
# connect, print the first few messages, send some commands
python3 tools/wstest.py --cmd "rf 146400000" --cmd "sa G4GUO" --steps 6

# hold PTT for 3 s while playing a raw 8 kHz s16le file into the mic channel
python3 tools/wstest.py --ptt --wav-in test.raw --wav-out decoded.raw --duration 4

# dump the final state JSON (handy for assertions)
python3 tools/wstest.py --steps 3 --state-out state.json
"""
import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time

TAG_MIC = 0x01
TAG_SPK = 0x02
RATE = 8000


class WSClient:
    def __init__(self, host, port, path="/ws", timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("server closed the connection during handshake")
            buf += chunk
        head, _, rest = buf.partition(b"\r\n\r\n")
        if b"101" not in head.split(b"\r\n")[0]:
            raise RuntimeError("handshake failed: " + head.decode(errors="replace"))
        self.rx = rest
        self.sock.settimeout(0.5)

    # -- framing ------------------------------------------------------------
    def _frame(self, opcode, payload):
        header = bytearray([0x80 | opcode])
        n = len(payload)
        mask = os.urandom(4)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", n)
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return bytes(header) + masked

    def send_text(self, text):
        self.sock.sendall(self._frame(0x1, text.encode()))

    def send_bin(self, data):
        self.sock.sendall(self._frame(0x2, bytes(data)))

    def recv(self):
        """Returns (opcode, payload) or None on timeout."""
        while True:
            frame = self._try_parse()
            if frame:
                return frame
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                return None
            except OSError:
                return None
            if not chunk:
                return None
            self.rx += chunk

    def _try_parse(self):
        b = self.rx
        if len(b) < 2:
            return None
        opcode = b[0] & 0x0F
        n = b[1] & 0x7F
        pos = 2
        if n == 126:
            if len(b) < 4:
                return None
            n = struct.unpack(">H", b[2:4])[0]
            pos = 4
        elif n == 127:
            if len(b) < 10:
                return None
            n = struct.unpack(">Q", b[2:10])[0]
            pos = 10
        if b[1] & 0x80:            # server frames must not be masked
            raise RuntimeError("unexpected masked frame from server")
        if len(b) < pos + n:
            return None
        payload = b[pos:pos + n]
        self.rx = b[pos + n:]
        return (opcode, payload)

    def close(self):
        try:
            self.sock.sendall(self._frame(0x8, b""))
        except OSError:
            pass
        self.sock.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--cmd", action="append", default=[], help="command to send (repeatable)")
    ap.add_argument("--steps", type=int, default=5, help="messages to read before sending commands")
    ap.add_argument("--duration", type=float, default=0.0, help="keep reading for N seconds")
    ap.add_argument("--ptt", action="store_true", help="send 'tx' then stream mic audio")
    ap.add_argument("--wav-in", help="raw 8 kHz s16le file played into the mic channel")
    ap.add_argument("--wav-out", help="file to write received speaker audio to")
    ap.add_argument("--state-out", help="write the last state message to this file")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    ws = WSClient(args.host, args.port)
    counts = {}
    last_state = None
    mic_in = open(args.wav_in, "rb") if args.wav_in else None
    spk_out = open(args.wav_out, "wb") if args.wav_out else None
    spk_samples = 0
    mic_samples = 0

    def note(kind):
        counts[kind] = counts.get(kind, 0) + 1

    deadline = time.time() + args.duration if args.duration else None
    pending_mic = b""

    def pump(seconds):
        nonlocal last_state, spk_samples, mic_samples, pending_mic
        end = time.time() + seconds
        mic_next = time.time()
        while time.time() < end:
            if args.ptt and mic_in:
                now = time.time()
                if now >= mic_next:
                    chunk = mic_in.read(320)          # 160 samples = 20 ms
                    if not chunk:
                        mic_in.seek(0)
                        chunk = mic_in.read(320)
                    mic_samples += len(chunk) // 2
                    ws.send_bin(bytes([TAG_MIC]) + chunk)
                    mic_next += 0.02
            frame = ws.recv()
            if not frame:
                continue
            opcode, payload = frame
            if opcode == 0x1:
                try:
                    msg = json.loads(payload.decode())
                except Exception:
                    note("bad-json")
                    continue
                note(msg.get("t", "?"))
                if msg.get("t") == "state":
                    last_state = msg
                    if not args.quiet:
                        print("[state] mode=%s ptt=%s f=%s/%s sig=%.2f frames=%s gerr=%s clients=%s" % (
                            msg.get("mode"), msg.get("ptt"), msg.get("rx_freq"), msg.get("tx_freq"),
                            msg.get("signal", 0), msg.get("frames"), msg.get("gerr"), msg.get("clients")))
                elif msg.get("t") == "log":
                    print("[log:%s] %s" % (msg.get("kind"), msg.get("line")))
                elif msg.get("t") in ("resp", "help", "hello", "refs", "clear"):
                    if msg.get("t") == "refs":
                        print("[refs] %d reflectors" % len(msg.get("names", [])))
                    elif msg.get("t") == "help":
                        print("[help] %d lines" % len(msg.get("lines", [])))
                    elif msg.get("t") == "hello":
                        print("[hello] %s" % msg)
                    else:
                        print("[%s] %s" % (msg.get("t"), msg.get("line")))
            elif opcode == 0x2:
                if payload and payload[0] == TAG_SPK:
                    pcm = payload[1:]
                    spk_samples += len(pcm) // 2
                    if spk_out:
                        spk_out.write(pcm)
                    note("audio-in")
                else:
                    note("binary-other")
            elif opcode == 0x8:
                print("[close] server closed the connection")
                break

    print(f"connected to ws://{args.host}:{args.port}/ws")
    pump(1.5)
    for c in args.cmd:
        print(f"[send] {c}")
        ws.send_text(c)
        pump(0.35)
    if args.ptt:
        print("[send] tx (PTT on)")
        ws.send_text("tx")
        pump(0.5)
        pump(max(args.duration, 3.0))
        print("[send] rx (PTT off)")
        ws.send_text("rx")
        pump(0.5)
    if deadline and time.time() < deadline:
        pump(deadline - time.time())
    elif args.duration and not args.ptt:
        pump(args.duration)

    if args.state_out and last_state:
        with open(args.state_out, "w") as fh:
            json.dump(last_state, fh, indent=2)

    print("\nmessage counts:", counts)
    print(f"mic samples sent: {mic_samples}   speaker samples received: {spk_samples}")
    if spk_out:
        spk_out.close()
    if mic_in:
        mic_in.close()
    ws.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
