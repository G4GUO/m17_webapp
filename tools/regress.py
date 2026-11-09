#!/usr/bin/env python3
"""Regression tests for an m17web station.

Answers one question: after a change, is this radio still a working station?
Runs on the station itself (it needs the clone, the binary, the service and
localhost:8080) and touches nothing: no PTT, no transmit, no file writes
outside a temporary directory.

    tools/regress.py                  # the fast checks, about 15 s
    tools/regress.py --deep           # adds the sim/fake-reflector tests, ~90 s
    tools/regress.py --json out.json  # machine-readable result
    tools/regress.py --list           # what each check proves

Exit status is 0 only when every check passes.  Warnings do not fail the run,
but they are printed with the reason, because a station can pass every check
while its GPS is dead or its sound card has vanished.

Written with the standard library only, on purpose: the station may be a fresh
Pi with nothing installed but python3 and the toolchain.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

OK, FAIL, WARN, SKIP = "ok", "FAIL", "warn", "skip"
RESULTS = []


def record(state, name, detail=""):
    RESULTS.append({"state": state, "name": name, "detail": detail})
    tag = {OK: "  ok  ", FAIL: "  FAIL", WARN: "  warn", SKIP: "  skip"}[state]
    print(f"{tag}  {name:<28} {detail}")
    return state == OK


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, cwd=None, timeout=30):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout.strip(), p.stderr.strip()
    except FileNotFoundError:
        return 127, "", f"{cmd[0]}: not found"
    except subprocess.TimeoutExpired:
        return 124, "", "timed out"


# ---------------------------------------------------------------------------
# The deployment the station is running
# ---------------------------------------------------------------------------
def check_repo(repo):
    rc, head, _ = run(["git", "-C", repo, "rev-parse", "--short", "HEAD"])
    if rc:
        return record(FAIL, "repo/head", f"no git checkout at {repo}"), None
    rc, dirty, _ = run(["git", "-C", repo, "status", "--porcelain", "--untracked-files=no"])
    n_dirty = len([l for l in dirty.splitlines() if l.strip()])
    rc, counts, _ = run(["git", "-C", repo, "rev-list", "--left-right", "--count", "HEAD...origin/master"])
    behind = ahead = "?"
    if rc == 0 and counts:
        ahead, behind = counts.split()
    detail = f"HEAD {head}, {n_dirty} dirty, {ahead} ahead / {behind} behind origin/master"
    if n_dirty:
        return record(FAIL, "repo/clean", detail), head
    if behind != "0":
        return record(FAIL, "repo/up-to-date", detail), head
    return record(OK, "repo", detail), head


def check_www(repo, service_dir):
    src = os.path.join(repo, "m17gismo", "www")
    dst = os.path.join(service_dir, "www")
    if not os.path.isdir(src):
        return record(FAIL, "www/source", f"{src} missing")
    if not os.path.isdir(dst):
        return record(FAIL, "www/served", f"{dst} missing - the service serves from there")
    names = sorted(os.listdir(src))
    diffs = []
    for n in names:
        a, b = os.path.join(src, n), os.path.join(dst, n)
        if not os.path.isfile(a):
            continue
        if not os.path.isfile(b):
            diffs.append(f"{n} absent")
        elif md5(a) != md5(b):
            diffs.append(f"{n} differs")
    extra = [n for n in sorted(os.listdir(dst))
             if os.path.isfile(os.path.join(dst, n)) and not os.path.exists(os.path.join(src, n))]
    # Backups an operator left behind (index.html.bak-*) are expected; only flag
    # a live asset that the clone does not have.
    extra = [n for n in extra if not re.search(r"\.(bak|orig|save)", n)]
    if diffs or extra:
        return record(FAIL, "www/served-matches", "; ".join(diffs + [f"extra: {', '.join(extra)}"]))
    return record(OK, "www/served-matches", f"{len([n for n in names if os.path.isfile(os.path.join(src, n))])} files, md5 identical")


def check_assets(url_base):
    """index.html must ask for the files it ships, and the versions must be live."""
    rc, html, err = fetch(url_base + "/")
    if rc != 200:
        return record(FAIL, "page/assets", f"GET / -> {rc} {err}")
    refs = re.findall(r'"([a-z0-9_.-]+)\?v=(\d+)"', html, re.I)
    if not refs:
        return record(FAIL, "page/assets", "no ?v=N asset references in index.html")
    bad, seen = [], []
    for name, ver in refs:
        seen.append(f"{name}?v={ver}")
        code, _, _ = fetch(f"{url_base}/{name}?v={ver}")
        if code != 200:
            bad.append(f"{name}?v={ver} -> {code}")
    detail = ", ".join(seen)
    if bad:
        return record(FAIL, "page/assets", "; ".join(bad))
    return record(OK, "page/assets", detail)


def check_page_markup(url_base):
    rc, html, _ = fetch(url_base + "/")
    if rc != 200:
        return record(FAIL, "page/markup", f"GET / -> {rc}")
    wanted = ["id=\"helpcard\"", "id=\"cmdinput\"", "id=\"log\"", "id=\"smscard\"",
              "id=\"refstatus\"", "id=\"diagtoggle\"", "id=\"hwinfo\""]
    missing = [w for w in wanted if w not in html]
    if missing:
        return record(FAIL, "page/markup", "missing " + ", ".join(missing))
    return record(OK, "page/markup", f"{len(wanted)} anchors present")


def check_service(unit):
    rc, out, err = run(["systemctl", "is-active", unit])
    if rc or out != "active":
        return record(FAIL, "service", f"{unit} is {out or err}")
    rc, since, _ = run(["systemctl", "show", "-p", "ActiveEnterTimestamp", unit])
    rc2, nrestarts, _ = run(["systemctl", "show", "-p", "NRestarts", unit])
    return record(OK, "service", f"{unit} active since {since.split('=', 1)[-1]}, restarts {nrestarts.split('=', 1)[-1]}")


def check_binary(unit):
    """The running binary must be newer than every source file, or the station is
    reporting on code the repository no longer contains."""
    rc, execstart, _ = run(["systemctl", "show", "-p", "ExecStart", unit])
    m = re.search(r"path=(\S+)", execstart)
    if not m:
        return record(FAIL, "binary", "cannot read ExecStart from systemd")
    path = m.group(1)
    if not os.path.isfile(path):
        return record(FAIL, "binary", f"{path} does not exist")
    bm = os.path.getmtime(path)
    size = os.path.getsize(path)
    rc, wd, _ = run(["systemctl", "show", "-p", "WorkingDirectory", unit])
    wd = wd.split("=", 1)[-1].strip() or os.getcwd()
    # Newest tracked source, from git, so a stray untracked file cannot mask it.
    rc, files, _ = run(["git", "-C", wd, "ls-files", "m17gismo/*.c", "m17gismo/*.h",
                        "m17gismo/*.cpp", "m17gismo/makefile", "m17gismo/Makefile"], cwd=wd)
    newest, newest_name = 0.0, "?"
    for f in files.splitlines():
        p = os.path.join(wd, f)
        if os.path.isfile(p) and os.path.getmtime(p) > newest:
            newest, newest_name = os.path.getmtime(p), f
    detail = f"{os.path.basename(path)} {size} B, md5 {md5(path)[:8]}"
    if newest and bm < newest:
        return record(FAIL, "binary/current", f"{detail}; older than {newest_name} - rebuild")
    return record(OK, "binary", detail)


# ---------------------------------------------------------------------------
# The radio, over the same websocket the panel uses
# ---------------------------------------------------------------------------
class Panel:
    """Thin wrapper over the panel's websocket, returning the state messages."""

    def __init__(self, host, port, timeout=5.0, wstest_dir=None):
        # wstest.py ships with the station code in m17gismo/tools; fall back to
        # this directory so a copied-out script still works.
        for d in [wstest_dir, os.path.dirname(os.path.abspath(__file__)),
                  os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "m17gismo", "tools")]:
            if d and os.path.isfile(os.path.join(d, "wstest.py")):
                sys.path.insert(0, d)
                break
        from wstest import WSClient
        self.ws = WSClient(host, port, timeout=timeout)
        self.states, self.texts = [], []

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            frame = self.ws.recv()
            if not frame:
                continue
            opcode, payload = frame
            if opcode != 0x1:
                continue
            try:
                msg = json.loads(payload.decode())
            except Exception:
                continue
            if msg.get("t") == "state":
                self.states.append(msg)
            else:
                self.texts.append(msg)
        return self

    def send(self, text):
        self.ws.send_text(text)

    def close(self):
        self.ws.close()

    @property
    def state(self):
        return self.states[-1] if self.states else {}


