#!/usr/bin/env python3
"""Map the addresses in a console log (log.txt / log_prev.txt) to functions and source lines.

    tools/symbolize.py log.txt [--elf build/khdays.elf]

The eboot is relocated when it loads, so every FAULT line carries the run-time address of
main(); the difference to main() in the ELF is the load bias. pc, lr and any register that
points into the code are resolved with addr2line. The ELF must come from the same build as the
log (bump VERSION for every build that goes to the console).
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("VITASDK", Path.home() / "vitasdk")) / "bin"


def elf_symbol(elf, name):
    out = subprocess.run([str(BIN / "arm-vita-eabi-nm"), str(elf)], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    sys.exit(f"symbolize: {name} not in {elf}")


def text_range(elf):
    out = subprocess.run([str(BIN / "arm-vita-eabi-objdump"), "-h", str(elf)], capture_output=True, text=True).stdout
    lo, hi = None, None
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 4 and (p[1] == ".text" or p[1].startswith(".kh_ov_text")):
            start, size = int(p[3], 16), int(p[2], 16)
            lo = start if lo is None else min(lo, start)
            hi = start + size if hi is None else max(hi, start + size)
    return lo, hi


def addr2line(elf, addrs):
    if not addrs:
        return {}
    out = subprocess.run([str(BIN / "arm-vita-eabi-addr2line"), "-f", "-C", "-e", str(elf)] +
                         [hex(a) for a in addrs], capture_output=True, text=True).stdout.splitlines()
    res = {}
    for i, a in enumerate(addrs):
        fn, loc = out[2 * i], out[2 * i + 1]
        loc = loc.replace(str(ROOT) + "/", "")
        res[a] = f"{fn} ({loc})"
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--elf", default=str(ROOT / "build" / "khdays.elf"))
    args = ap.parse_args()
    elf = Path(args.elf)
    elf_main = elf_symbol(elf, "main")
    lo, hi = text_range(elf)
    text = Path(args.log).read_text(errors="replace")

    for block in re.finditer(r"FAULT[^\n]*\n(?:  [^\n]*\n)*", text):
        blk = block.group(0)
        print(blk.rstrip())
        m = re.search(r"main=([0-9a-f]{8})", blk)
        if not m:
            continue
        bias = int(m.group(1), 16) - elf_main
        regs = dict((k, int(v, 16)) for k, v in re.findall(r"(\w+)=([0-9a-f]{8})", blk))
        wanted = {}
        for k, v in regs.items():
            a = v - bias
            if lo <= a < hi and k not in ("main", "far", "fsr", "spsr"):
                wanted[k] = a & ~1
        # words on the stack that point into the code: probably return addresses, outermost
        # last (a call is the instruction before the address, hence the -2)
        stack = [int(v, 16) - bias for v in re.findall(r"\b([0-9a-f]{8})\b",
                 "\n".join(l for l in blk.splitlines() if "stack+" in l))]
        stack = [a for a in stack if lo <= a < hi]
        names = addr2line(elf, sorted(set(wanted.values()) | {a - 2 for a in stack}))
        for k in ("pc", "lr") + tuple(sorted(set(wanted) - {"pc", "lr"})):
            if k in wanted:
                print(f"    {k:4s} {wanted[k]:08x}  {names[wanted[k]]}")
        seen = set()
        for a in stack:
            if a not in seen:
                seen.add(a)
                print(f"    stack {a:08x}  {names[a - 2]}")
        print()

    # thread entries ("entry X") and the watchdog's stacks of parked threads ("at X"): the log
    # lines that carry them, each annotated, with the load bias of the run's "fault:" line
    m = re.search(r"\(main=([0-9a-f]{8})\)", text)
    if m:
        bias = int(m.group(1), 16) - elf_main
        pat = re.compile(r"(?:entry| at) ([0-9a-f]{8})")
        # a return address ("at") points after the call: look up the call itself
        keyed = [(l, int(pat.search(l).group(1), 16) - bias - (2 if " at " in l else 0))
                 for l in text.splitlines() if pat.search(l)]
        names = addr2line(elf, sorted({k for _, k in keyed if lo <= k < hi}))
        for l, k in keyed:
            print(f"{l}   {names.get(k, '(outside the ELF)')}")


if __name__ == "__main__":
    main()
