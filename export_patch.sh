#!/usr/bin/env bash
# Brings the port's changes to the decomp back into this repository: everything that differs in
# build/decomp from the pinned commit becomes patches/decomp.patch.
set -euo pipefail
cd "$(dirname "$0")"

tree=build/decomp
commit=$(cat DECOMP_COMMIT)
git -C "$tree" add -A
git -C "$tree" diff --cached --binary "$commit" > patches/decomp.patch
git -C "$tree" reset --quiet
echo "export: $(grep -c '^diff --git' patches/decomp.patch || true) files in patches/decomp.patch"