def check_panel(host, port, allow_sim, expect_radio, wstest_dir=None):
    try:
        p = Panel(host, port, wstest_dir=wstest_dir).pump(3.0)
    except Exception as e:
        return record(FAIL, "panel/websocket", f"cannot open ws://{host}:{port}/ws: {e}"), None
    if not p.states:
        p.close()
        return record(FAIL, "panel/state", "connected but no state message in 3 s"), None
    st = p.state
    hello = [t for t in p.texts if t.get("t") in ("hello", "refs", "help")]
    ok = record(OK, "panel/websocket",
                f"{len(p.states)} state frames, {len(hello)} text frames, radio '{st.get('radio')}'")

    radio = (st.get("radio") or "").lower()
    if expect_radio and expect_radio != "auto" and radio != expect_radio:
        ok &= record(FAIL, "radio/route", f"expected '{expect_radio}', got '{radio}'")
    elif radio == "sim" and not allow_sim:
        ok &= record(FAIL, "radio/route", "station is running the simulator, not a radio")
    else:
        ok &= record(OK, "radio/route", radio)

    rx, tx = st.get("rx_freq"), st.get("tx_freq")
    if not rx or not tx:
        ok &= record(FAIL, "radio/frequency", f"rx {rx}, tx {tx}")
    else:
        ok &= record(OK, "radio/frequency", f"rx {rx/1e6:.4f} MHz, tx {tx/1e6:.4f} MHz")

    # A receiver that is streaming has a noise floor; a dead one reports zero.
    sig = st.get("signal") or 0
    if sig > 0:
        ok &= record(OK, "radio/receiver", f"signal {sig:.3f}, uptime {st.get('uptime', 0)} s")
    else:
        ok &= record(FAIL, "radio/receiver", "signal is zero - no samples arriving")

    # Counters must not move while idle: movement means drops or restarts.
    before = {k: st.get(k, 0) for k in ("gerr", "tx_drops", "tx_overruns", "spk_xruns")}
    frames0 = st.get("frames", 0)
    p.pump(2.0)
    after = p.state
    moved = {k: (before[k], after.get(k)) for k in before if after.get(k, 0) != before[k]}
    if moved:
        ok &= record(WARN, "radio/counters", f"moved while idle: {moved}")
    else:
        ok &= record(OK, "radio/counters", f"no gerr/drop/overrun movement, frames {frames0} -> {after.get('frames')}")

    # The command path the operator actually uses.
    p.send("h")
    p.pump(2.0)
    helps = [t for t in p.texts if t.get("t") in ("help", "resp")]
    if helps and any(len(t.get("lines", [])) > 3 or t.get("line") for t in helps):
        ok &= record(OK, "command/help", f"{len(helps)} reply frames")
    else:
        ok &= record(FAIL, "command/help", "no reply to 'h'")

    gps = after.get("gps") or {}
    if gps.get("has") and not gps.get("valid"):
        ok &= record(WARN, "gps", f"receiver present, no fix ({gps.get('sats')} sats, {gps.get('age_s')} s old)")
    elif gps.get("has"):
        ok &= record(OK, "gps", f"fix {gps.get('grid', '--')} {gps.get('sats')} sats")
    else:
        ok &= record(WARN, "gps", "no GPS receiver seen (META will be empty)")

    audio = after.get("audio")
    route_ok = (after.get("spk_out") or {}).get("open") or audio == "browser"
    ok &= record(OK if route_ok else WARN, "audio",
                 f"route {audio}, mic {after.get('mic_route')}, speaker {after.get('spk_route')}")

    gpio = after.get("gpio") or {}
    if gpio.get("built"):
        ok &= record(OK if gpio.get("ok") else FAIL, "gpio",
                     f"Ptt={gpio.get('out')} Cos={gpio.get('in')} pressed={gpio.get('pressed')}")

    ref = after.get("ref") or {}
    ok &= record(OK, "reflector", "connected to " + str(ref.get("name")) if ref.get("connected")
                 else "not connected (expected while idle)")
    p.close()
    return ok, after


