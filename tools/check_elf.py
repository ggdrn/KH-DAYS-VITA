#!/usr/bin/env python3
"""Post-link checks on the Vita ELF. Fails the build on problems that only show on the console.

Self-calls. GCC recognises a loop that computes strlen/memset/memcpy and replaces it with a
call to that function. Compiled inside a function of the same name -- the decomp's MSL
strlen -- the result is a function that jumps to itself: an endless loop at the first use, and
since it replaces the Vita's own strlen, the port freezes before it can log anything. Any
function whose first instruction branches to its own start is reported.

Loose statics. The decomp defines the statics of a "khdays: shared-bss" group weakly in every
file that uses them; on the DS they sit in one block after a base symbol, and some functions
reach them off that base. A name that is still a weak object after the link has no place in the
block: code that uses the name and code that uses the base see two different variables (0.0.9:
OSi_CurrentThreadPtr stayed NULL while OS_InitThread set the block's copy). Such names go in
tools/gen_link_support.py SUBOBJECTS. C++ runtime objects (_Z...) are weak by design, and
data_<address> objects are the DS objects themselves.

Absolute addresses. vita-elf-create does not relocate references to absolute symbols, and the
eboot is loaded elsewhere than its link address: an absolute symbol whose value lies inside
the image is an address the code will get wrong at run time (ld makes --defsym name=sym+off
absolute; 0.0.10's OSi_CurrentThreadPtr). Define such names as labels or `.set` in assembly.

    check_elf.py build/khdays.elf
"""
import os
import re
import subprocess
import sys
from pathlib import Path

BIN = Path(os.environ.get("VITASDK", Path.home() / "vitasdk")) / "bin"
# named after their DS address: the object itself, placed by its module's layout
DS_NAMED = re.compile(r"^data_(?:ov\d+_)?[0-9a-f]{8}$")


def main():
    elf = sys.argv[1]
    out = subprocess.run([str(BIN / "arm-vita-eabi-objdump"), "-d", "--no-show-raw-insn", elf],
                         capture_output=True, text=True, check=True).stdout
    bad, cur, first = [], None, False
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]+) <([^>]+)>:$", line)
        if m:
            cur, first = m.groups(), True
            continue
        if cur and first and re.match(r"^\s*[0-9a-f]+:", line):
            first = False
            b = re.search(r"\tb(?:\.w|\.n)?\t([0-9a-f]+) <([^>+]+)>", line)
            if b and b.group(1) == cur[0]:
                bad.append(cur[1])
    rc = 0
    if bad:
        print("check_elf: functions that jump to themselves (a loop GCC turned into a call to "
              "the function being defined?):", ", ".join(bad), file=sys.stderr)
        rc = 1
    syms = subprocess.run([str(BIN / "arm-vita-eabi-nm"), elf], capture_output=True, text=True,
                          check=True).stdout
    loose = sorted(p[2] for p in (l.split() for l in syms.splitlines())
                   if len(p) == 3 and p[1] in "Vv" and not p[2].startswith("_Z")
                   and not DS_NAMED.match(p[2]))
    if loose:
        print("check_elf: weak objects with no place in a DS block (add them to "
              "gen_link_support.py SUBOBJECTS):", ", ".join(loose), file=sys.stderr)
        rc = 1
    heads = subprocess.run([str(BIN / "arm-vita-eabi-objdump"), "-h", elf], capture_output=True,
                           text=True, check=True).stdout.splitlines()
    ranges = []
    for i, line in enumerate(heads[:-1]):
        p = line.split()
        if len(p) >= 4 and p[0].isdigit() and "ALLOC" in heads[i + 1]:
            ranges.append((int(p[3], 16), int(p[3], 16) + int(p[2], 16)))
    lo, hi = min(a for a, _ in ranges), max(b for _, b in ranges)
    absolute = sorted({p[2] for p in (l.split() for l in syms.splitlines())
                       if len(p) == 3 and p[1] in "Aa" and lo <= int(p[0], 16) < hi})
    if absolute:
        print("check_elf: absolute symbols inside the image (not relocated on the Vita; define "
              "them in assembly):", ", ".join(absolute), file=sys.stderr)
        rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
