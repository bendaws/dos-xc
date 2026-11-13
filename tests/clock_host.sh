#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
# The runtime headers contain x86 port-I/O intrinsics; use Rosetta on Apple Silicon.
ARCH_FLAGS=
if [ "$(uname -s)" = Darwin ]; then ARCH_FLAGS='-arch x86_64'; fi
clang $ARCH_FLAGS -O1 -g -fsanitize=address,undefined -Wno-unused-parameter \
    "$ROOT/tests/programs/clock_host.c" "$ROOT/os/runtime/clock.c" -o "$WORK/clock"
"$WORK/clock"
