#!/usr/bin/env python3
"""Data layout check: objects that sit next to each other on the DS must sit the same way in the
Vita ELF when they come from the same source file.

Game code reaches past one object into the next (a table read as data_02041fd4 + layer * 0x18
spans four objects); that works only while the objects keep their DS order and spacing. For
every pair of DS-named objects (data_[ovNNN_]ADDRESS) defined in the same object file and the
same output section, the difference of their ELF addresses must equal the difference of their
DS addresses.

    check_layout.py build/khdays.elf build/lib    (exit 1 and a list when some pair differs)
"""
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

BIN = Path(os.environ.get("VITASDK", Path.home() / "vitasdk")) / "bin"
DS_NAME = re.compile(r"^data_(ov\d+_)?([0-9a-f]{8})$")


def main():
    elf, libdir = sys.argv[1], Path(sys.argv[2])
    # symbol -> (ELF address, section) for defined objects
    out = subprocess.run([str(BIN / "arm-vita-eabi-objdump"), "-t", elf], capture_output=True,
                         text=True, check=True).stdout
    where = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 6 and p[-1].startswith("data_") and p[2] == "O":
            where[p[-1]] = (int(p[0], 16), p[3])
    # symbol -> defining archive member
    owner = {}
    for lib in sorted(libdir.glob("*.a")):
        nm = subprocess.run([str(BIN / "arm-vita-eabi-nm"), "-A", "--defined-only", str(lib)],
                            capture_output=True, text=True).stdout
        for line in nm.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in "DdRrBbVv" and DS_NAME.match(p[2]):
                owner.setdefault(p[2], p[0].rsplit(":", 1)[0])
    groups = defaultdict(list)
    for name, member in owner.items():
        if name in where:
            m = DS_NAME.match(name)
            groups[(member, where[name][1], m.group(1) or "")].append((int(m.group(2), 16), name))
    bad = []
    for (member, section, _), items in groups.items():
        items.sort()
        for (a, x), (b, y) in zip(items, items[1:]):
            if where[y][0] - where[x][0] != b - a:
                bad.append(f"{member.split('/')[-1]}: {x} -> {y}: DS +{b - a:#x}, ELF "
                           f"{where[y][0] - where[x][0]:+#x} ({section})")
    # across files: objects back to back on the DS (one ends where the next starts) should be
    # back to back here too; gen_link_support.py orders them so. Reported, not fatal.
    sizes = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 6 and p[2] == "O" and DS_NAME.match(p[-1]):
            sizes[p[-1]] = int(p[4], 16)
    runs = defaultdict(list)
    for name, (addr, section) in where.items():
        m = DS_NAME.match(name)
        if m and name in sizes:
            runs[(m.group(1) or "", section)].append((int(m.group(2), 16), name))
    split = 0
    for items in runs.values():
        items.sort()
        for (a, x), (b, y) in zip(items, items[1:]):
            if a + sizes[x] == b and where[y][0] - where[x][0] != b - a:
                split += 1
                if split <= 10:
                    print(f"  across files: {x} -> {y}: DS +{b - a:#x}, ELF "
                          f"{where[y][0] - where[x][0]:+#x}", file=sys.stderr)
    if split:
        print(f"check_layout: {split} DS-contiguous object pairs apart in the ELF", file=sys.stderr)
    if bad:
        print(f"check_layout: {len(bad)} neighbouring objects out of their DS layout:", file=sys.stderr)
        for b in bad[:int(os.environ.get("KH_LAYOUT_SHOW", "40"))]:
            print("  " + b, file=sys.stderr)
        return 1
    print("check_layout: DS layout kept for every object pair from one file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
