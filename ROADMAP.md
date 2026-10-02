# Roadmap

Phases follow the generic "port a decompiled game to the Vita" plan. ✅ done · 🔶 in progress · ⬜ to do.

## 1. Environment ✅
- ✅ VitaSDK 2026.08 (GCC 15.2) via `vdpm` bootstrap in `~/vitasdk`
- ✅ vitaShaRK, libmathneon, kubridge, taihen, SceShaccCgExt; vitaGL built with `HAVE_GLSL_SUPPORT=1`
- ✅ EU dump verified (`YKGP`, SHA-1 `6fae8f5b…`), the one the decomp matches
- ⬜ Console: `libshacccg.suprx` in `ur0:data/`, kubridge plugin

## 2. Reference build 🔶
- ✅ `tools/check_decomp_match.sh` (wraps the decomp's configure/ninja/`dsd check`/`build_rom.sh`)
- 🔶 DS toolchain in an OrbStack Linux machine (`tools/ds_reference/setup.sh`: wine, dsd,
  Python modules, binutils); WineHQ's macOS casks were disabled (Gatekeeper). ⬜ CodeWarrior
  binaries (mwccarm 3.0_patch4, 2.0/sp2p2, 1.2/sp4; mwldarm 2.0/sp2p4)

## 3–4. Strategy / game code changes 🔶
- ✅ Whole decomp compiles for the Vita (24,514 C/C++/asm sources); the 26 C failures fixed under
  `PLATFORM_VITA`, MSL strcmp/strcpy replaced by newlib's (`tools/decomp_sources.py`)
- ✅ `tools/rewrite_decomp.py abs-symbols`: linker-absolute constants (OVERLAY_n_ID, SDK_*) as
  address-typed macros (35 files)
- ✅ `tools/rewrite_decomp.py hw`: fixed DS addresses wrapped in `KH_HW()` wherever they are
  used as addresses (461 files; masks and flags with the same values left alone, reviewed with
  `--report`); `vram-sync`: VRAMCNT writers move the banks on exit
- ✅ CodeWarrior pragmas (`#pragma thumb`, `opt_*`) need nothing: GCC ignores them

## 5. Vita build 🔶
- ✅ `configure_vita.py` → ninja → ELF → velf → eboot → param.sfo → VPK, versioned from `VERSION`
- ✅ LiveArea placeholders (8-bit palettized, exact sizes) from `tools/make_livearea.py`
- ✅ The whole game links into the VPK (5.2 MB of code, 0 unresolved symbols):
  - one archive per module (host file-descriptor limit), `-Dmain=NitroMain`
  - `tools/weaken_unowned.py`: delinks.txt section ownership for data defined twice
  - `tools/gen_link_support.py`: every module's .bss as one block with the DS layout (487
    symbols), aliases, and `kh_overlays[]` (load address, sizes, .bss block, entry function)
    from the ROM's overlay table
  - `platform/nitro/asm_replacements.c`: the 70 CodeWarrior-asm library routines in C
    (copies, matrices, MATH_QSort, streaming LZ, SHA-1, CP context, interrupt state, MobiClip
    blit); `platform/nitro/dsprotect.c`: DS Protect answers "genuine"
- ✅ Hardware addresses translated; a scan of the linked ELF finds no DS address left
- 🔶 Console: boots to `NitroMain` (0.0.8); 0.0.9 past the GBA-slot probe and into the first SDK thread; 0.0.10 pins the shared-bss statics (OSi_CurrentThreadPtr was NULL); 0.0.11 = 0.0.10 on decomp ea39ead5e; 0.0.12 defines those names in assembly (--defsym block+off was absolute, not relocated); 0.0.13 gives 64-bit struct members the DS 4-byte alignment (OSThread.state was at +0x68); 0.0.15 completes async card reads without IME (main loads ov001 before interrupts are on); 0.0.17 keeps the I bit per thread across switches and reaches the frame loop (IME on, both screens configured); 0.0.18 renders 2D ~5x faster; 0.0.19 wakes OS_WaitIrq sleepers on the right queue (the frame loop hung on its first VBlank); 0.0.20 unwraps REG_G3X_GXSTAT_GE_MASK (G3X_ResetMtxStack spun forever) and loads the title overlay (ov000); 0.0.21 answers its CARD_IdentifyBackup (the save works); 0.0.22 unpacks archive handles (pointers packed into 24 bits); 0.0.24 tells file names from handles on the Vita (bit 31 is set on every Vita pointer) and reads the boot text; 0.0.25 completes card reads with the DS timing (task path at once, DMA path later) and loads the font; 0.0.26 keeps the DS data layout (GCC reordered and realigned objects), shows the boot text, writes the save and loads the title pack; 0.0.27 gives IRQ-mode code its own I bit and reaches the title and New Game; 0.0.28 plays MobiClip with the portable decoder and reaches the field (ov002, ov007, ov023); 0.0.29 gives NNS G3D its two ITCM-gap function tables; 0.0.30 runs the geometry engine (polygon lists built, not drawn yet); 0.0.31 delivers SWAP_BUFFERS (sent through main.c's own register macro) and draws the 3D layer on the GPU; 0.0.32 builds the shaders as CG; 0.0.33 links data in DS order across files (the field's pause/HUD read a config 12 bytes into the next file) and renders 2D on two cores; 0.0.34 completes the GX DMA callback at once (NNS spun on its busy flag), raises VBlank from its own 60 Hz thread and shares 2D chunks between cores; 0.0.42 dispatches calls to addresses several overlays share (mission partners were always Axel), draws 2D only on new game frames, 3D at 3x; 0.0.51 shares cached resources again (names were stored as ids: textures ran out); 0.0.52 plays sound (the ARM7 sound driver, high-level)