# ---------------------------------------------------------------------------
# The software path with no radio: sim instance plus a reflector that dies
# ---------------------------------------------------------------------------
def check_deep(repo, unit, wstest_dir=None):
    exe = None
    rc, execstart, _ = run(["systemctl", "show", "-p", "ExecStart", unit])
    m = re.search(r"path=(\S+)", execstart)
    if m and os.path.isfile(m.group(1)):
        exe = m.group(1)
    if not exe:
        return record(SKIP, "deep/sim", "no binary to run")
    tmp = tempfile.mkdtemp(prefix="m17regress-")
    port = free_port(18080)
    fake = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_reflector.py")
    if not os.path.isfile(fake):
        record(SKIP, "deep/fake-reflector", f"{fake} missing")
        shutil.rmtree(tmp, ignore_errors=True)
        return False
    ref_port = free_port(17100)
    with open(os.path.join(tmp, "M17Hosts.txt"), "w") as f:
        f.write(f"M17-TESTREF 127.0.0.1 {ref_port}\n")
    shutil.copytree(os.path.join(repo, "m17gismo", "www"), os.path.join(tmp, "www"))
    log = open(os.path.join(tmp, "m17web.log"), "w")
    proc = subprocess.Popen([exe, "-r", "sim", "-w", str(port), "-a", "none"],
                            cwd=tmp, stdout=log, stderr=subprocess.STDOUT)
    ok = True
    try:
        if not wait_http(f"http://127.0.0.1:{port}", 20):
            ok &= record(FAIL, "deep/sim-start", f"no HTTP on {port} within 20 s")
            return ok
        p = Panel("127.0.0.1", port, timeout=5.0, wstest_dir=wstest_dir).pump(2.0)
        ok &= record(OK if p.state.get("radio") == "sim" else FAIL, "deep/sim-start",
                     f"radio '{p.state.get('radio')}' on port {port}")
        # A reflector that answers, then goes silent without saying goodbye.
        ref = subprocess.Popen([sys.executable, fake, str(ref_port), "12"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        p.send("conn TESTREF")
        p.pump(6.0)
        connected = (p.state.get("ref") or {}).get("connected")
        ok &= record(OK if connected else FAIL, "deep/reflector-connect",
                     f"ref.connected={connected} name={(p.state.get('ref') or {}).get('name')!r}")
        ref.wait(timeout=30)
        dropped_at, seen = None, time.time()
        while time.time() - seen < 45:
            p.pump(2.0)
            if not (p.state.get("ref") or {}).get("connected"):
                dropped_at = time.time() - seen
                break
        ok &= record(OK if dropped_at is not None else FAIL, "deep/reflector-timeout",
                     f"link marked down {dropped_at:.0f}s after the reflector vanished"
                     if dropped_at is not None else "still claiming a link 45 s after it died")
        p.send("disc")
        p.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


def free_port(start):
    for port in range(start, start + 40):
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    raise RuntimeError("no free port")


def wait_http(url, seconds):
    end = time.time() + seconds
    while time.time() < end:
        try:
            code, _, _ = fetch(url + "/", timeout=2)
            if code == 200:
                return True
        except Exception:
            pass
        time.sleep(0.5)
    return False


def fetch(url, timeout=5):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace"), ""
    except urllib.error.HTTPError as e:
        return e.code, "", str(e)
    except Exception as e:
        return 0, "", str(e)


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Regression tests for an m17web station")
    ap.add_argument("--host", default="127.0.0.1", help="where the panel listens")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--repo", default=os.path.expanduser("~/m17web-src"))
    ap.add_argument("--service", default="m17web")
    ap.add_argument("--service-dir", default=None,
                    help="the service's WorkingDirectory (its www/ is the served copy)")
    ap.add_argument("--expect-radio", default="auto", help="'pluto', 'sim' or 'auto'")
    ap.add_argument("--allow-sim", action="store_true", help="do not fail when the route is sim")
    ap.add_argument("--deep", action="store_true", help="add the sim/reflector tests")
    ap.add_argument("--json", help="write the result as JSON here")
    ap.add_argument("--list", action="store_true", help="describe the checks and exit")
    args = ap.parse_args()

    if args.list:
        print((__doc__ or "").strip())
        return 0

    print(f"m17web regression  host {args.host}:{args.port}  repo {args.repo}")
    print("-" * 78)

    service_dir = args.service_dir
    if not service_dir:
        rc, wd, _ = run(["systemctl", "show", "-p", "WorkingDirectory", args.service])
        service_dir = wd.split("=", 1)[-1].strip() or os.path.expanduser("~/m17web")

    check_repo(args.repo)
    check_service(args.service)
    check_binary(args.service)
    check_www(args.repo, service_dir)
    wstest_dir = os.path.join(args.repo, "m17gismo", "tools")
    url_base = f"http://{args.host}:{args.port}"
    check_page_markup(url_base)
    check_assets(url_base)
    check_panel(args.host, args.port, args.allow_sim, args.expect_radio, wstest_dir)
    if args.deep:
        check_deep(args.repo, args.service, wstest_dir)

    fails = [r for r in RESULTS if r["state"] == FAIL]
    warns = [r for r in RESULTS if r["state"] == WARN]
    print("-" * 78)
    print(f"{len([r for r in RESULTS if r['state'] == OK])} passed, "
          f"{len(fails)} failed, {len(warns)} warnings")
    if fails:
        print("failed: " + ", ".join(r["name"] for r in fails))
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"host": args.host, "port": args.port, "results": RESULTS,
                       "failed": len(fails), "warnings": len(warns)}, f, indent=1)
        print(f"json -> {args.json}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
