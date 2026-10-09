#!/usr/bin/env python3
"""Generate platform/nitro/button_sprites.inc: the battle HUD's A/B/X/Y sprites redrawn as the
Vita's Circle/Cross/Triangle/Square.

    tools/button_sprites.py ROM

The icons live in UI/btl/main.p2 (entry 4, a D2KP holding an NCLR and a 32x19-tile NCGR):
the four brown 16-pixel shortcut buttons (A, X, Y, B), the green 8-pixel buttons (A, B, X, Y:
the combo prompts) and the white ones (A, B). Nothing of the ROM goes into the output: each
tile to change is named by a hash of its original 32 bytes (and an XOR of its words, a quick
first test), with the pixels to write over it, which are drawn here. The port applies them to
every decompressed file whose tiles match (nitro/button_sprites.c).
"""
import struct
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "platform" / "nitro" / "button_sprites.inc"


def nitrofs(rom):
    fnt_off, _, fat_off, _ = struct.unpack_from("<IIII", rom, 0x40)
    files = {}

    def walk(dir_id, prefix):
        sub_off, first_id, _ = struct.unpack_from("<IHH", rom, fnt_off + (dir_id & 0xFFF) * 8)
        p, fid = fnt_off + sub_off, first_id
        while rom[p]:
            t = rom[p]
            name = rom[p + 1:p + 1 + (t & 0x7F)].decode("latin1")
            p += 1 + (t & 0x7F)
            if t & 0x80:
                walk(struct.unpack_from("<H", rom, p)[0], prefix + name + "/")
                p += 2
            else:
                files[prefix + name] = struct.unpack_from("<II", rom, fat_off + fid * 8)
                fid += 1

    walk(0xF000, "")
    return files


def lz11(d):
    size, p, out = d[1] | d[2] << 8 | d[3] << 16, 4, bytearray()
    assert d[0] == 0x11
    while len(out) < size:
        flags = d[p]
        p += 1
        for b in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> b):
                ind = d[p] >> 4
                if ind == 0:
                    n, disp, p = ((d[p] & 15) << 4 | d[p + 1] >> 4) + 0x11, ((d[p + 1] & 15) << 8 | d[p + 2]) + 1, p + 3
                elif ind == 1:
                    n = ((d[p] & 15) << 12 | d[p + 1] << 4 | d[p + 2] >> 4) + 0x111
                    disp, p = ((d[p + 2] & 15) << 8 | d[p + 3]) + 1, p + 4
                else:
                    n, disp, p = ind + 1, ((d[p] & 15) << 8 | d[p + 1]) + 1, p + 2
                for _ in range(n):
                    out.append(out[-disp])
            else:
                out.append(d[p])
                p += 1
    return bytes(out)


