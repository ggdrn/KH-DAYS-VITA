#!/usr/bin/env python3
"""Generate build.ninja for the PS Vita port.

    python3 configure_vita.py                 # port shell only (boot, ROM check, diagnostics)
    python3 configure_vita.py --with-game     # + the decomp's C, from the build tree (setup.sh)
    ninja                                     # -> build/VPK/khdays-vita-<VERSION>.vpk

Game sources come from the build tree (the decomp with patches/decomp.patch applied), never
from this repository. What is compiled from it, and what the port replaces, is listed in
tools/decomp_sources.py.
"""
import argparse
import os
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))

TITLE = "Kingdom Hearts 358/2 Days"
TITLE_ID = "KHDD00358"

VITASDK = Path(os.environ.get("VITASDK", Path.home() / "vitasdk"))
BIN = VITASDK / "bin"

# The game code is compiled for the Cortex-A9 in ARM mode. -fsigned-char and -fshort-enums off
# (int enums) follow mwccarm's settings in the decomp's configure.py (-char signed, -enum int),
# so structs keep the layout the original compiler gave them. gnu11: the decomp relies on
# unprototyped `extern int f();` declarations, which C23 would read as `f(void)`.
COMMON_CFLAGS = [
    "-mcpu=cortex-a9", "-mfpu=neon", "-marm", "-mfloat-abi=hard",
    "-fsigned-char", "-fno-short-enums", "-fno-strict-aliasing", "-fwrapv",
    "-ffunction-sections", "-fdata-sections", "-g",
    "-DPLATFORM_VITA=1",
]
PORT_CFLAGS = ["-std=gnu11", "-O3", "-Wall", "-Wno-unused-function", "-funwind-tables"]
GAME_CFLAGS = [
    "-std=gnu11", "-O2", "-w", "-funwind-tables",
    # locals the ROM reads before setting (it gets whatever the register held) are 0: GCC would
    # otherwise treat such a read as any value it likes and fold branches on it
    "-ftrivial-auto-var-init=zero",
    # objects in source order, as mwcc emits them: the decomp's data files list a module's
    # objects in DS address order, and code reads from one into the next (tools/check_layout.py)
    "-fno-toplevel-reorder",
    # The decomp's K&R-style calls and int<->pointer casts are deliberate: mwccarm accepted
    # them, and GCC 14+ turns these diagnostics into errors by default.
    "-Wno-error=implicit-function-declaration", "-Wno-error=int-conversion",
    "-Wno-error=incompatible-pointer-types", "-Wno-error=implicit-int",
    "-Wno-error=return-mismatch",
]

GAME_CXXFLAGS = ["-std=gnu++11", "-O2", "-w", "-ftrivial-auto-var-init=zero", "-fno-exceptions", "-funwind-tables",
                 "-fno-toplevel-reorder", "-fpermissive"]

LIBS = [
    "-lvitaGL", "-lvitashark", "-lSceShaccCgExt", "-lmathneon", "-lstdc++", "-lm", "-lc",
    "-lkubridge_stub_weak", "-lSceVshBridge_stub", "-ltaihen_stub", "-lSceShaccCg_stub", "-lSceKernelDmacMgr_stub",
    "-lSceCommonDialog_stub", "-lSceGxm_stub", "-lSceDisplay_stub", "-lSceAppMgr_stub",
    "-lSceAppUtil_stub", "-lSceCtrl_stub", "-lSceTouch_stub", "-lSceAudio_stub",
    "-lScePower_stub", "-lSceRtc_stub", "-lSceSysmodule_stub", "-lSceLibKernel_stub",
]


