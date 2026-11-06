#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ISO=${ISO:-"$ROOT/build/BenDOS-x32.iso"}
QEMU=${QEMU:-qemu-system-i386}

if [ ! -f "$ISO" ]; then
    echo "error: ISO not found: $ISO (run ./build.sh first)" >&2
    exit 1
fi

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "error: QEMU emulator not found: $QEMU" >&2
    exit 1
}

exec "$QEMU" -cdrom "$ISO" "$@"
