#!/usr/bin/env bash
# Builds and runs the host test for platform/hw/gpu2d.c; the scenes land in build/gpu2d_test as
# PNGs (top-left pixel first, 256x192).
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/gpu2d_test
mkdir -p "$out"
cc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Iplatform -Iplatform/compat \
    tools/gpu2d_test/gpu2d_test.c platform/hw/gpu2d.c -o "$out/gpu2d_test"
(cd "$out" && ./gpu2d_test)
python3 - "$out" <<'PY'
import struct, sys, zlib, pathlib
out = pathlib.Path(sys.argv[1])
for raw in sorted(out.glob("*.rgba")):
    data = raw.read_bytes()
    rows = b"".join(b"\0" + data[y * 1024:(y + 1) * 1024] for y in range(192))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 256, 192, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))
    raw.with_suffix(".png").write_bytes(png)
    raw.unlink()
    print(raw.with_suffix(".png"))
PY
