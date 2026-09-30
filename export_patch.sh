#!/usr/bin/env bash
# Brings the port's changes to the decomp back into this repository: everything that differs in
# build/decomp from the pinned commit becomes patches/decomp.patch.
set -euo pipefail
cd "$(dirname "$0")"

tree=build/decomp
commit=$(cat DECOMP_COMMIT)
git -C "$tree" add -A
# nothing the repository must not publish: Windows binaries (CodeWarrior), ROM images, the
# ROM's extraction
bad=$(git -C "$tree" diff --cached --name-only "$commit" | grep -Ei '\.(exe|dll|nds|srl|bin)$|^dsd_extract/' || true)
if [ -n "$bad" ]; then
    git -C "$tree" reset --quiet
    echo "export: refusing, these must not go into the patch:" >&2
    echo "$bad" >&2
    exit 1
fi
git -C "$tree" diff --cached --binary "$commit" > patches/decomp.patch
git -C "$tree" reset --quiet
echo "export: $(grep -c '^diff --git' patches/decomp.patch || true) files in patches/decomp.patch"