def p2_entry(d, i):
    cnt = struct.unpack_from("<H", d, 2)[0]
    sec = struct.unpack_from("<H", d, 0x10 + i * 2)[0]
    size = struct.unpack_from("<I", d, ((cnt + 1) // 2) * 4 + 0x10 + i * 4)[0]
    raw = d[0x200 + sec * 0x200:0x200 + sec * 0x200 + (size & 0x7FFFFFFF)]
    return lz11(raw) if size & 0x80000000 else raw


# 5x5 symbols, '#' drawn in the letter's colour
SYMBOLS = {
    "circle": [".###.", "#...#", "#...#", "#...#", ".###."],
    "cross": ["#...#", ".#.#.", "..#..", ".#.#.", "#...#"],
    "triangle": ["..#..", ".#.#.", ".#.#.", "#...#", "#####"],
    "square": ["#####", "#...#", "#...#", "#...#", "#####"],
}
# DS button -> the Vita's button in the same place, and with config confirm_cross (the DS's A
# on Cross, B on Circle)
VITA = {"A": "circle", "B": "cross", "X": "triangle", "Y": "square"}
VITA_SWAPPED = dict(VITA, A="cross", B="circle")

# (x, y) of the symbol's top-left pixel in the NCGR, the button, the area cleared first
# (x0, y0, x1, y1 inclusive) and its colour, the symbol's colour, its shadow's (or None)
ICONS = []
for i, b in enumerate("AXYB"):  # the shortcut buttons, 16 pixels, brown
    x = 96 + i * 16
    ICONS.append(((x + 5, 85), b, (x + 3, 85, x + 10, 90), 0xE, 0xA, 0xF))
for i, b in enumerate("ABXY"):  # green, 8 pixels
    x = 208 + i * 8
    ICONS.append(((x + 1, 133), b, (x + 1, 133, x + 6, 137), 0x4, 0x2, None))
for i, b in enumerate("AB"):  # white, 8 pixels
    x = 240 + i * 8
    ICONS.append(((x + 1, 133), b, (x + 1, 133, x + 6, 137), 0x2, 0xE, None))


def main():
    rom = Path(sys.argv[1]).read_bytes()
    files = nitrofs(rom)
    s, e = files["UI/btl/main.p2"]
    x = p2_entry(rom[s:e], 4)
    o = x.find(b"RGCN")
    p = o + struct.unpack_from("<H", x, o + 0xC)[0]
    datasize = struct.unpack_from("<I", x, p + 0x18)[0]
    tiles = bytearray(x[p + 0x20:p + 0x20 + datasize])
    cols = 32
    orig = bytes(tiles)

    def setpx(px, py, v):
        t = (py // 8) * cols + px // 8
        i = t * 32 + (py % 8) * 4 + (px % 8) // 2
        sh = (px & 1) * 4
        tiles[i] = (tiles[i] & ~(15 << sh)) | (v << sh)

    def draw(mapping):
        tiles[:] = orig
        for (sx, sy), b, (x0, y0, x1, y1), bg, fg, shadow in ICONS:
            for yy in range(y0, y1 + 1):
                for xx in range(x0, x1 + 1):
                    setpx(xx, yy, bg)
            sym = SYMBOLS[mapping[b]]
            on = {(sx + c, sy + r) for r in range(5) for c in range(5) if sym[r][c] == "#"}
            if shadow is not None:
                for (px, py) in on:
                    if (px + 1, py + 1) not in on and px + 1 <= x1 and py + 1 <= y1:
                        setpx(px + 1, py + 1, shadow)
            for (px, py) in on:
                setpx(px, py, fg)
        return bytes(tiles)

    drawn = {1: draw(VITA), 2: draw(VITA_SWAPPED)}

    def fnv64(b):
        h = 0xCBF29CE484222325
        for c in b:
            h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        return h

    entries = []
    changed = [t for t in range(len(orig) // 32)
               if any(orig[t * 32:t * 32 + 32] != drawn[v][t * 32:t * 32 + 32] for v in drawn)]
    hashes = [fnv64(orig[t * 32:t * 32 + 32]) for t in changed]
    below = cols * 32  # the tile under it, in the NCGR's 32-tile rows
    for t, h in zip(changed, hashes):
        a = orig[t * 32:t * 32 + 32]
        xor = 0
        for w in struct.unpack("<8I", a):
            xor ^= w
        # two icons' tiles alike (X's and Y's tops) tell apart by the tile under them
        ctx = 0, 0
        if hashes.count(h) > 1:
            ctx = below, fnv64(orig[t * 32 + below:t * 32 + below + 32])
        # the changed bytes only (pixels are nibbles: a byte holds two); one entry for both
        # mappings (variant 0) where they draw the tile alike, else one each (1 as placed, 2
        # with confirm_cross)
        ed = {v: [(i, drawn[v][t * 32 + i]) for i in range(32) if a[i] != drawn[v][t * 32 + i]] for v in drawn}
        if ed[1] == ed[2]:
            entries.append((h, xor, ctx, 0, ed[1]))
        else:
            entries.extend((h, xor, ctx, v, ed[v]) for v in drawn)
    tiles[:] = drawn[1]

    lines = ["/* Generated by tools/button_sprites.py from the user's ROM: no ROM data, only hashes",
             " * of the original tiles and the bytes drawn over them. */"]
    for h, xor, (coff, chash), variant, edits in entries:
        body = ", ".join("{ %d, 0x%02x }" % e for e in edits)
        lines.append("{ 0x%016xull, 0x%08xu, %d, 0x%016xull, %d, %d, { %s } }," % (h, xor, coff, chash, variant, len(edits), body))
    OUT.write_text("\n".join(lines) + "\n")
    print(f"{len(entries)} entries, {sum(len(e[4]) for e in entries)} bytes drawn -> {OUT}")

    # a preview beside the ROM-free output, for checking by eye
    if len(sys.argv) > 2:
        pal_o = x.find(b"RLCN")
        pp = pal_o + struct.unpack_from("<H", x, pal_o + 0xC)[0]
        raw = x[pp + 0x18:pp + 0x18 + 32]
        pal = [struct.unpack_from("<H", raw, i)[0] for i in range(0, 32, 2)]
        import zlib
        sc, X0, Y0, W, H = 12, 92, 78, 68, 16
        img = bytearray()
        for yy in range(H * sc):
            img.append(0)
            for xx in range(W * sc):
                px, py = X0 + xx // sc, Y0 + yy // sc
                if False:
                    img += bytes((30, 30, 50))
                    continue
                t = (py // 8) * cols + px // 8
                v = (tiles[t * 32 + (py % 8) * 4 + (px % 8) // 2] >> ((px & 1) * 4)) & 15
                c = pal[v]
                img += bytes((40, 40, 60)) if v == 0 else bytes(((c & 31) * 8, (c >> 5 & 31) * 8, (c >> 10 & 31) * 8))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        Path(sys.argv[2]).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W * sc, H * sc, 8, 2, 0, 0, 0))
                                      + chunk(b"IDAT", zlib.compress(bytes(img))) + chunk(b"IEND", b""))


if __name__ == "__main__":
    main()
