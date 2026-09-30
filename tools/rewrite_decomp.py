#!/usr/bin/env python3
"""Mechanical PLATFORM_VITA rewrites of the build tree (build/decomp).

Each pass edits sources in place and is idempotent: a line it already rewrote is left alone.
The DS side of every change is the original text, so the matching build does not change.
After running, ./export_patch.sh records the result in patches/decomp.patch.

    tools/rewrite_decomp.py [--tree build/decomp] [PASS...]

Passes:
  abs-symbols   `extern T NAME[1];` for linker-absolute constants (OVERLAY_n_ID, SDK_*): the
                source takes the symbol's *address* as the number. vita-elf-create cannot
                relocate absolute symbols, so on the Vita NAME becomes a macro that yields the
                same number as an address of the same type.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The DS linker's absolute symbols. OVERLAY_n_ID = n (dsd's lcf); the SDK constants come from
# the decomp's tools/configure.py ABSOLUTE_SYMBOLS (the ROM's literal pools hold them).
SDK_ABSOLUTE = {
    "SDK_SYS_STACKSIZE": 0x00000000,
    "SDK_IRQ_STACKSIZE": 0x00000800,
    "SDK_SECTION_ARENA_DTCM_START": 0x027E0E60,
}

MARK = "/* PLATFORM_VITA: absolute symbol */"


def abs_value(name):
    m = re.fullmatch(r"OVERLAY_(\d+)_ID", name)
    if m:
        return int(m.group(1))
    return SDK_ABSOLUTE.get(name)


DECL = re.compile(
    r"^(?P<indent>[ \t]*)extern\s+(?P<type>[A-Za-z_][\w ]*?)\s*(?P<name>OVERLAY_\d+_ID|SDK_[A-Z_]+)"
    r"\s*(?P<suffix>\[\s*\d*\s*\]|\(\s*void\s*\))?\s*;(?P<rest>.*)$")


def macro_for(indent, typ, name, suffix, value):
    typ = typ.strip()
    if suffix and suffix.startswith("("):  # extern void NAME(void): a function designator
        expr = f"(({typ} (*)(void))0x{value:x})"
    elif suffix:  # extern T NAME[n]: an array lvalue at that address
        n = re.sub(r"\D", "", suffix) or "1"
        expr = f"(*({typ} (*)[{n}])0x{value:x})"
    else:  # extern T NAME: a scalar lvalue at that address
        expr = f"(*({typ} *)0x{value:x})"
    return f"{indent}#define {name} {expr} {MARK}"


def pass_abs_symbols(tree):
    changed = 0
    for path in sorted(list((tree / "src").rglob("*.c")) + list((tree / "libs").rglob("*.c"))):
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if "OVERLAY_" not in text and "SDK_" not in text:
            continue
        lines = text.split("\n")
        out = []
        i = 0
        dirty = False
        while i < len(lines):
            line = lines[i]
            m = DECL.match(line)
            already = i >= 2 and lines[i - 1].strip() == "#else" and MARK in lines[i - 2]
            value = abs_value(m.group("name")) if m else None
            if m and value is not None and not already:
                out += ["#ifdef PLATFORM_VITA",
                        macro_for(m.group("indent"), m.group("type"), m.group("name"),
                                  m.group("suffix"), value),
                        "#else", line, "#endif"]
                dirty = True
            else:
                out.append(line)
            i += 1
        if dirty:
            path.write_text("\n".join(out), encoding="utf-8", errors="surrogateescape")
            changed += 1
    return changed


PASSES = {"abs-symbols": pass_abs_symbols}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default=str(ROOT / "build" / "decomp"))
    ap.add_argument("passes", nargs="*", default=list(PASSES))
    args = ap.parse_args()
    tree = Path(args.tree)
    for name in args.passes:
        n = PASSES[name](tree)
        print(f"{name}: {n} files changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