## 6. Platform layer 🔶
- ✅ main/system: clocks, data dir, log + previous log, ROM open/verify (cached SHA-1), msg dialog
- ✅ video: two 256×192 textures on vitaGL, three layouts
- ✅ input: positional buttons, stick as d-pad, touch mapped to the bottom screen, X/Y ext word
- ✅ hw memory map (`platform/compat/kh_hw_map.h`, `platform/hw/memmap.c`): I/O page, palette,
  OAM, VRAM views (engine A/B BG and OBJ, LCDC), shared area; divider, square root and
  DISPSTAT/VCOUNT computed on read; power-on register values; GBA slot as an empty slot
  (all 0xff) and the ARM9 BIOS logo area, for CTRDG's cartridge probe
- ✅ VRAM banks A–I (`platform/hw/vram.c`): home in LCDC, copied into the CPU-visible view they are
  mapped to and back on remap
- ✅ platform/nitro/cpu.c: NitroSDK threads on Vita threads (one baton, the SDK's scheduler),
  interrupt delivery (IME/IE/IF, IRQ mode, IRQ thread queue, deferred reschedule), OS_Halt
- ✅ timers 0-3 from the Vita clock; OS tick from the clock; DTCM as one DS-layout block; arenas
- ✅ DMA performed at start (immediate, GX FIFO); async completions as interrupts
- ✅ CARD ROM reads from the dump; card ID; overlays resident with per-overlay sections
  (`build/gen/overlays.ld`): .data restore, .bss clear, entry as ram_address, destructors
- ✅ ARM7 over PXI: every tag ready; SOUND command lists acknowledged; CTRDG module info; RTC (Vita local time);
  touch/PM/NVRAM requests acknowledged; touch sampling with calibration
- ✅ GX/G3: geometry engine (platform/hw/gx3d.c): packed FIFO and command ports (direct stores via the decomp's `KH_GX_CMD`), matrix stacks, lighting, texcoord generation, primitive assembly and culling, box/position/vector tests, clip/vector matrix read-back; host test in tools/gx3d_test
- ⬜ VBlank/HBlank DMA timings, wireless
- 🔶 2D renderer (engines A/B), `platform/hw/gpu2d.c`: text/affine/extended BGs (tiles, 256-colour and
  direct bitmaps, extended palettes), sprites (normal, affine, double size, bitmap,
  semi-transparent, OBJ window), windows, blending, master brightness, display modes;
  host test with synthetic scenes (`tools/gpu2d_test/run.sh`). ⬜ mosaic, mid-frame (HBlank)
  register changes, the large bitmap of mode 6, the 3D layer on BG0
- 🔶 3D: geometry engine, texture decoding + cache (hw/textures.c), vitaGL rendering at 1-3x (config.ini render_scale) with toon/highlight/decal and alpha test, GPU composition with engine A's layers; not yet: shadow polygons, wireframe, fog, edge marking, w-buffer, threading
- ⬜ Sound: native ARM7 SND (sequence player, channels, ADPCM/PSG, mixer, 48 kHz out)
- 🔶 MobiClip videos: the frame decoder is the decomp's portable C++ model
  (`libs/mobiclip/video/portable`, ARM9 state layout asserted in `abi_check.c`), checked on the
  host on the user's dump (`tools/mobiclip_test/run.sh`: 802.mods, 1400 frames). ⬜ audio (IMA
  ADPCM, with the rest of the sound)
- 🔶 Save (`platform/nitro/backup.c`): the ARM7's backup service over PXI (identify, read,
  program, verify, erase) on `days.sav`, raw chip image compatible with emulator saves;
  written a second after the last change, atomically. ⬜ flush on suspend

- 🔶 Data layout (`tools/check_layout.py`): objects of one file keep their DS order and
  spacing (`-fno-toplevel-reorder`; data files at `-Os -fno-zero-initialized-in-bss`):
  1384 differing pairs down to 19, all in overlays off the boot path (field, MobiClip,
  camp menu 2, enemies: objects declared at a size other than the DS's)

## 7. Diagnostics 🔶
- ✅ kubridge fault handler (all registers, run-time `main` for the load bias)
- ✅ `tools/symbolize.py`: log addresses → function, file, line
- ✅ watchdog: SDK thread list with each parked thread's call stack (EHABI unwind tables,
  `-funwind-tables`), resolved by `tools/symbolize.py`
- ✅ boot trace: `KH_TRACE` milestones in main(), card reads by ROM file name (FNT/FAT),
  changes of IME/IE/DISPCNT/POWCNT1/VRAMCNT per frame
- ⬜ `psp2dmp` reader; network log and eboot push (vitacompanion)

## 8–10. Bring-up loop, performance, enhancements ⬜
- First milestone: title screen (ov000) with 2D, input and music

## 11. Distribution
- ✅ Repository holds no game data, no decomp sources (patch only), no Sony modules
- ✅ ROM read from `ux0:data/khdays/` at runtime, SHA-1 checked
- ✅ MobiClip's ROM machine code (`ov024/data/mobiclip_*.s`) is no longer linked
- ⬜ Audit step that fails the build if ROM bytes end up in the ELF

## 12. Organisation ✅
- ✅ `setup.sh` / `export_patch.sh` / `DECOMP_COMMIT`, README, ARCHITECTURE
