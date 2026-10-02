#!/usr/bin/env python3
"""Find calls whose declaration disagrees with the definition on 64-bit parameters.

    tools/check_abi64.py build/decomp

mwccarm passes a long long in the next two registers whatever their number, so the decomp
can declare a function as taking two ints where it is defined with a long long and both
compile to the same code on the DS. GCC's EABI starts a 64-bit argument on an even register
(r0:r1 or r2:r3), so such a call puts every later argument in the wrong place (0.0.76:
Ov192_BoxSweepPush read its aim pointer from the stack and the Poison Plant crashed).
Lists each function defined with a 64-bit parameter and every extern declaration of it that
does not have a 64-bit type at the same position, and the reverse."""
import re
import sys
from pathlib import Path

T64 = re.compile(r"\b(long\s+long|s64|u64|fx64c?|s64v|u64v)\b")
DEF = re.compile(r"^(?!\s*(?:extern|typedef|static\s+inline|return|if|while|for|switch)\b)"
                 r"[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(([^;{}()]*(?:\([^()]*\)[^;{}()]*)*)\)\s*\{",
                 re.M)
DECL = re.compile(r"\bextern\b[^;{}]*?\b(\w+)\s*\(([^;{}()]*)\)\s*;")


def params(text):
    text = text.strip()
    if not text or text == "void":
        return []
    return [p.strip() for p in text.split(",")]


def sig(ps):
    return [bool(T64.search(p)) for p in ps]


def main():
    root = Path(sys.argv[1])
    files = list(root.glob("src/**/*.c")) + list(root.glob("libs/**/*.c"))
    defs, decls = {}, []
    for f in files:
        s = re.sub(r"/\*.*?\*/|//[^\n]*", "", f.read_text(errors="replace"), flags=re.S)
        for m in DEF.finditer(s):
            defs.setdefault(m.group(1), []).append((f, params(m.group(2))))
        for m in DECL.finditer(s):
            decls.append((f, m.group(1), params(m.group(2))))
    bad = 0
    for f, name, ps in decls:
        if name not in defs:
            continue
        for df, dps in defs[name]:
            a, b = sig(ps), sig(dps)
            if not any(a) and not any(b):
                continue
            if not ps:  # unprototyped: the call site decides
                continue
            if a != b:
                bad += 1
                print(f"{f.relative_to(root)}: {name}({', '.join(ps)})")
                print(f"    defined in {df.relative_to(root)}: {name}({', '.join(dps)})")
    print(f"check_abi64: {bad} mismatched declarations")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
