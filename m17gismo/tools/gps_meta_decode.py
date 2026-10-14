#!/usr/bin/env python3
"""Decode an M17 LSF META "GNSS Position Data" payload, from the specification.

Written directly from specification 2.0.9 (section "GNSS Data") rather than from
the C code, so it can be used to check the C encoder independently:

    python3 tools/gps_meta_decode.py 5A00C1D1...

Byte layout (112 bits):
  byte 0      Data Source (4) | Station Type (4)
  byte 1      Validity (4) | Radius (3) | Bearing bit 8
  byte 2      Bearing bits 7..0
  bytes 3..5  Latitude  24 bit two's complement, +-8388607 = +-90 deg
  bytes 6..8  Longitude 24 bit two's complement, +-8388607 = +-180 deg
  bytes 9..10 Altitude  16 bit, 0.5 m steps, offset 500 m
  byte 11     Speed bits 11..4
  byte 12     Speed bits 3..0 | reserved
  byte 13     reserved
"""
import sys


def bits(data: bytes, start: int, count: int) -> int:
    v = 0
    for i in range(start, start + count):
        v = (v << 1) | ((data[i >> 3] >> (7 - (i & 7))) & 1)
    return v


def twos(value: int, width: int) -> int:
    return value - (1 << width) if value & (1 << (width - 1)) else value


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    h = sys.argv[1].strip().replace(" ", "").replace("0x", "")
    data = bytes.fromhex(h)
    if len(data) != 14:
        print(f"expected 14 bytes (28 hex digits), got {len(data)}")
        return 2

    source = bits(data, 0, 4)
    station = bits(data, 4, 4)
    validity = bits(data, 8, 4)
    radius = bits(data, 12, 3)
    bearing = (bits(data, 15, 1) << 8) | bits(data, 16, 8)
    lat_raw = twos(bits(data, 24, 24), 24)
    lon_raw = twos(bits(data, 48, 24), 24)
    alt = bits(data, 72, 16)
    speed = bits(data, 88, 12)

    lat = lat_raw / 8388607.0 * 90.0
    lon = lon_raw / 8388607.0 * 180.0
    alt_m = alt * 0.5 - 500.0
    speed_kmh = speed * 0.5

    print(f"data source      : {source}  (0 = M17 client, 1 = OpenRTX, 15 = other)")
    print(f"station type     : {station}  (0 fixed, 1 mobile, 2 handheld, 15 other)")
    print(f"validity         : 0b{validity:04b}  position={bool(validity & 8)} "
          f"altitude={bool(validity & 4)} velocity={bool(validity & 2)} radius={bool(validity & 1)}")
    print(f"radius code      : {radius}")
    print(f"latitude         : {lat:.6f}   (raw {lat_raw})")
    print(f"longitude        : {lon:.6f}   (raw {lon_raw})")
    print(f"altitude         : {alt_m:.1f} m")
    print(f"speed            : {speed_kmh:.1f} km/h")
    print(f"bearing          : {bearing} deg")
    return 0


if __name__ == "__main__":
    sys.exit(main())
