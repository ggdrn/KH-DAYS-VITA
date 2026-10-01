"""Calls into overlay code at addresses several overlays share.

On the DS the overlays of one group are loaded at the same address, and a caller in another
module branches to that address: whatever overlay is in memory then runs. The delinker records
such relocations as `module:overlays(a,b,...)` and the decomp names the target after one of
them (ov029's player-slot hooks all say func_ov088_020ba7c0, ov088 being Axel's slot-3 code).
On the port every overlay is linked at its own place, so the call has to pick the one loaded
now: a dispatcher per address (build/gen/ds_bss.S, platform/nitro/overlay.c resolves) and, in
the callers' sources, the name mapped to it (tools/rewrite_decomp.py pass ovdisp).
"""
import re
from collections import defaultdict
from pathlib import Path

SYM = re.compile(r"(\S+) kind:(\S+) addr:(0x[0-9a-f]+)")
AMBIG = re.compile(r"to:(0x[0-9a-f]+) module:overlays\(([^)]*)\)")


def _symbols(path):
    out = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            m = SYM.match(line)
            if m:
                out.setdefault(int(m.group(3), 16), []).append((m.group(1), m.group(2)))
    return out


def targets(cfg):
    """[(address, [(overlay id, function name)])] for the shared addresses called across
    modules where at least one candidate has a function there."""
    cfg = Path(cfg)
    found = set()
    for rf in [cfg / "relocs.txt"] + sorted((cfg / "overlays").glob("*/relocs.txt")):
        if not rf.exists():
            continue
        for line in rf.read_text(encoding="utf-8").splitlines():
            m = AMBIG.search(line)
            if m:
                found.add((int(m.group(1), 16), tuple(int(x) for x in m.group(2).split(","))))
    cache = {}
    merged = defaultdict(dict)
    for addr, cands in found:
        for ov in cands:
            mod = f"ov{ov:03d}"
            if mod not in cache:
                cache[mod] = _symbols(cfg / "overlays" / mod / "symbols.txt")
            for name, kind in cache[mod].get(addr, []):
                if kind.startswith("function"):
                    merged[addr][ov] = name
                    break
    return [(a, sorted(c.items())) for a, c in sorted(merged.items()) if c]


def alias(name):
    return f"kh_ovd_{name}"


def dispatcher_asm(tgts):
    """The dispatchers: keep r0-r3 and the stack, ask kh_ovdisp_resolve which candidate is
    loaded, branch there."""
    out = ["", "@ ---- shared-address overlay calls (tools/ovdisp.py) ----", ".text", ".arm"]
    for addr, cands in tgts:
        d = f"kh_ovdisp_{addr:08x}"
        out += [f".global {d}", f".type {d}, %function", ".balign 4", f"{d}:",
                "    push {r0-r4, lr}",
                f"    ldr r0, =.Lovd_{addr:08x}",
                "    bl kh_ovdisp_resolve",
                "    mov ip, r0",
                "    pop {r0-r4, lr}",
                "    bx ip",
                ".ltorg"]
        names = sorted({n for _, n in cands})
        for n in names:
            out += [f".global {alias(n)}", f".type {alias(n)}, %function", f".set {alias(n)}, {d}"]
    out += [".section .rodata.kh_ovdisp", ".balign 4"]
    for addr, cands in tgts:
        out += [f".Lovd_{addr:08x}:", f"    .word 0x{addr:08x}, {len(cands)}"]
        for ov, n in cands:
            out += [f"    .weak {n}", f"    .word {ov}, {n}"]
    return out
