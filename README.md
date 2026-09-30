# khdays-vita

A native PS Vita port of *Kingdom Hearts 358/2 Days* (Nintendo DS), built from the
[khdays-decomp](https://github.com/Yokimitsuro/khdays-decomp) matching decompilation.

The game code is **recompiled** for the Vita's Cortex-A9. It is not emulated. What the game
expected from the DS hardware (the 2D and 3D engines, the ARM7 sound processor, the card, the
touch screen) is reimplemented in a platform layer on top of VitaSDK and vitaGL.

> **Status: early bring-up.** The port shell boots, verifies the user's ROM and shows a
> controls/touch diagnostic screen. The game is not playable yet; see [ROADMAP.md](ROADMAP.md).

## No game data here

This repository contains only the port's own code, tools and a patch against the decomp. It does
**not** contain the ROM, assets, the decomp's sources or any Sony module. To play, you need your
own dump of the **European** cartridge:

| | |
|---|---|
| Gamecode | `YKGP` (Europe, En/Fr/De/Es/It) |
| Size | 268,435,456 bytes |
| SHA-1 | `6fae8f5bbe80114b4e2535260eab5f4d0fc8a844` |

The decomp is built against this exact dump, so other regions (the US `YKGE` included) will not
work.

## Installing on the Vita

1. HENkaku/Enso and VitaShell. Install `libshacccg.suprx` to `ur0:data/` (extract it from your
   own console; see the vitaGL README). **Never** redistribute it.
2. Install `khdays-vita-<version>.vpk` with VitaShell.
3. Copy your dump to `ux0:data/khdays/days.nds`.
4. Launch it. The first boot hashes the ROM (a few seconds). After that the check is cached in
   `ux0:data/khdays/rom.verified`.

Logs go to `ux0:data/khdays/log.txt`, and the previous run's log is kept as `log_prev.txt`.

Controls: the face buttons are positional, as on the DS (Circle = A, Cross = B, Triangle = X,
Square = Y). The left stick doubles as the d-pad. The front touch panel maps onto the bottom
screen. L+R+Select cycles the screen layout.

## Building

Requirements: [VitaSDK](https://vitasdk.org) (installed with `vdpm`'s `bootstrap-vitasdk.sh`),
plus `vitaShaRK`, `libmathneon`, `kubridge`, `taihen`, `SceShaccCgExt` and
[vitaGL](https://github.com/Rinnegatamante/vitaGL) built with `HAVE_GLSL_SUPPORT=1`. You also
need `ninja` and Python 3.

```sh
export VITASDK=~/vitasdk PATH=$VITASDK/bin:$PATH
vdpm install vitaShaRK libmathneon kubridge taihen SceShaccCgExt
(cd vitaGL && make HAVE_GLSL_SUPPORT=1 install)

# port shell only
python3 configure_vita.py && ninja

# with the game code (work in progress: links, does not run yet)
./setup.sh                      # decomp at DECOMP_COMMIT + patches/decomp.patch -> build/decomp
python3 tools/rewrite_decomp.py # mechanical PLATFORM_VITA rewrites (idempotent)
python3 configure_vita.py --with-game --rom ../days.nds && ninja
```

`--rom` (default `../days.nds`, or `$KH_ROM`) is read at configure time for the overlay table
only: load addresses, resolved to function names. No ROM bytes go into the build from it.

The VPK version comes from `VERSION`. Bump it for every build you put on the console.

### Working on the decomp side

- `./setup.sh` creates `build/decomp`, the local build tree.
- Edit the game code there. Every change goes under `#ifdef PLATFORM_VITA`, or behind a macro
  that expands to the original code on the DS.
- `./export_patch.sh` writes those changes back to `patches/decomp.patch`.
- `tools/check_decomp_match.sh` rebuilds the DS game from the patched tree with the original
  toolchain (mwccarm, dsd) and checks it is still byte-identical.

## Layout

See [ARCHITECTURE.md](ARCHITECTURE.md).

## Legal

*Kingdom Hearts* is © Disney and Square Enix. This is an unofficial fan project, not affiliated
with or endorsed by them. You must own the game.
