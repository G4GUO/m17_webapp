#!/usr/bin/env python3
"""Pretend to be a GPS receiver: a pty that emits GGA and RMC once a second.

The station opens the printed pty as a serial port, so a fix can be given to a
simulated radio without a receiver on the bench:

    python3 tools/gps_feed.py &            # prints the pty it made
    ./m17web -r sim -a none -w 8089 &
    python3 tools/wstest.py --port 8089 --cmd "gps /dev/pts/N 38400"

The position is fixed and only the time changes, so the sentences stay valid and
the locator is stable enough to assert on.  It is the author's QTH (IO90ST).
"""
import datetime, os, pty, time, tty

LAT_DEG, LAT_MIN = 50, 49.482          # 50 deg 49.482' N
LON_DEG, LON_MIN = 0, 27.072           # 0 deg 27.072' W


def sentence(body):
    """Frame a body with the NMEA checksum the parsers insist on."""
    c = 0
    for ch in body:
        c ^= ord(ch)
    return "$%s*%02X\r\n" % (body, c)


def main():
    master, slave = pty.openpty()
    tty.setraw(master)
    print(os.ttyname(slave), flush=True)     # the caller reads this line
    while True:
        now = datetime.datetime.utcnow()
        hhmmss = now.strftime("%H%M%S") + ".00"
        ddmmyy = now.strftime("%d%m%y")
        lat = "%02d%06.3f" % (LAT_DEG, LAT_MIN)
        lon = "%03d%06.3f" % (LON_DEG, LON_MIN)
        gga = sentence("GPGGA,%s,%s,N,%s,W,1,09,0.9,12.4,M,47.0,M,," % (hhmmss, lat, lon))
        rmc = sentence("GPRMC,%s,A,%s,N,%s,W,0.0,0.0,%s,," % (hhmmss, lat, lon, ddmmyy))
        os.write(master, (gga + rmc).encode())
        time.sleep(1)


if __name__ == "__main__":
    main()