def rel(p):
    return os.path.relpath(p, ROOT).replace(" ", "$ ")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--with-game", action="store_true", help="link the decomp's game code")
    ap.add_argument("--decomp", default=str(ROOT / "build" / "decomp"),
                    help="build tree: the decomp with the port patch applied (see setup.sh)")
    ap.add_argument("--rom", default=os.environ.get("KH_ROM", str(ROOT.parent / "days.nds")),
                    help="the EU dump; only its overlay table is read (addresses, no content)")
    args = ap.parse_args()

    version = (ROOT / "VERSION").read_text().strip()
    major, minor, patch = (version.split(".") + ["0", "0"])[:3]
    app_ver = f"{int(major):02d}.{int(minor) * 10 + int(patch):02d}"

    # abi_check.c needs the decomp's headers: built with the game's flags below
    port_srcs = sorted(p for p in (ROOT / "platform").rglob("*.c") if p.name != "abi_check.c")
    game_srcs = []
    defines = [f'-DKH_VERSION="{version}"']
    includes = [f"-I{ROOT / 'platform' / 'core'}", f"-I{ROOT / 'platform'}",
                f"-I{ROOT / 'platform' / 'compat'}"]
    game_includes = []
    if args.with_game:
        import decomp_sources
        decomp = Path(args.decomp).resolve()
        if not (decomp / "src").is_dir():
            sys.exit(f"{decomp} is not a decomp tree: run ./setup.sh first")
        game_srcs = decomp_sources.game_sources(decomp)
        rom = Path(args.rom).resolve()
        if not rom.is_file():
            sys.exit(f"{rom}: the EU ROM is needed to read its overlay table (--rom)")
        subprocess.run([sys.executable, str(ROOT / "tools" / "gen_link_support.py"), str(decomp),
                        str(ROOT / "build" / "gen"), str(rom)], check=True)
        defines.append("-DKH_WITH_GAME=1")
        defines_game = ["-Dmain=NitroMain"]
        game_includes = defines_game + [f"-I{ROOT / 'platform' / 'compat'}", f"-I{decomp / 'include'}"]
    else:
        port_srcs = [p for p in port_srcs if "platform/hw" not in p.as_posix()
                     and "platform/nitro" not in p.as_posix()]

    q = lambda xs: " ".join(shlex.quote(str(x)) for x in xs)
    out = []
    w = out.append
    w("# generated by configure_vita.py -- do not edit")
    w("ninja_required_version = 1.10")
    w(f"cc = {BIN / 'arm-vita-eabi-gcc'}")
    w(f"cxx = {BIN / 'arm-vita-eabi-g++'}")
    w(f"ar = {BIN / 'arm-vita-eabi-gcc-ar'}")
    # the version and the game switch only concern the port's code: a new VERSION must not
    # recompile the 24k game sources
    w(f"common = {q(COMMON_CFLAGS)}")
    w(f"port_cflags = {q(PORT_CFLAGS + defines + includes)}")
    w(f"game_cflags = {q(GAME_CFLAGS + game_includes)}")
    w(f"game_cxxflags = {q(GAME_CXXFLAGS + game_includes)}")
    w(f"libs = {q(LIBS)}")
    w("")
    w("rule cc_port\n  command = $cc $common $port_cflags -MMD -MF $out.d -c $in -o $out\n"
      "  depfile = $out.d\n  deps = gcc\n  description = CC $in")
    w("rule cxx_game\n  command = $cxx $common $game_cxxflags -MMD -MF $out.d -c $in -o $out\n"
      "  depfile = $out.d\n  deps = gcc\n  description = CXX $in")
    w("rule as_game\n  command = $cc $common -c $in -o $out\n  description = AS $in")
    w("rule cc_game\n  command = $cc $common $game_cflags -MMD -MF $out.d -c $in -o $out\n"
      "  depfile = $out.d\n  deps = gcc\n  description = CC $in")
    # One archive per game module: ld keeps every input open, and 24k objects exceed the
    # host's file limit. --whole-archive keeps all of it, as the DS build had it;
    # --gc-sections then drops what nothing reaches.
    # tools/weaken_unowned.py keeps delinks.txt's section ownership (see its docstring)
    w(f"rule ar\n  command = python3 tools/weaken_unowned.py {shlex.quote(str(Path(args.decomp).resolve()))}"
      " $out $in\n  description = AR $out")
    w("rule as\n  command = $cc $common -c $in -o $out\n  description = AS $in")
    w("rule link\n  command = $cc -Wl,-q -Wl,--gc-sections -Wl,--no-enum-size-warning $link_extra"
      " $port_objs -Wl,--whole-archive $game_libs -Wl,--no-whole-archive $libs -o $out"
      " && python3 tools/check_elf.py $out || (rm -f $out; false)\n"
      "  description = LINK $out")
    w(f"rule velf\n  command = {BIN / 'vita-elf-create'} $in $out\n  description = VELF $out")
    w(f"rule fself\n  command = {BIN / 'vita-make-fself'} -c $in $out\n  description = FSELF $out")
    w(f"rule sfo\n  command = {BIN / 'vita-mksfoex'} -s TITLE_ID={TITLE_ID} -s APP_VER={app_ver}"
      f" -d ATTRIBUTE2=12 {shlex.quote(TITLE)} $out\n  description = SFO $out")
    # the LiveArea art: the user's own from art_src/ (never committed: it is the game's
    # artwork) converted to the exact format VitaShell accepts, else the repository's
    # placeholders in sce_sys/
    art_dir = "build/sce_sys" if (ROOT / "art_src").is_dir() else "sce_sys"
    art_from = " --from art_src" if art_dir != "sce_sys" else ""
    w(f"rule livearea\n  command = python3 {rel(ROOT / 'tools' / 'make_livearea.py')} {art_dir}"
      f"{art_from}\n  description = LIVEAREA {art_dir}")
    w(f"rule vpk\n  command = {BIN / 'vita-pack-vpk'} -s build/param.sfo -b build/eboot.bin"
      f" -a {art_dir}/icon0.png=sce_sys/icon0.png"
      f" -a {art_dir}/livearea/contents/bg.png=sce_sys/livearea/contents/bg.png"
      f" -a {art_dir}/livearea/contents/startup.png=sce_sys/livearea/contents/startup.png"
      " -a sce_sys/livearea/contents/template.xml=sce_sys/livearea/contents/template.xml"
      " $out\n  description = VPK $out")
    w("")

    objs = []
    for src in port_srcs:
        obj = "build/port/" + os.path.relpath(src, ROOT)[:-2] + ".o"
        w(f"build {obj}: cc_port {rel(src)}")
        objs.append(obj)
    modules = {}
    if game_srcs:
        import decomp_sources
        decomp = Path(args.decomp).resolve()
        rule = {"c": "cc_game", "cpp": "cxx_game", "s": "as_game"}
        for src, kind in game_srcs:
            relsrc = os.path.relpath(src, decomp)
            obj = "build/game/" + os.path.splitext(relsrc)[0] + ".o"
            w(f"build {obj.replace(' ', '$ ')}: {rule[kind]} {str(src).replace(' ', '$ ')}")
            if kind == "c" and "/data/" in relsrc.replace(os.sep, "/"):
                # data-only files, laid out as mwcc did: at -Os, as GCC for ARM word-aligns
                # arrays and structs only when optimising for speed (ARM_EXPAND_ALIGNMENT), and
                # with zero initialisers kept in .data next to their neighbours, not moved to .bss
                w("  game_cflags = $game_cflags -Os -fno-zero-initialized-in-bss")
            modules.setdefault(decomp_sources.module_of(relsrc), []).append(obj)
    link_extra = ""
    if game_srcs:
        w("build build/gen/ds_bss.o: as build/gen/ds_bss.S")
        w("build build/gen/overlays.o: cc_port build/gen/overlays.c")
        # static asserts on the decomp's struct layouts (game flags and headers)
        w("build build/gen/abi_check.o: cc_game platform/nitro/abi_check.c")
        objs += ["build/gen/ds_bss.o", "build/gen/overlays.o", "build/gen/abi_check.o"]
        link_extra = "@build/gen/link.rsp -Wl,-T,build/gen/overlays.ld"
    game_libs = []
    for name, mobjs in sorted(modules.items()):
        lib = f"build/lib/{name}.a"
        w(f"build {lib}: ar {' '.join(mobjs)} | build/gen/bss_names.txt tools/weaken_unowned.py")
        game_libs.append(lib)

    art = [f"{art_dir}/icon0.png", f"{art_dir}/livearea/contents/bg.png",
           f"{art_dir}/livearea/contents/startup.png"]
    art_deps = " ".join(str(p.relative_to(ROOT)).replace(" ", "$ ")
                        for p in sorted((ROOT / "art_src").glob("*"))) if art_from else ""
    vpk = f"build/VPK/khdays-vita-{version}.vpk"
    w("")
    w(f"build build/khdays.elf: link {' '.join(objs + game_libs)}")
    w(f"  port_objs = {' '.join(objs)}")
    w(f"  game_libs = {' '.join(game_libs)}")
    w(f"  link_extra = {link_extra}")
    w("build build/khdays.velf: velf build/khdays.elf")
    w("build build/eboot.bin: fself build/khdays.velf")
    w("build build/param.sfo: sfo | VERSION")
    w(f"build {' '.join(art)}: livearea" + (f" | {art_deps}" if art_deps else ""))
    w(f"build {vpk}: vpk build/eboot.bin build/param.sfo {' '.join(art)}"
      " sce_sys/livearea/contents/template.xml")
    w(f"default {vpk}")
    (ROOT / "build.ninja").write_text("\n".join(out) + "\n")
    print(f"build.ninja: {len(port_srcs)} port + {len(game_srcs)} game sources -> {vpk}")


if __name__ == "__main__":
    main()
