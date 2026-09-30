#!/usr/bin/env python3
"""Write the LiveArea images the VPK needs: 8-bit palettized PNGs at the exact sizes.

Without arguments, draws plain placeholders (no game artwork is distributed). With
`--from DIR`, converts DIR/{icon0,bg,startup}.png (needs Pillow) to the required format.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

SIZES = {
    "icon0.png": (128, 128),
    "livearea/contents/bg.png": (840, 500),
    "livearea/contents/startup.png": (280, 158),
}


def png_indexed(w, h, pixels, palette):
    """pixels: rows of palette indices; palette: list of (r, g, b)."""
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    raw = b"".join(b"\x00" + bytes(row) for row in pixels)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
            + chunk(b"PLTE", b"".join(bytes(c) for c in palette))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def placeholder(w, h):
    """A dark vertical gradient with a light frame: clearly a placeholder, still readable."""
    palette = [(int(10 + 30 * i / 15), int(12 + 20 * i / 15), int(40 + 60 * i / 15)) for i in range(16)]
    palette.append((230, 200, 120))
    rows = []
    for y in range(h):
        base = y * 16 // h
        row = [16 if x < 3 or y < 3 or x >= w - 3 or y >= h - 3 else base for x in range(w)]
        rows.append(row)
    return png_indexed(w, h, rows, palette)


def convert(src, w, h):
    from PIL import Image  # only needed for real artwork
    img = Image.open(src).convert("RGB").resize((w, h), Image.LANCZOS)
    img = img.quantize(colors=256, method=Image.MEDIANCUT)
    import io
    out = io.BytesIO()
    img.save(out, "PNG", optimize=True)
    return out.getvalue()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", help="sce_sys directory to write into")
    ap.add_argument("--from", dest="src", help="directory with icon0.png, bg.png, startup.png")
    args = ap.parse_args()
    out = Path(args.out)
    for rel, (w, h) in SIZES.items():
        dst = out / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if args.src:
            dst.write_bytes(convert(Path(args.src) / Path(rel).name, w, h))
        elif not dst.exists():
            dst.write_bytes(placeholder(w, h))
    return 0


if __name__ == "__main__":
    sys.exit(main())
