# Architecture

How the DS game becomes a Vita program. [ROADMAP.md](ROADMAP.md) tracks what is done.

## Why recompiling works here

The DS's ARM946E-S and the Vita's Cortex-A9 are both 32-bit little-endian ARM with the AAPCS
calling convention. Pointers and `int` are both 4 bytes, and the decomp's structs keep their
offsets as long as enums stay 32 bits (`-fno-short-enums`, like mwccarm's `-enum int`) and
`char` stays signed. A first trial compile of the whole decomp (24,610 C files) with
`arm-vita-eabi-gcc` failed on 204 files. 94 of those are the libraries' CodeWarrior-syntax
assembly, which the port replaces anyway. Most of the rest are diagnostics that GCC 15 promotes to
errors (`int-conversion`, `return-mismatch`, …), and flags turn them back into warnings.

## Two trees

```
khdays-vita/            this repository (public): port code, tools, patch, docs
  platform/core/        Vita side: main, log, ROM, video, input, message dialog
  platform/hw/          the DS machine: memory map, 2D/3D engines, DMA, timers, IRQs   (planned)
  platform/nitro/       native replacements for hardware-bound NitroSDK functions      (planned)
  platform/compat/      headers forced into the game build (CodeWarrior-isms)          (planned)
  patches/decomp.patch  the port's changes to the decomp, all under PLATFORM_VITA
  tools/                build, LiveArea, reference check, crash-dump tools
build/decomp/           local build tree = decomp @ DECOMP_COMMIT + decomp.patch (never pushed)
```

## Boot

1. `platform/core/main.c`: clocks to 444/222/222/166 MHz, the data directory, the log, vitaGL.
2. `rom.c` opens `ux0:data/khdays/days.nds`, checks size, gamecode and SHA-1, and caches the
   result. On failure a system message dialog says what is wrong.
3. With the game linked (`KH_WITH_GAME`), `kh_game_run()` sets up the emulated DS state and
   runs the decomp's `main` (NitroMain) on its own thread. Without it, a diagnostic screen shows
   the input.

## The DS machine (platform/hw)

The decomp talks to hardware in three ways, and each is handled differently.

**Fixed addresses cast to pointers** (`*(vu16 *)0x04000304 |= 0x8000`, `reg_GX_DISPCNT`, the
VRAM at `0x06000000`, the shared area at `0x027fxxxx`). A rewrite script (`tools/hwrewrite.py`,
planned) wraps every such literal in `KH_HW(addr)`. Under `PLATFORM_VITA` that macro resolves to
host memory: the I/O page, palette, OAM and VRAM views. On the DS it expands to the literal, so
the matching build does not change. For a constant address it folds to `symbol + offset`, so it
costs nothing.

**Registers with side effects** (the GX command FIFO, DMA, the divider and square root, IPC/PXI,
IE/IF, VRAMCNT). The functions that drive them are small and live in the NitroSDK: `G3_*`/`GX_*`,
`MI_Dma*`, `CP_*`, `PXI_*`, `OS_*Interrupt*`. The port replaces those functions with native ones
(`platform/nitro/`) that call into the emulated machine directly. A write to a plain memory copy
of a register would have no effect, so these cannot be left to the rewrite.

**VRAM banking.** Banks A–I move between LCDC, BG, OBJ, texture and palette slots through
VRAMCNT. Each bank lives at one host address at a time: its current mapping inside a flat view
per region (LCDC, BG-A, OBJ-A, BG-B, OBJ-B, texture, texture palette, extended palettes).
Remapping a bank copies its 16–128 KiB from the old view to the new one. Games remap rarely,
and this keeps pointer arithmetic by the game valid inside a region.

Rendering:

- **2D engines A and B**: a scanline renderer in C (BG text/affine/bitmap, OBJ, windows,
  blending, master brightness) writes RGBA into the two 256×192 screens. It runs on its own
  core.
- **3D engine**: the geometry engine (matrices, lighting, clip) runs in C from the FIFO
  commands. Its polygons are drawn by vitaGL into an FBO, with textures decoded from texture VRAM
  and cached by address and parameters. The result becomes BG0 of engine A.
- **Presentation**: `video.c` draws both screens scaled; the layouts are side by side, top
  focus and bottom focus.

## Threads and timing

The DS main loop waits for VBlank (`OS_WaitVBlankIntr`). The port runs the game on core 0 at
60 Hz. A VBlank thread raises the emulated VBlank IRQ, runs the registered handlers and
presents the frame. NitroSDK threads (`OS_CreateThread`, used by the file loader and sound)
become Vita threads with the same priorities mapped onto the Vita's range.

## Linking the game

- Game objects are archived per module (main, each overlay, each library) and linked with
  `--whole-archive`; `--gc-sections` then drops what nothing reaches.
- Some variables are defined in two sources (the function that uses one and the `data/` file
  for its address). dsd's `delinks.txt` says which file each section comes from, and
  `tools/weaken_unowned.py` applies the same rule by making the other copies weak.
- Most `.bss` exists in the decomp only as addresses. `tools/gen_link_support.py` emits each
  module's `.bss` as one block with the DS layout, a label per symbol, so that neighbours stay
  adjacent. It also emits `kh_overlays[]` from the ROM's overlay table.
- The game enters an overlay by calling its load address (`FSOverlayInfo.ram_address`), so each
  entry in `kh_overlays[]` carries the function at that address. The native `FS_*Overlay*` hands
  that out instead of a DS address.

## Files and the card

The NitroSDK FS library keeps working unchanged: it walks the ROM's FNT/FAT itself. Only the
bottom of the card path (`CARD_ReadRom` and friends) is replaced with `rom_read()`, which reads
from the dump with `sceIoPread`. Overlays are all linked in. `FS_LoadOverlay` becomes "reset
that overlay's `.data`/`.bss` and run its static initializers", which is what loading one did on
the DS.

## Sound

The ARM9 side (NNS_Snd, NitroSDK SND) is compiled as is. The ARM7 side is not in the decomp, and
the port reimplements it natively: the SND command processor, the sequence player (SSEQ), the
16 channels (PCM8/16, IMA-ADPCM, PSG, noise) and the mixer, at 32,728 Hz resampled to 48 kHz.
It runs on a `sceAudioOut` thread. The ARM7 functions are listed in the decomp's
`docs/ARM7.md`.

## Save

The game's backup (card EEPROM/flash) goes through `CARD_*Backup*`. The port keeps it in
memory and writes it to `ux0:data/khdays/days.sav` with a temporary file and a rename. It also
writes on suspend, from the `scePowerRegisterCallback` callback.

## Diagnostics

`log.txt`/`log_prev.txt`, a kubridge fault handler for invalid accesses, a watchdog that forces
a core dump when the game thread stops advancing, and `tools/` scripts that map crash addresses
to file and line with `addr2line` on the build's ELF.
