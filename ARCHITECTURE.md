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

**Fixed addresses.** `tools/rewrite_decomp.py hw` wraps every literal the code uses as a
hardware or shared-area address in `KH_HW(addr)`. A literal counts as an address when it follows
a pointer cast, when it is the value of a register-address `#define`, when it is the first
argument of a register helper, or when it is a `HW_MAIN_MEM + 0x7ffxxx` sum; a few sites were
reviewed by hand. Masks and flags with the same values (`0x04000000` is also a texture-format
bit) are left alone, and `--report` lists them. On the DS `KH_HW` is the identity
(`include/nitro/kh_hw.h` in the patch), so the matching build does not change.
On the Vita it is a chain of constant comparisons (`platform/compat/kh_hw_map.h`). With a
constant address it folds to `symbol + offset`, so it costs nothing and stays usable in static
initializers. The result is an integer, like the literal was.

**Registers computed on read** (the divider and square-root results, DISPSTAT/VCOUNT) map through
a function that refreshes them first. Any other address resolves at compile time.

**Registers with side effects on write** (the GX FIFO, DMA, IPC, IE/IF, the card) are driven only
by NitroSDK functions (`libs/nitro`), apart from a handful of game files. The port replaces those
functions with native ones (`platform/nitro/`), because a plain memory store cannot trigger
anything.

**VRAM banking.** Banks A–I live in their LCDC slots. When VRAMCNT maps a bank into a view the CPU
can see (engine A/B BG or OBJ), its contents are copied there. They are copied back when it
leaves. Texture, palette and ARM7 mappings leave it home, where the renderer reads it. The
NitroSDK functions that write VRAMCNT run `kh_vram_sync()` on return: the `vram-sync` pass adds
a `cleanup` variable to them. Remaps are rare, and the game's pointer arithmetic within a view
stays valid.

Rendering:

- **2D engines A and B**: a scanline renderer in C (BG text/affine/bitmap, OBJ, windows,
  blending, master brightness) writes RGBA into the two 256×192 screens. It runs on its own
  core.
- **3D engine**: the geometry engine (matrices, lighting, clip) runs in C from the FIFO
  commands. Its polygons are drawn by vitaGL into an FBO, with textures decoded from texture VRAM
  and cached by address and parameters. The result becomes BG0 of engine A.
- **Presentation**: `video.c` draws both screens scaled; the layouts are side by side, top
  focus and bottom focus.

## The ARM9 as the NitroSDK sees it (platform/nitro/cpu.c)

**Threads.** Each NitroSDK thread (`OSThread`) runs on its own Vita thread, but only one holds
the baton at a time. The others wait on a semaphore. The SDK's own scheduler, compiled from the
decomp, still picks who runs. The port replaces only the switch:
- `OS_SaveContext` binds the calling Vita thread to its `OSThread`.
- `OS_LoadContext` hands the baton to the next thread and parks the caller.

When a parked thread is chosen again, `OS_LoadContext` returns into `OSi_RescheduleThread`.
That lands where `OS_SaveContext` would have returned TRUE on the DS.

**Interrupts.** Hardware events set IF bits from any Vita thread: VBlank from the display loop,
timer overflows, DMA ends, and card and ARM7 completions. They are delivered on the baton holder
at the points where the DS could take them and the port can observe:
- interrupts being re-enabled;
- the idle thread's `OS_Halt`, which sleeps until something is pending;
- code spinning on DISPSTAT or VCOUNT.

Delivery follows `OS_IrqHandler`. The lowest bit goes first and IF is acknowledged. The handler
from the DTCM vector table runs in IRQ mode (`OS_GetProcMode` reports it, so reschedules are
deferred). Then `OSi_IrqThreadQueue` is woken and a pending reschedule is done.

**Timers** (`platform/hw/timers.c`) run from the Vita's microsecond clock. They notice how they
were programmed on their next access or at the next delivery point. The tick (`OS_GetTick`) reads
the clock directly.

**DTCM** is one 16 KiB block with the DS layout (`kh_ds_dtcm`): the SDK reaches its vector table,
check word and stacks as base + offset.

**Arenas** are host memory of the DS's sizes (`platform/nitro/os.c`).

**DMA** (`platform/nitro/dma.c`). The MI functions perform a transfer when it is started, because
the SDK waits on the enable bit through saved pointers. Immediate transfers copy. GX-FIFO transfers
feed the geometry FIFO. Asynchronous ones complete like the DMA-end interrupt.

## The ARM7 (platform/nitro/arm7.c)

The ARM9 talks to the ARM7 only through the PXI FIFO. The port replaces `PXI_InitFifo` and
`PXI_SendWordByFifo` and answers on the ARM7's behalf. A reply runs the tag's receive callback
like the IPC interrupt did. Every tag reports a handler. What each tag does so far:
- **SOUND:** walks the command lists and advances the finished-command tag. The sound engine
  proper is still to come.
- **RTC:** returns the Vita's local time.
- **Touch panel, power management and NVRAM:** requests are acknowledged, and touch sampling
  reports the front panel. The boot writes touch calibration points so that raw = pixel × 16 / 21.

## Display loop (platform/hw/game.c)

The Vita's main thread is the DS's display. Every Vita VBlank it:
- writes the controls into KEYINPUT, the X/Y word and the touch sample;
- stamps the VBlank start for VCOUNT;
- raises the VBlank interrupt when DISPSTAT enables it;
- presents the screens.

Until the 2D engines exist, each screen shows its engine's backdrop colour under master
brightness, which is enough to follow the game's fades.

## Diagnostics

The kubridge fault handler (`platform/core/fault.c`) logs every register and the run-time address
of `main`. `tools/symbolize.py log.txt` maps them to functions and lines in the matching ELF.

## Files, the card and overlays

The NitroSDK FS library keeps working unchanged: it walks the ROM's FNT/FAT itself. Only
`CARDi_ReadRom`, the single path every ROM read takes, is replaced (`platform/nitro/card.c`). It
reads from the dump and completes asynchronous reads at the next interrupt point.
`CARDi_ReadRomIDCore` answers with the card ID the boot left in the shared area.

All overlays are linked in. `build/gen/overlays.ld` groups each overlay's sections, so the port
knows where an overlay lives (`platform/nitro/overlay.c`):
- `FS_LoadOverlayImage` restores the overlay's `.data` from a snapshot taken at boot and clears
  its `.bss`.
- `FS_StartOverlay` reports the overlay's entry function as `ram_address` (the game jumps there).
- `FS_EndOverlay` runs the global destructors registered from inside the overlay.

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
