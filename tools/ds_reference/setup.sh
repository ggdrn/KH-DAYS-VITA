#!/usr/bin/env bash
# The DS reference toolchain in an OrbStack Linux machine (x86_64), for
# tools/check_decomp_match.sh: WineHQ's macOS builds no longer pass Gatekeeper, so the
# CodeWarrior compilers (Windows-only) run under the Linux wine instead.
#
#   tools/ds_reference/setup.sh          # on the Mac, with OrbStack set up
#
# Creates the machine `khdays-ds` (Ubuntu, amd64 under Rosetta) with wine (32-bit, as
# mwccarm/mwldarm are), Python + the decomp's modules, ninja, binutils-arm-none-eabi and dsd
# (AetiasHax/ds-decomp, the release the decomp's CONTRIBUTING.md points to). The Mac's
# filesystem is visible in the machine at the same paths, so the build runs on the tree in
# place. The CodeWarrior binaries are not part of this: place them in build/decomp/tools/mwccarm
# (see wrap_mwccarm.sh).
set -euo pipefail

MACHINE=khdays-ds
DSD_VERSION=v0.12.1
DSD_URL=https://github.com/AetiasHax/ds-decomp/releases/download/$DSD_VERSION/dsd-linux-x86_64

command -v orb >/dev/null || { echo "setup: OrbStack (orb) not found" >&2; exit 1; }
if ! orb list 2>/dev/null | grep -q "^$MACHINE\b"; then
    orb create --arch amd64 ubuntu:noble "$MACHINE"
fi

orb -m "$MACHINE" -u root bash -euo pipefail -s <<EOF
export DEBIAN_FRONTEND=noninteractive
if ! command -v wine >/dev/null; then
    dpkg --add-architecture i386
    apt-get update -q
    apt-get install -y -q --no-install-recommends wine wine32:i386 wine64 \
        python3 python3-pip python3-venv ninja-build binutils-arm-none-eabi curl ca-certificates
fi
if [ ! -x /opt/khdays/venv/bin/python ]; then
    python3 -m venv /opt/khdays/venv
    /opt/khdays/venv/bin/pip install -q capstone pyelftools ndspy
fi
if [ ! -x /opt/khdays/dsd ]; then
    curl -fsSL -o /opt/khdays/dsd "$DSD_URL"
    chmod +x /opt/khdays/dsd
fi
EOF

# a wine prefix for the user, created once (quietly: no GUI dialogs)
orb -m "$MACHINE" bash -c 'WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i >/dev/null 2>&1 || true'

echo "setup: $MACHINE ready (wine $(orb -m "$MACHINE" wine --version), dsd $DSD_VERSION)"
