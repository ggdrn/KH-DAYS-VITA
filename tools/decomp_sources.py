"""Which of the decomp's sources the Vita build compiles.

Everything under src/ and libs/ is game or library C and is compiled, except:

- the libraries' own assembly (`asm_stubs/`): CodeWarrior `asm` functions for BIOS calls, the
  boot code, cache and CP15 maintenance. platform/nitro provides native versions;
- DS Protect (ov028): the cartridge anti-tamper checks, meaningless off a DS card. The port
  reports "genuine" to its callers;
- REPLACED: library functions that drive hardware with side effects (GX FIFO, DMA, divider,
  interrupts, card). platform/nitro defines them instead.

First trial compile (decomp 497f599a9, GCC 15.2, GAME_CFLAGS): 24,610 files, 130 failures. 98
are asm_stubs and 4 are DS Protect. The other 28 need a PLATFORM_VITA change in the decomp patch:
CodeWarrior inline `asm { clz }` (5) and cache flush (1), `static` after an `extern`
declaration (18), CodeWarrior's cast-as-lvalue `((u8 *)p)++` (3), assignment to an array (1).
"""
from pathlib import Path

EXCLUDED_DIRS = (
    "asm_stubs",
    "ov028_dsprotect",
)

# Paths relative to the decomp root. Grows as platform/nitro takes functions over.
REPLACED = set()


def game_sources(decomp: Path):
    srcs = []
    for top in ("src", "libs"):
        for p in sorted((decomp / top).rglob("*.c")):
            rel = p.relative_to(decomp).as_posix()
            if any(f"/{d}/" in f"/{rel}" for d in EXCLUDED_DIRS):
                continue
            if rel in REPLACED:
                continue
            srcs.append(p)
    return srcs
