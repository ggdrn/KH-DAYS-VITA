#!/usr/bin/env bash
# Reference check: rebuilds the patched decomp (build/decomp) with the original toolchain and
# confirms the port's changes did not alter the DS build. Run after every change to the decomp.
#
# Needs, inside build/decomp (see the decomp's CONTRIBUTING.md): tools/mwccarm/, tools/dsd.exe
# and days.nds (the EU dump). On macOS/Linux the .exe tools run through wine; set WINE= if it
# is not on PATH. Everything the port adds is under PLATFORM_VITA, which mwccarm never defines.
set -euo pipefail
cd "$(dirname "$0")/../build/decomp"

EXPECTED=6fae8f5bbe80114b4e2535260eab5f4d0fc8a844
for need in tools/mwccarm tools/dsd.exe days.nds; do
    [ -e "$need" ] || { echo "check: missing build/decomp/$need" >&2; exit 2; }
done
[ "$(shasum days.nds | cut -d' ' -f1)" = "$EXPECTED" ] || { echo "check: days.nds is not the EU dump" >&2; exit 2; }

python3 tools/configure.py
ninja
ninja build/arm9.elf
${WINE:-wine} tools/dsd.exe check modules --config-path config/arm9/config.yaml -f
bash tools/build_rom.sh
echo "check: decomp still builds the original game (306/306 modules byte-exact)"
