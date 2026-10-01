#!/usr/bin/env bash
# Builds and runs the host MobiClip decoder test (mobiclip_test.cpp) on the user's dump; the
# sampled frames land in build/mobiclip_test as PNGs.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=build/mobiclip_test
mkdir -p "$out"
d=build/decomp
data=$d/src/overlays/system/ov024_mobiclip/data
port=$d/libs/mobiclip/video/portable
# the C parts: the two run/level tables (the decomp's data) and the port's ROM file lookup
for c in "$data/ov024_vlc_variant0_0208a7c4.c" "$data/ov024_vlc_variant1_020886c4.c" platform/nitro/romfs.c; do
    cc -std=gnu11 -O2 -w -I"$d/include" -Iplatform/compat -Itools/mobiclip_test -Iplatform \
        -c "$c" -o "$out/$(basename "${c%.c}").o"
done
c++ -std=c++11 -O2 -w -I"$port" -Iplatform -Itools/mobiclip_test \
    tools/mobiclip_test/mobiclip_test.cpp "$port/mobiclip_frame_core.cpp" "$port/mobiclip_reference.cpp" \
    "$out"/*.o -o "$out/mobiclip_test"
(cd "$out" && rm -f frame_*.p?m && ./mobiclip_test "$(cd ../.. && pwd)/../days.nds" "${1:-/mv/802.mods}" "${2:-60}")
python3 - "$out" <<'PY'
import pathlib, struct, sys, zlib
for p in sorted(pathlib.Path(sys.argv[1]).glob("frame_*.ppm")):
    d = p.read_bytes(); parts = d.split(b"\n", 3); w, h = map(int, parts[1].split()); px = parts[3]
    rows = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    ch = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    p.with_suffix(".png").write_bytes(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(rows)) + ch(b"IEND", b""))
    p.unlink()
PY
ls "$out"/*.png | head
