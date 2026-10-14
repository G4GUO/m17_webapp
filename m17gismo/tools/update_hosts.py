#!/usr/bin/env python3
"""Refresh M17Hosts.txt from the M17 Project's published reflector list.

    python3 tools/update_hosts.py [--out M17Hosts.txt] [--also /home/charles/m17web]

The published list is JSON (designator, ipv4, port, modules, sponsor...); this
writes the simple "Designator IP Port" file the program expects, keeping the
full designators ("M17-M17").

Worth knowing: reflector addresses change.  A 2022 copy of this file still
pointed M17-M17 at an address it left years ago, and the symptom is exactly a
connect button that appears to do nothing - the lookup succeeds, the CONN packet
goes to nowhere, and no reply ever arrives.
"""
import argparse
import json
import os
import shutil
import sys
import urllib.request
from datetime import datetime, timezone

URL = "https://m17-project.github.io/hostfiles/M17Hosts.json"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="M17Hosts.txt")
    ap.add_argument("--also", action="append", default=[],
                    help="copy the file here too (a directory, or a full path)")
    ap.add_argument("--url", default=URL)
    a = ap.parse_args()

    try:
        with urllib.request.urlopen(a.url, timeout=30) as r:
            data = json.load(r)
    except Exception as e:                                   # noqa: BLE001
        print(f"cannot fetch {a.url}: {e}", file=sys.stderr)
        return 1

    rows, seen = [], set()
    for r in data.get("reflectors", []):
        d = (r.get("designator") or "").strip()
        ip = r.get("ipv4")
        port = r.get("port") or 17000
        if not d or not ip or d in seen:
            continue
        if not d.upper().startswith("M17-"):
            d = "M17-" + d
        seen.add(d)
        rows.append((d, ip, int(port)))
    rows.sort()

    meta = data.get("_refcheck_metadata") or {}
    head = (
        "# m17web reflector list\n"
        "#\n"
        f"# Generated from {a.url}\n"
        "#   (reflector data via https://refcheck.radio, processed by W0CHP)\n"
        f"#   list generated {meta.get('generated', 'unknown')}; "
        f"written {datetime.now(timezone.utc):%Y-%m-%d %H:%M} UTC\n"
        "#\n"
        "# Refresh with:  python3 tools/update_hosts.py\n"
        "# Format: Designator IP Port.  The leading 'M17-' may be given or omitted\n"
        "# in the web interface and in the conn command.\n"
        "#\n"
    )
    text = head + "\n".join(f"{d} {ip} {p}" for d, ip, p in rows) + "\n"
    with open(a.out, "w") as f:
        f.write(text)
    print(f"wrote {a.out}: {len(rows)} reflectors")

    for t in a.also:
        dst = os.path.join(t, os.path.basename(a.out)) if os.path.isdir(t) else t
        shutil.copyfile(a.out, dst)
        print(f"copied to {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
