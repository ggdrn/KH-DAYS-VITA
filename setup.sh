#!/usr/bin/env bash
# Builds the local build tree: the decomp at the pinned commit (DECOMP_COMMIT) with the port's
# changes (patches/decomp.patch) applied, in build/decomp. The tree is local only: it holds the
# decomp's code, which this repository does not redistribute.
#
#   ./setup.sh [DECOMP_REPO]    DECOMP_REPO: a local clone or URL
#                               (default: ../khdays-decomp, else the upstream GitHub repo)
set -euo pipefail
cd "$(dirname "$0")"

UPSTREAM=https://github.com/Yokimitsuro/khdays-decomp.git
repo=${1:-}
if [ -z "$repo" ]; then
    if [ -d ../khdays-decomp/.git ]; then repo=../khdays-decomp; else repo=$UPSTREAM; fi
fi
commit=$(cat DECOMP_COMMIT)
tree=build/decomp

if [ -d "$tree/.git" ]; then
    if [ -n "$(git -C "$tree" status --porcelain)" ]; then
        echo "setup: $tree has local changes; run ./export_patch.sh first or remove it" >&2
        exit 1
    fi
else
    mkdir -p build
    git clone --quiet "$repo" "$tree"
fi

git -C "$tree" fetch --quiet "$repo" "$commit" 2>/dev/null || true
git -C "$tree" checkout --quiet --detach "$commit"
if [ -s patches/decomp.patch ]; then
    git -C "$tree" apply --whitespace=nowarn "$PWD/patches/decomp.patch"
fi
echo "setup: $tree at ${commit:0:9} with patches/decomp.patch applied"
