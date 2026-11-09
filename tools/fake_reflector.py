#!/usr/bin/env python3
"""A stand-in reflector: say ACKN, keep the link alive with PINGs, then vanish.

Used to test what m17web does when a reflector stops answering without saying
goodbye.  A live M17 reflector pings every few seconds, so silence is the only
signal a client gets.
"""
import socket, sys, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 17099
ALIVE_FOR = float(sys.argv[2]) if len(sys.argv) > 2 else 24.0
PING_EVERY = 2.0
CALL = bytes([ord('G'), ord('4'), ord('G'), ord('U'), ord('O'), 0x00])

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", PORT))
print(f"fake reflector on {PORT}, alive for {ALIVE_FOR}s", flush=True)

peer = None
t0 = time.time()
deadline = None
s.settimeout(0.5)

while True:
    now = time.time()
    if deadline is None and now - t0 > 180:
        print("no connect within 120s, giving up", flush=True)
        break
    if deadline is not None and now > deadline:
        print(f"{now - t0:6.1f}s  going silent, no DISC sent", flush=True)
        break
    try:
        data, addr = s.recvfrom(2048)
    except socket.timeout:
        data = None
    except KeyboardInterrupt:
        break
    if data:
        tag = data[:4].decode("latin-1", "replace")
        print(f"{time.time() - t0:6.1f}s  <- {len(data)}B {tag!r} from {addr[0]}:{addr[1]}", flush=True)
        if tag == "CONN" and peer is None:
            peer = addr
            s.sendto(b"ACKN", peer)
            print(f"{time.time() - t0:6.1f}s  -> ACKN", flush=True)
            deadline = time.time() + ALIVE_FOR
            next_ping = time.time() + PING_EVERY
            continue
    if peer is not None and time.time() >= next_ping:
        s.sendto(b"PING" + CALL, peer)
        print(f"{time.time() - t0:6.1f}s  -> PING", flush=True)
        next_ping = time.time() + PING_EVERY
