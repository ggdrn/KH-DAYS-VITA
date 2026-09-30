#!/usr/bin/env bash
# Reference check: rebuilds the patched decomp (build/decomp) with the original toolchain and
# confirms the port's changes did not alter the DS build. Run after every change to the decomp.
#
#   tools/check_decomp_match.sh
#
# On the Mac it runs itself in the OrbStack machine set up by tools/ds_reference/setup.sh (the
# CodeWarrior compilers are Windows programs; the Linux wine runs them). Needs, inside
# build/decomp: tools/mwccarm/ (the CodeWarrior binaries, see the decomp's CONTRIBUTING.md)
# and days.nds (the EU dump; ../days.nds is linked in when missing). Everything the port adds
# is under PLATFORM_VITA, which mwccarm never defines.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

if [ "$(uname -s)" = Darwin ]; then
    exec orb -m khdays-ds bash "$here/check_decomp_match.sh" "$@"
fi

cd "$here/../build/decomp"
export PATH="/opt/khdays/venv/bin:$PATH" # the decomp's Python modules (capstone, ndspy, ...)

EXPECTED=6fae8f5bbe80114b4e2535260eab5f4d0fc8a844
[ -e days.nds ] || ln -s ../../../days.nds days.nds
[ -d tools/mwccarm ] || { echo "check: missing build/decomp/tools/mwccarm (CodeWarrior)" >&2; exit 2; }
[ "$(sha1sum days.nds | cut -d' ' -f1)" = "$EXPECTED" ] || { echo "check: days.nds is not the EU dump" >&2; exit 2; }

bash "$here/ds_reference/wrap_tools.sh" .
[ -d dsd_extract ] || tools/dsd.exe rom extract --rom days.nds --output-path dsd_extract/

python3 tools/configure.py
ninja
ninja build/arm9.elf
tools/dsd.exe check modules --config-path config/arm9/config.yaml -f
bash tools/build_rom.sh
echo "check: decomp still builds the original game (306/306 modules byte-exact)"
