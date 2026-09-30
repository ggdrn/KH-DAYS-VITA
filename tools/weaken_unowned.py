#!/usr/bin/env python3
"""Archive one game module, keeping the decomp's section ownership.

The decomp sometimes defines a variable in more than one source: the function that uses it
declares `int data_02042730 = 1;` and so does the `data/` file for that address. On the DS
build this is harmless, because dsd's delinks.txt says which file each section of the binary
comes from, and the linker only takes that file's copy. A plain link sees duplicates.

This tool applies the same rule. A data symbol (.data/.bss/.rodata/common) whose section kind
delinks.txt does not assign to its source file is made weak, so the owner's strong definition
wins. Where no file owns it, the linker keeps one of the weak copies.

Symbols listed in build/gen/bss_names.txt (tools/gen_link_support.py) are weakened wherever C
defines them: their single copy is the generated per-module .bss block.

    weaken_unowned.py DECOMP OUT.a OBJ...    (objects are build/game/<decomp-relative>.o)
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

BIN = Path(os.environ.get("VITASDK", Path.home() / "vitasdk")) / "bin"
NM = str(BIN / "arm-vita-eabi-nm")
OBJCOPY = str(BIN / "arm-vita-eabi-objcopy")
AR = str(BIN / "arm-vita-eabi-gcc-ar")

DATA_KINDS = (".data", ".bss", ".rodata", ".sdata", ".sbss")


def load_ownership(decomp):
    """source path -> set of section kinds it owns, from every module's delinks.txt."""
    owned = {}
    for delinks in (decomp / "config" / "arm9").rglob("delinks.txt"):
        current = None
        for line in delinks.read_text(encoding="utf-8").splitlines():
            if line and not line[0].isspace() and line.endswith(":"):
                current = owned.setdefault(line[:-1], set())
            elif current is not None:
                m = re.match(r"\s+(\.\w+)\s+start:", line)
                if m:
                    current.add(m.group(1))
    return owned


def kind_of(section_letter):
    """nm's letter -> section kind (nm -A prints one letter per symbol)."""
    return {"D": ".data", "B": ".bss", "R": ".rodata", "C": ".bss", "G": ".sdata", "S": ".sbss"}.get(section_letter)


def main():
    decomp, out, objs = Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
    owned = load_ownership(decomp)
    bss_names = set(Path("build/gen/bss_names.txt").read_text().split())
    # nm over all objects at once: "<obj>: <addr> <letter> <name>"
    res = subprocess.run([NM, "-A", "--defined-only", *objs], capture_output=True, text=True, check=True)
    weaken = {}
    for line in res.stdout.splitlines():
        m = re.match(r"(.+?):\s*(?:[0-9a-fA-F]+)?\s+([A-Z])\s+(\S+)$", line)
        if not m:
            continue
        obj, letter, name = m.groups()
        kind = kind_of(letter)
        if kind is None:
            continue
        base = os.path.relpath(obj, "build/game")[:-2]
        kinds = next((owned[base + e] for e in (".c", ".cpp", ".s") if base + e in owned), set())
        if name in bss_names or kind not in kinds:
            weaken.setdefault(obj, []).append(name)

    tmp = Path(out + ".d")
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    final = []
    for i, obj in enumerate(objs):
        names = weaken.get(obj)
        if not names:
            final.append(obj)
            continue
        dst = tmp / f"{i}_{Path(obj).name}"
        args = [OBJCOPY]
        for n in names:
            args += ["-W", n]
        subprocess.run(args + [obj, str(dst)], check=True)
        final.append(str(dst))

    if os.path.exists(out):
        os.remove(out)
    rsp = out + ".rsp"
    Path(rsp).write_text("\n".join(f'"{p}"' for p in final))
    subprocess.run([AR, "rcs", out, "@" + rsp], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
