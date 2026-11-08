#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sed -n 's/^#define DOSXC_VERSION "\(.*\)"$/\1/p' "$ROOT/src/kernel/bootinfo.h")
ISO=${ISO:-"$ROOT/build/iso/DOSXC_${VERSION}_x64.iso"}
QEMU=${QEMU:-qemu-system-x86_64}
MEMORY=${MEMORY:-1024}   # MiB; GCC needs several hundred

if [ ! -f "$ISO" ]; then
    echo "error: ISO not found: $ISO (run scripts/build.sh first)" >&2
    exit 1
fi

command -v "$QEMU" >/dev/null 2>&1 || {
    echo "error: QEMU emulator not found: $QEMU" >&2
    exit 1
}

# The PC speaker needs an audio backend: coreaudio on macOS, PulseAudio
# elsewhere (AUDIO=none, sdl, pipewire, alsa ... to choose another).
case "$(uname -s)" in
Darwin) AUDIO=${AUDIO:-coreaudio} ;;
*) AUDIO=${AUDIO:-pa} ;;
esac

exec "$QEMU" -m "$MEMORY" -audiodev "$AUDIO",id=speaker -machine pcspk-audiodev=speaker -cdrom "$ISO" "$@"
