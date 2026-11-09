#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(sed -n 's/^#define DOSXC_VERSION "\(.*\)"$/\1/p' "$ROOT/src/kernel/bootinfo.h")
ISO=${ISO:-"$ROOT/build/iso/DOSXC_${VERSION}_x64.iso"}
QEMU=${QEMU:-qemu-system-x86_64}
MEMORY=${MEMORY:-1024}   # MiB; GCC needs several hundred

# -hd PATH boots from a virtual disk instead of the ISO.  Every other
# argument goes to QEMU unchanged.
HD=
remaining=$#
while [ "$remaining" -gt 0 ]; do
    arg=$1
    shift
    remaining=$((remaining - 1))
    if [ "$arg" = -hd ]; then
        if [ "$remaining" -eq 0 ]; then
            echo "error: -hd needs a disk image path" >&2
            exit 1
        fi
        HD=$1
        shift
        remaining=$((remaining - 1))
    else
        set -- "$@" "$arg"
    fi
done

if [ -n "$HD" ]; then
    if [ ! -f "$HD" ]; then
        echo "error: disk image not found: $HD" >&2
        exit 1
    fi
    # Name the format: QEMU blocks writes to sector 0 of a raw image it had to guess.
    case "$HD" in
    *.qcow2) HD_FORMAT=qcow2 ;;
    *.vmdk) HD_FORMAT=vmdk ;;
    *.vdi) HD_FORMAT=vdi ;;
    *.vhd) HD_FORMAT=vpc ;;
    *.vhdx) HD_FORMAT=vhdx ;;
    *) HD_FORMAT=raw ;;
    esac
    HD_ESCAPED=$(printf '%s\n' "$HD" | sed 's/,/,,/g')   # -drive separates options with commas
    set -- -drive "file=$HD_ESCAPED,format=$HD_FORMAT,if=ide,index=0,media=disk" -boot c "$@"
else
    if [ ! -f "$ISO" ]; then
        echo "error: ISO not found: $ISO (run scripts/build.sh first)" >&2
        exit 1
    fi
    set -- -cdrom "$ISO" "$@"
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

# DISPLAYS=2 (up to 4) gives the guest that many monitors, one graphics
# adapter each.  Pick the monitor in QEMU's View menu.
DISPLAYS=${DISPLAYS:-1}
VIDEO="-vga none -device VGA,id=video0"
i=1
while [ "$i" -lt "$DISPLAYS" ]; do
    VIDEO="$VIDEO -device secondary-vga,id=video$i"
    i=$((i + 1))
done

# shellcheck disable=SC2086
exec "$QEMU" $VIDEO -device qemu-xhci,id=xhci -device usb-mouse,bus=xhci.0 -m "$MEMORY" -audiodev "$AUDIO",id=speaker -machine pcspk-audiodev=speaker "$@"
