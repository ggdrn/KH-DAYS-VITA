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
    """entry i of a P2 pack: the header (its size at +0xc) then 512-byte sectors"""
    cnt = struct.unpack_from("<H", d, 2)[0]
    hdr = struct.unpack_from("<H", d, 0xC)[0] or 0x200
    sec = struct.unpack_from("<H", d, 0x10 + i * 2)[0]
    size = struct.unpack_from("<I", d, ((cnt + 1) // 2) * 4 + 0x10 + i * 4)[0]
    raw = d[hdr + sec * 0x200:hdr + sec * 0x200 + (size & 0x7FFFFFFF)]
    return lz11(raw) if size & 0x80000000 else raw


def ncgr(x):
    """the first NCGR in x: (offset of its tile data, the data, bits a pixel)"""
    o = x.find(b"RGCN")
    p = o + struct.unpack_from("<H", x, o + 0xC)[0]
    bd = struct.unpack_from("<I", x, p + 0xC)[0]
    size = struct.unpack_from("<I", x, p + 0x18)[0]
    return p + 0x20, x[p + 0x20:p + 0x20 + size], 4 if bd == 3 else 8


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
for i, b in enumerate("AXYB"):  # the shortcut buttons, 16 pixels, brown (black in the field's
    x = 96 + i * 16              # palette): in the letters' own colour, no shadow (in the
    ICONS.append(((x + 5, 85), b, (x + 3, 85, x + 10, 90), 0xE, 0xF, None))  # field's palette
    # a shadow came out light, a second symbol over the first: 0.4.19)
for i, b in enumerate("ABXY"):  # green, 8 pixels
    x = 208 + i * 8
    ICONS.append(((x + 1, 133), b, (x + 1, 133, x + 6, 137), 0x4, 0x2, None))
for i, b in enumerate("AB"):  # white, 8 pixels
    x = 240 + i * 8
    ICONS.append(((x + 1, 133), b, (x + 1, 133, x + 6, 137), 0x2, 0xE, None))


# ---- the menus' icons: the font's letters drawn into sprites -------------------------------
# The camp menu, the panel screen and their help lines show the fonts' A/B/X/Y icons (a 9x9
# circle, the letter cut out) drawn into 8-bit sprites, a second colour for the letter and
# often a shadow. Found by shape in these files (sprites of `wide` tiles a row, 1D mapping)
# and in a frame dump of the panel screen (0.4.23: the X by the scroll bar and on the Stats
# button, the green Y). The letter becomes the Vita's symbol, the circle's own colours kept.
# (button, the circle's row the letter starts on): the letter's rows, ' ' cut out. The fonts'
# letters fill rows 2-6; the camp menu's help lines (START Submenu, A Grab Panel B Exit) have
# a taller A and B, rows 1-6 (0.4.24's frame dump)
LETTERS = {
    ("A", 2): ["#### ####", "### # ###", "### # ###", "##     ##", "## ### ##"],
    ("B", 2): ["##    ###", "## ### ##", "##    ###", "## ### ##", "##    ###"],
    ("X", 2): ["## ### ##", "### # ###", "#### ####", "### # ###", "## ### ##"],
    ("Y", 2): ["## ### ##", "### # ###", "#### ####", "#### ####", "#### ####"],
    ("A", 1): ["#### ####", "### # ###", "## ### ##", "##     ##", "## ### ##", "## ### ##"],
    ("B", 1): ["##    ###", "## ### ##", "##    ###", "## ### ##", "## ### ##", "##    ###"],
}
# the symbols in the font's style (nitro/button_glyphs.c): rows 0-8 of the 9x9 circle
DESIGNS = {
    "circle": ["  #####  ", " ##   ## ", "## ### ##", "# ##### #", "# ##### #",
               "# ##### #", "## ### ##", " ##   ## ", "  #####  "],
    "cross": ["  #####  ", " ####### ", "## ### ##", "### # ###", "#### ####",
              "### # ###", "## ### ##", " ####### ", "  #####  "],
    "triangle": ["  #####  ", " ### ### ", "### # ###", "## ### ##", "# ##### #",
                 "#       #", "#########", " ####### ", "  #####  "],
    "square": ["  #####  ", " ####### ", "##     ##", "## ### ##", "## ### ##",
               "## ### ##", "##     ##", " ####### ", "  #####  "],
}
LANGS = ("de", "en", "es", "fr", "it")
MENU_SOURCES = [  # (file, P2 entries or None, sprite width in tiles)
    ("UI/cm/cm.p2", (8, 36), 2),   # the panel screen's green Y (four frames)
    ("UI/cm/cm.p2", (40,), 4),     # the X by the scroll bar and on the Stats button
] + [("UI/cm/cmo_%s.p2" % lang, (2,), 4) for lang in LANGS]  # the camp menu's X buttons


class Sprites:
    """an NCGR's tiles as sprites of `wide` tiles a row: pixel access by window"""

    def __init__(self, data, bpp):
        self.d, self.bpp = bytearray(data), bpp
        self.ts = 32 if bpp == 4 else 64
        self.n = len(data) // self.ts

    def at(self, i, wide, x, y):
        t = i + (y // 8) * wide + x // 8
        if bpp_ok := (t < self.n):
            pass
        if not bpp_ok:
            return None
        if self.bpp == 8:
            return t * 64 + (y % 8) * 8 + x % 8, None
        return t * 32 + (y % 8) * 4 + (x % 8) // 2, (x & 1) * 4

    def get(self, i, wide, x, y):
        a = self.at(i, wide, x, y)
        if a is None:
            return None
        o, sh = a
        return self.d[o] if sh is None else (self.d[o] >> sh) & 15

    def set(self, i, wide, x, y, v):
        o, sh = self.at(i, wide, x, y)
        if sh is None:
            self.d[o] = v
        else:
            self.d[o] = (self.d[o] & ~(15 << sh)) | (v << sh)


def find_letters(sp, wide):
    """(i, cx, cy, letter): the DS letter icons in sprites of `wide` tiles a row, cx, cy the 9x9
    circle's top-left; one per icon"""
    hits, seen = [], set()
    for i in range(0, sp.n - 2 * wide + 1):
        for k, rows in LETTERS.items():
            n = len(rows)
            D = [(x, y) for y in range(n) for x in range(9) if rows[y][x] == " "
                 and not (k[1] == 1 and y == 0 and x in (0, 8))]
            C = [(x, y) for y in range(n) for x in range(1, 8) if (x, y) not in D
                 and (x - 1, y - 1) not in D and (x - 1, y) not in D and (x, y - 1) not in D]
            for oy in range(k[1], 16 - (8 - k[1]) + 1):
                for ox in range(0, wide * 8 - 8):
                    vd = {sp.get(i, wide, ox + x, oy + y) for x, y in D}
                    if len(vd) != 1:
                        continue
                    vc = {sp.get(i, wide, ox + x, oy + y) for x, y in C}
                    if len(vc) != 1 or vc == vd or 0 in vc or 0 in vd:
                        continue
                    t0 = i + (oy // 8) * wide + ox // 8
                    key = (t0, ox % 8, oy % 8, k)
                    if key not in seen:
                        seen.add(key)
                        hits.append((i, ox, oy - k[1], k))
    return hits


def redraw(sp, i, wide, cx, cy, letter, design):
    """the letter of the icon at (cx, cy) redrawn as `design`, the circle's colours kept"""
    rows = LETTERS[letter]
    old = {(x, y + letter[1]) for y in range(len(rows)) for x in range(9) if rows[y][x] == " "
           and 1 <= x <= 7}
    from collections import Counter
    vd = Counter(sp.get(i, wide, cx + x, cy + y) for x, y in old).most_common(1)[0][0]
    body = [(x, y) for y in range(letter[1], letter[1] + len(rows)) for x in range(1, 8)
            if (x, y) not in old and (x - 1, y - 1) not in old]
    vc = Counter(sp.get(i, wide, cx + x, cy + y) for x, y in body).most_common(1)[0][0]
    sh = Counter(v for v in (sp.get(i, wide, cx + x + 1, cy + y + 1) for x, y in old
                             if (x + 1, y + 1) not in old) if v not in (vc, vd, None))
    shadow = sh.most_common(1)[0][0] if sh else None
    new = {(x, y) for y in range(9) for x in range(9) if design[y][x] == " "
           and 1 <= x <= 7 and 1 <= y <= 7}
    for y in range(1, 8):
        for x in range(1, 8):
            v = sp.get(i, wide, cx + x, cy + y)
            if v is None:
                continue
            was_shadow = shadow is not None and v == shadow and (x - 1, y - 1) in old
            if v in (vc, vd) or was_shadow:
                sp.set(i, wide, cx + x, cy + y, vd if (x, y) in new else vc)
    if shadow is not None:
        for x, y in new:
            if (x + 1, y + 1) not in new and x + 1 <= 7 and y + 1 <= 7:
                if sp.get(i, wide, cx + x + 1, cy + y + 1) == vc:
                    sp.set(i, wide, cx + x + 1, cy + y + 1, shadow)


def menu_entries(rom, files, fnv64):
    """the edits for the menus' icons: [(hash, xor, ctx, variant, edits)] on 32-byte halves"""
    out = []
    for name, ents, wide in MENU_SOURCES:
        s, e = files[name]
        d = rom[s:e]
        datas = [lz11(d)] if name.endswith(".z") else [p2_entry(d, k) for k in ents]
        for x in datas:
            data_off, data, bpp = ncgr(x)
            base = Sprites(data, bpp)
            hits = find_letters(base, wide)
            drawn = {}
            for v, mapping in ((1, VITA), (2, VITA_SWAPPED)):
                sp = Sprites(data, bpp)
                for i, cx, cy, k in hits:
                    redraw(sp, i, wide, cx, cy, k, DESIGNS[mapping[k[0]]])
                drawn[v] = bytes(sp.d)
            print(f"  {name}: {len(hits)} icons ({''.join(sorted(h[3][0] for h in hits))})")
            halves = []
            for o in range(0, len(data) - 31, 32):
                a = data[o:o + 32]
                ed = {v: [(j, drawn[v][o + j]) for j in range(32) if a[j] != drawn[v][o + j]] for v in drawn}
                if ed[1] or ed[2]:
                    halves.append((o, a, ed))
            hashes = [fnv64(a) for _, a, _ in halves]
            for (o, a, ed), h in zip(halves, hashes):
                xor = 0
                for w in struct.unpack("<8I", a):
                    xor ^= w
                # alike halves drawn differently tell apart by the 32 bytes after them (in the
                # file as decompressed, where the port looks)
                ctx = 0, 0
                if hashes.count(h) > 1:
                    at = data_off + o + 32
                    ctx = 32, fnv64(x[at:at + 32])
                if ed[1] == ed[2]:
                    out.append((h, xor, ctx, 0, ed[1]))
                else:
                    out.extend((h, xor, ctx, v, ed[v]) for v in drawn)
    return out


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
    entries += menu_entries(rom, files, fnv64)
    # one entry per (tile, variant, context), sorted by the quick test (the port looks them up
    # by binary search, nitro/button_sprites.c)
    uniq = {}
    for en in entries:
        uniq[(en[0], en[2], en[3], tuple(en[4]))] = en
    entries = sorted(uniq.values(), key=lambda en: (en[1], en[0]))

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
