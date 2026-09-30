#!/usr/bin/env python3
"""Post-link checks on the Vita ELF. Fails the build on problems that only show on the console.

Self-calls. GCC recognises a loop that computes strlen/memset/memcpy and replaces it with a
call to that function. Compiled inside a function of the same name -- the decomp's MSL
strlen -- the result is a function that jumps to itself: an endless loop at the first use, and
since it replaces the Vita's own strlen, the port freezes before it can log anything. Any
function whose first instruction branches to its own start is reported.

    check_elf.py build/khdays.elf
"""
import os
import re
import subprocess
import sys
from pathlib import Path

BIN = Path(os.environ.get("VITASDK", Path.home() / "vitasdk")) / "bin"


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
    if bad:
        print("check_elf: functions that jump to themselves (a loop GCC turned into a call to "
              "the function being defined?):", ", ".join(bad), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
