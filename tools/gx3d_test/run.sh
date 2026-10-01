#!/usr/bin/env bash
# Builds and runs the host test for platform/hw/gx3d.c (the DS geometry engine).
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/gx3d_test
cc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Itools/gx3d_test -Iplatform \
    tools/gx3d_test/gx3d_test.c platform/hw/gx3d.c -o build/gx3d_test/gx3d_test
build/gx3d_test/gx3d_test
