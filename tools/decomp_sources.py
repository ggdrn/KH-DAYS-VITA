"""Which of the decomp's sources the Vita build compiles.

Everything under src/ and libs/ is game or library code and is compiled: C, the MobiClip
player's C++ (ov024), and the assembly already in GNU syntax (`.s`: the CodeWarrior runtime's
division helpers, MobiClip's audio transform and its hand-written ARM kept as data). Except:

- CodeWarrior-syntax assembly (`asm_stubs/*.c`): BIOS calls, boot code, cache and CP15
  maintenance, interrupt state, the MI copy/fill primitives. platform/nitro/asm_replacements.c
  provides native versions;
- DS Protect (ov028): the cartridge anti-tamper checks, meaningless off a DS card. The port
  reports "genuine" to its callers;
- REPLACED: library functions that drive hardware with side effects (GX FIFO, DMA, divider,
  interrupts, card). platform/nitro defines them instead.

First trial compile (decomp 497f599a9, GCC 15.2, GAME_CFLAGS): 24,610 files, 130 failures. 98
are asm_stubs and 4 are DS Protect. Of the other 28, MSL's strcmp/strcpy are replaced by newlib's
and 26 are fixed under PLATFORM_VITA in patches/decomp.patch: CodeWarrior inline `asm { clz }` (5)
and cache flush (1), `static` after an `extern` declaration (18), CodeWarrior's cast-as-lvalue
`((T *)p)++` (1), assignment through an array type (1).
"""
from pathlib import Path

EXCLUDED_DIRS = (
    "asm_stubs",
    "ov028_dsprotect",
)

# Paths relative to the decomp root. Grows as platform/nitro takes functions over.
REPLACED = {
    # MSL's standard C functions: newlib has the same names, and the Vita's own code (the log,
    # vitaGL, stdio) must get newlib's. MSL's strlen is also a loop GCC turns into a call to
    # strlen -- itself (tools/check_elf.py) -- and strcmp/strcpy use CodeWarrior's cast-as-lvalue.
    "libs/msl/c/auto/strcmp.c",
    "libs/msl/c/auto/strcpy.c",
    "libs/msl/c/auto/strncpy.c",
    "libs/msl/c/auto/abs.c",
    "libs/msl/c/calls/strlen.c",
    "libs/msl/c/calls/strncmp.c",
    "libs/msl/c/calls/strtol.c",
    "libs/msl/c/calls/__strtoul.c",
    # ov024's MobiClip payloads: ROM machine code kept as data, which ran in place on the DS.
    # The frame decoder is the portable C++ model (libs/mobiclip/video/portable, through
    # Ov024_MobiClip_GetDecoderCodeCached); the deblocking filter and FastAudio never run in this
    # game (platform/nitro/mobiclip.c stands in). Leaving them out also keeps their ROM bytes out
    # of the eboot.
    "src/overlays/system/ov024_mobiclip/data/mobiclip_payload.s",
    "src/overlays/system/ov024_mobiclip/data/mobiclip_deblock.s",
    "src/overlays/system/ov024_mobiclip/data/mobiclip_fastaudio.s",
}

# Library functions (one file each, named after the function) that platform/nitro defines
# natively because their DS versions drive hardware or return DS addresses.
REPLACED_FUNCS = {
    # os.c: arenas in host memory, the tick from the Vita's clock
    "OS_GetInitArenaLo", "OS_GetInitArenaHi", "OS_InitTick", "OS_GetTick", "OS_GetTickLo",
    # cpu.c: IF is write-1-to-clear
    "OS_ResetRequestIrqMask",
    # arm7.c: the PXI FIFO goes to the port's ARM7
    "PXI_InitFifo", "PXI_SendWordByFifo",
    # card.c: ROM reads from the dump
    "CARDi_ReadRom", "CARDi_ReadRomIDCore",
    # dma.c: transfers happen when they are started
    "MI_DmaCopy16", "MI_DmaCopy32", "MI_DmaCopy32Async", "MI_DmaFill32", "MI_DmaFill32Async",
    "MI_SendGXCommandAsync", "MI_SendGXCommandAsyncFast", "MIi_CardDmaCopy32", "MI_StopDma",
    "MI_WaitDma", "MIi_DmaSetParams", "MIi_DmaSetChannelRegs", "func_01ff85d0", "func_01ff8664",
    # overlay.c: overlays are resident; loading resets their state
    "FS_LoadOverlayImage", "FS_StartOverlay", "FS_EndOverlay", "FS_ClearOverlayImage",
}


def game_sources(decomp: Path):
    """(path, kind) with kind "c", "cpp" or "s"."""
    srcs = []
    for top in ("src", "libs"):
        for ext, kind in (("c", "c"), ("cpp", "cpp"), ("s", "s")):
            for p in sorted((decomp / top).rglob(f"*.{ext}")):
                rel = p.relative_to(decomp).as_posix()
                # .s under asm_stubs is GNU syntax and stays; only CodeWarrior .c asm goes
                if kind != "s" and any(f"/{d}/" in f"/{rel}" for d in EXCLUDED_DIRS):
                    continue
                if kind == "s" and "ov028_dsprotect" in rel:
                    continue
                # the portable MobiClip decoder is built: it replaces the payload on the Vita
                if rel in REPLACED:
                    continue
                if rel.startswith("libs/") and p.stem in REPLACED_FUNCS:
                    continue
                srcs.append((p, kind))
    return srcs


def module_of(rel):
    """Archive a source is linked from: src/engine -> main, src/overlays/<kind>/ovNNN_x ->
    ovNNN_x, libs/<vendor>/<module> -> <vendor>_<module>."""
    parts = Path(rel).parts
    if parts[0] == "src" and parts[1] == "engine":
        return "main"
    if parts[0] == "src" and parts[1] == "overlays":
        return parts[3]
    return f"{parts[1]}_{parts[2]}"
