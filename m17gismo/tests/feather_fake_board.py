#!/usr/bin/env python3
"""feather_fake_board.py - stand in for the Feather modem, without the hardware.

Opens a pty, prints its path, and speaks the link protocol on it: it answers
status, and it streams symbol blocks the way the board does (192 symbols every
40 ms, but only while the host holds PTT and TXEN).

    ./feather_fake_board.py --noise            # symbols that never lock
    ./feather_fake_board.py --echo             # loop its own received dibits
    FEATHER_TTY=$(./feather_fake_board.py --echo) ./m17web -r feather

--echo maps dibits to ideal symbol amplitudes, so a transmission comes straight
back as an ideal channel: the host should decode what it sent.  --noise sends
random amplitudes, which is what a quiet band looks like to the framer.

This is a development aid, not a test: the tests are in tests/feather_test.cpp.
"""

import argparse
import os
import random
import select
import struct
import sys
import time
import tty

SYNC = b"\xa5\x5a"
TX_DIBITS, TX_CONTROL, PING, TX_ABORT = 0x01, 0x02, 0x03, 0x04
RX_SYMBOLS, RX_SOFT, RX_STATUS, PONG = 0x81, 0x82, 0x83, 0x84
SYMBOLS_PER_FRAME = 192
CTL_PTT, CTL_TXEN, CTL_LOOPBACK, CTL_RX_LOCK = 1, 2, 4, 8
AMP = (0.333, 1.0, -0.333, -1.0)


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def encode(typ: int, seq: int, payload: bytes) -> bytes:
    body = SYNC + bytes([typ, seq & 0xFF]) + struct.pack("<H", len(payload)) + payload
    return body + struct.pack("<H", crc16(body))


def pack_symbols(sym) -> bytes:
    """int8 symbols with a per-block power-of-two scale, as link_pack_symbols()."""
    peak = max(abs(s) for s in sym) or 1.0
    k = 0
    while peak * (1 << (k + 1)) <= 127.0 and k < 7:
        k += 1
    scale = 1 << k
    return bytes([k]) + bytes(max(-127, min(127, round(s * scale))) & 0xFF for s in sym)


def unpack_dibits(payload: bytes, n: int) -> list:
    out = []
    for i in range(n):
        byte = payload[i // 4]
        out.append((byte >> (6 - 2 * (i % 4))) & 3)
    return out


class Board:
    def __init__(self, master, mode):
        self.master = master
        self.mode = mode
        self.buf = b""
        self.seq = 0
        self.keyed = False
        self.lock = False
        self.dibits = []
        self.symbols_sent = 0
        self.blocks_sent = 0
        self.frames_in = 0
        self.last_status = 0

    def send(self, typ, payload):
        self.seq = (self.seq + 1) & 0xFF
        try:
            os.write(self.master, encode(typ, self.seq, payload))
        except OSError:
            pass

    def feed(self, data):
        self.buf += data
        while True:
            i = self.buf.find(SYNC)
            if i < 0:
                self.buf = self.buf[-1:]
                return
            self.buf = self.buf[i:]
            if len(self.buf) < 8:
                return
            typ, _seq = self.buf[2], self.buf[3]
            ln = struct.unpack("<H", self.buf[4:6])[0]
            if len(self.buf) < 6 + ln + 2:
                return
            payload = self.buf[6:6 + ln]
            got = struct.unpack("<H", self.buf[6 + ln:8 + ln])[0]
            if crc16(self.buf[:6 + ln]) != got:
                self.buf = self.buf[2:]
                continue
            self.buf = self.buf[8 + ln:]
            self.on_frame(typ, payload)

    def on_frame(self, typ, payload):
        if typ == TX_CONTROL and payload:
            flags = payload[0]
            was = self.keyed
            self.keyed = bool(flags & CTL_PTT) and bool(flags & CTL_TXEN)
            self.lock = bool(flags & CTL_RX_LOCK)
            if self.keyed != was:
                print(f"board: {'keyed' if self.keyed else 'unkeyed'}", flush=True)
        elif typ == TX_DIBITS:
            self.frames_in += 1
            self.dibits.extend(unpack_dibits(payload, SYMBOLS_PER_FRAME))
        elif typ == PING:
            self.send(PONG, payload[:4])

    def tick(self, now):
        if now - self.last_status > 0.25:
            self.last_status = now
            st = bytes([1 if self.keyed else 0, 1, 1 if self.lock else 0, 0,
                        0, 0, 0, 0, 0, 0, 0, 0])
            self.send(RX_STATUS, st)

        # The receiver runs whether or not the transmitter does: PTT gates the DAC
        # and the radio, not the ADC.  So symbols flow continuously, and it is the
        # content that is silent when nothing is coming back.
        if self.mode == "echo" and len(self.dibits) >= SYMBOLS_PER_FRAME:
            block, self.dibits = self.dibits[:SYMBOLS_PER_FRAME], self.dibits[SYMBOLS_PER_FRAME:]
            sym = [AMP[d] for d in block]
            self.symbols_sent += SYMBOLS_PER_FRAME
        elif self.mode == "echo":
            sym = [0.0] * SYMBOLS_PER_FRAME
        else:
            # Quiet band: the discriminator output with nothing on it.
            sym = [random.uniform(-0.25, 0.25) for _ in range(SYMBOLS_PER_FRAME)]
        self.send(RX_SYMBOLS, pack_symbols(sym))
        self.blocks_sent += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--echo", action="store_true", help="loop dibits back as ideal symbols")
    ap.add_argument("--noise", action="store_true", help="random symbols (never locks)")
    ap.add_argument("--seconds", type=float, default=0, help="exit after this long")
    args = ap.parse_args()
    mode = "echo" if args.echo else "noise"

    master, slave = os.openpty()
    name = os.ttyname(slave)
    tty.setraw(slave)
    print(name, flush=True)

    board = Board(master, mode)
    start = time.time()
    next_block = start
    while True:
        now = time.time()
        if args.seconds and now - start > args.seconds:
            print(f"board: {board.frames_in} dibit frames in, "
                  f"{board.blocks_sent} symbol blocks out "
                  f"({board.symbols_sent} echoing)", flush=True)
            return
        r, _, _ = select.select([master], [], [], 0.005)
        if r:
            try:
                board.feed(os.read(master, 4096))
            except OSError:
                return
        if now >= next_block:
            board.tick(now)
            next_block += 0.040


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
