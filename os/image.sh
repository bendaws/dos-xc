#!/bin/sh
#
# DOS-XC /os/image.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
ISO_DIR=$BUILD/iso
mkdir -p "$ISO_DIR"
MEDIA=$PACKAGES
if [ -f "$PART/media.list" ]; then
    MEDIA=$PART/media
    rm -rf "$MEDIA"
    mkdir -p "$MEDIA"
    while read -r package; do
        ln -f "$PACKAGES/$package" "$MEDIA/$package"
    done < "$PART/media.list"
fi
ISO32=$ISO_DIR/DOSXC_${VERSION}_x86.iso
ISO64=$ISO_DIR/DOSXC_${VERSION}_x64.iso

python3 "$ROOT/tools/pack_dos.py" "$PART"
"$NASM" -f bin -I"$PART/" -I"$ROOT/os/dos/" "$ROOT/os/dos/runtime.asm" -o "$PART/dos-runtime.bin"

"$CC" --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -c \
    "$ROOT/os/boot/uefi.c" -o "$PART/uefi.o"
"$CC" --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -nostdinc \
    -I"$ROOT/os" -c "$ROOT/os/drivers/fat32.c" -o "$PART/uefi-fat32.o"
"$LD" -flavor link /subsystem:efi_application /entry:efi_main /out:"$PART/iso64/EFI/BOOT/BOOTX64.EFI" \
    "$PART/uefi.o" "$PART/uefi-fat32.o"

"$NASM" -f bin "$ROOT/os/boot/mbr.asm" -o "$PART/mbr.bin"
"$NASM" -f bin "$ROOT/os/boot/volume.asm" -o "$PART/volume.bin"
"$NASM" -f bin -dFAT32 "$ROOT/os/boot/volume.asm" -o "$PART/volume32.bin"
"$NASM" -f bin "$ROOT/os/boot/fsloader.asm" -o "$PART/loader32.bin"
"$NASM" -f bin -dDOS_XC_64 "$ROOT/os/boot/fsloader.asm" -o "$PART/loader64.bin"
for bits in $EDITIONS; do
    python3 "$ROOT/tools/make_disk.py" "$PART" "$bits" "$MEDIA"
done
"$NASM" -f bin "$ROOT/os/boot/boot.asm" -o "$PART/boot.bin"
build_payload() {
    bits=$1
    kernel=$2
    stage2=$3
    payload=$4
    kernel_size=$(wc -c < "$kernel")
    
    kernel_sectors=$(((kernel_size + 2047) / 2048))
    if [ "$kernel_sectors" -gt 32 ]; then
        echo "error: kernel exceeds the BIOS loader's 64 KiB read buffer" >&2
        exit 1
    fi
    core_sectors=$((($(wc -c < "$PART/core$bits.dll") + 2047) / 2048))
    shell_sectors=$((($(wc -c < "$PART/shell$bits.dll") + 2047) / 2048))
    core_lba=$((97 + $(wc -c < "$PART/dos-data.bin") / 2048))
    shell_lba=$((core_lba + core_sectors))
    dos_sectors=$((($(wc -c < "$PART/dos-runtime.bin") + 2047) / 2048))
    stage2_flags="-dDOS_SECTORS=$dos_sectors -dKERNEL_SECTORS=$kernel_sectors -dCORE_LBA=$core_lba -dCORE_SECTORS=$core_sectors -dSHELL_LBA=$shell_lba -dSHELL_SECTORS=$shell_sectors"
    [ "$bits" = 64 ] && stage2_flags="$stage2_flags -dDOS_XC_64"
    stage2_flags="$stage2_flags -dKERNEL_LBA=33"
    "$NASM" -f bin $stage2_flags "$stage2" -o "$PART/stage2$bits.bin"
    stage2_size=$(wc -c < "$PART/stage2$bits.bin")
    stage2_padding=$((128 * 512 - stage2_size))
    if [ "$stage2_size" -gt 32768 ]; then
        echo "error: stage two exceeds its 32 KiB boot read buffer" >&2
        exit 1
    fi
    dd if=/dev/zero of="$PART/stage2$bits.pad" bs=1 count="$stage2_padding" status=none
    cat "$PART/stage2$bits.bin" "$PART/stage2$bits.pad" "$kernel" > "$payload"
    kernel_padding=$((kernel_sectors * 2048 - kernel_size))
    dd if=/dev/zero bs=1 count="$kernel_padding" status=none >> "$payload"
    # Kernel space ends at CD block 65. The native DOS runtime occupies
    # blocks 65..96; optional user-supplied game files begin at block 97.
    dd if=/dev/zero bs=2048 count="$((32 - kernel_sectors))" status=none >> "$payload"
    cat "$PART/dos-runtime.bin" >> "$payload"
    dd if=/dev/zero bs=1 count="$((65536 - $(wc -c < "$PART/dos-runtime.bin")))" status=none >> "$payload"
    cat "$PART/dos-data.bin" "$PART/core$bits.dll" >> "$payload"
    dd if=/dev/zero bs=1 count="$((core_sectors * 2048 - $(wc -c < "$PART/core$bits.dll")))" status=none >> "$payload"
    cat "$PART/shell$bits.dll" >> "$payload"
    dd if=/dev/zero bs=1 count="$((shell_sectors * 2048 - $(wc -c < "$PART/shell$bits.dll")))" status=none >> "$payload"
}
for bits in $EDITIONS; do
    build_payload "$bits" "$PART/kernel$bits.bin" "$ROOT/os/boot/stage2.asm" "$PART/$bits/payload.bin"
    cat "$PART/boot.bin" "$PART/$bits/payload.bin" > "$PART/iso$bits/boot/dosxc.img"
    case "$bits" in
    32)
        iso=$ISO32
        "$XORRISO" -as mkisofs -b boot/dosxc.img -no-emul-boot -boot-info-table -boot-load-size 4 \
            -o "$iso" "$PART/iso32"
        ;;
    64)
        iso=$ISO64
        "$XORRISO" -as mkisofs -b boot/dosxc.img -no-emul-boot -boot-info-table -boot-load-size 4 \
            -eltorito-alt-boot -e boot/efi.img -no-emul-boot \
            -o "$iso" "$PART/iso64"
        ;;
    esac
    python3 "$ROOT/tools/finalize_iso.py" "$iso"
    printf 'Built %s\n' "$iso"
done
