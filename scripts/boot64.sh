#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ISO=${ISO:-"$ROOT/build/DOS-XC-x64.iso"}
QEMU=${QEMU:-qemu-system-x86_64}
MEMORY=${MEMORY:-1024}   # MiB; GCC needs several hundred

if [ ! -f "$ISO" ]; then
    echo "error: ISO not found: $ISO (run ./build.sh first)" >&2
    exit 1
fi

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "error: QEMU emulator not found: $QEMU" >&2
    exit 1
}

exec "$QEMU" -m "$MEMORY" -cdrom "$ISO" "$@"
