#!/usr/bin/env bash
# Builds and runs the host test for platform/audio/snd7.c: tools/snd_test/run.sh ROM [seq...]
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/snd_test
cc -std=gnu11 -O2 -Wall -Wextra -Iplatform tools/snd_test/snd_test.c platform/audio/snd7.c -lm -o build/snd_test/snd_test
rom=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
(cd build/snd_test && ./snd_test "$rom" "$@")
