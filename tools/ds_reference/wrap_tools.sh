#!/usr/bin/env bash
# Inside the khdays-ds machine: makes the decomp's Windows tool paths runnable, without
# changing the decomp. Its scripts execute tools/dsd.exe and tools/mwccarm/<ver>/mw*arm.exe
# directly, as on Windows:
#   tools/dsd.exe                 -> the Linux dsd (/opt/khdays/dsd)
#   tools/mwccarm/**/mw*arm.exe   -> a script running the original (kept as *.win.exe) in wine
# Idempotent; run after placing new CodeWarrior binaries. Called by check_decomp_match.sh.
set -euo pipefail
cd "${1:-$(dirname "$0")/../../build/decomp}"

# none of this may reach patches/decomp.patch (export_patch.sh takes every untracked file):
# the CodeWarrior binaries are not ours to publish, the dump and its extraction are the game
for p in /tools/dsd.exe /tools/mwccarm/ /days.nds /dsd_extract/; do
    grep -qxF "$p" .git/info/exclude 2>/dev/null || echo "$p" >> .git/info/exclude
done

cat > tools/dsd.exe <<'EOF'
#!/bin/sh
exec /opt/khdays/dsd "$@"
EOF
chmod +x tools/dsd.exe

n=0
if [ -d tools/mwccarm ]; then
    while IFS= read -r exe; do
        win="${exe%.exe}.win.exe"
        if [ ! -e "$win" ]; then
            if head -c 2 "$exe" | grep -q MZ; then
                mv "$exe" "$win"
            else
                continue # not a Windows binary and no original next to it
            fi
        fi
        cat > "$exe" <<EOF
#!/bin/sh
# runs $(basename "$win") (CodeWarrior, Windows) in wine; see tools/ds_reference/wrap_tools.sh
WINEDEBUG=-all exec wine "\$(dirname "\$0")/$(basename "$win")" "\$@"
EOF
        chmod +x "$exe"
        n=$((n + 1))
    done < <(find tools/mwccarm -iname 'mw*arm.exe' ! -iname '*.win.exe')
fi
echo "wrap: dsd.exe, $n CodeWarrior tools"
