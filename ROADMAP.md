# Roadmap

Phases follow the generic "port a decompiled game to the Vita" plan. ✅ done · 🔶 in progress · ⬜ to do.

## 1. Environment ✅
- ✅ VitaSDK 2026.08 (GCC 15.2) via `vdpm` bootstrap in `~/vitasdk`
- ✅ vitaShaRK, libmathneon, kubridge, taihen, SceShaccCgExt; vitaGL built with `HAVE_GLSL_SUPPORT=1`
- ✅ EU dump verified (`YKGP`, SHA-1 `6fae8f5b…`), the one the decomp matches
- ⬜ Console: `libshacccg.suprx` in `ur0:data/`, kubridge plugin

## 2. Reference build 🔶
- ✅ `tools/check_decomp_match.sh` (wraps the decomp's configure/ninja/`dsd check`/`build_rom.sh`)
- ⬜ mwccarm + dsd available (Windows tools: wine or a Windows machine)

## 3–4. Strategy / game code changes 🔶
- ✅ Trial compile of the whole decomp for the Vita: 24,610 files, 28 real C failures (see
  `tools/decomp_sources.py`)
- ⬜ Fix the 28 under `PLATFORM_VITA` (clz/cache asm, static-after-extern, cast-lvalue)
- ⬜ `tools/hwrewrite.py`: wrap fixed DS addresses in `KH_HW()` (373 files)
- ⬜ `platform/compat/`: CodeWarrior-isms (`#pragma thumb`, `asm` qualifiers) neutralised

## 5. Vita build 🔶
- ✅ `configure_vita.py` → ninja → ELF → velf → eboot → param.sfo → VPK, versioned from `VERSION`
- ✅ LiveArea placeholders (8-bit palettized, exact sizes) from `tools/make_livearea.py`
- ⬜ Link the game: resolve symbol clashes and unresolved externs, overlay `.data`/`.bss` sections
  with start/end symbols for `FS_LoadOverlay` resets

## 6. Platform layer 🔶
- ✅ main/system: clocks, data dir, log + previous log, ROM open/verify (cached SHA-1), msg dialog
- ✅ video: two 256×192 textures on vitaGL, three layouts
- ✅ input: positional buttons, stick as d-pad, touch mapped to the bottom screen, X/Y ext word
- ⬜ hw memory map: I/O page, palette, OAM, VRAM views with bank remapping, shared area 0x027fxxxx
- ⬜ platform/nitro: OS (threads, IRQ/VBlank, alarms, ticks), MI (DMA/copies), CP (div/sqrt), GX/G3
  (FIFO → geometry engine), CARD (ROM reads, backup), PXI/SND hookup, BIOS SWIs, cache no-ops
- ⬜ 2D renderer (engines A/B)
- ⬜ 3D: geometry engine + vitaGL rasterisation + texture cache
- ⬜ Sound: native ARM7 SND (sequence player, channels, ADPCM/PSG, mixer, 48 kHz out)
- ⬜ MobiClip videos (the decoder is C in ov024: needs only frame output + audio)
- ⬜ Save: backup ↔ `days.sav`, atomic writes, save on suspend

## 7. Diagnostics ⬜
- ⬜ kubridge fault handler, watchdog, `psp2dmp` reader, log-address mapper (addr2line)

## 8–10. Bring-up loop, performance, enhancements ⬜
- First milestone: title screen (ov000) with 2D, input and music

## 11. Distribution
- ✅ Repository holds no game data, no decomp sources (patch only), no Sony modules
- ✅ ROM read from `ux0:data/khdays/` at runtime, SHA-1 checked
- ⬜ Audit step that fails the build if ROM bytes end up in the ELF

## 12. Organisation ✅
- ✅ `setup.sh` / `export_patch.sh` / `DECOMP_COMMIT`, README, ARCHITECTURE
