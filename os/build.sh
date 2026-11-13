#!/bin/sh
#
# DOS-XC /os/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
. "$(dirname "$0")/../scripts/common.sh"
mkdir -p "$PART/32" "$PART/64" "$PART/iso32/boot" "$PART/iso64/boot" "$PART/iso64/EFI/BOOT"

compile_kernel() {
    bits=$1
    target=$2
    tag=$3
    kernel_base=$4
    core_base=$5
    shell_base=$6
    emulation=elf_x86_64
    [ "$bits" = 32 ] && emulation=elf_i386
    "$NASM" -f elf"$bits" "$ROOT/os/kernel/entry$bits.asm" -o "$PART/entry$tag.o"
    for component in kernel/main kernel/console drivers/keyboard drivers/pcspeaker drivers/fat32 drivers/storage programs/shell; do
        name=${component##*/}
        spawn "$CC" --target="$target" -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-sse -mno-sse2 \
            -nostdinc -I"$ROOT/os" -c "$ROOT/os/$component.c" -o "$PART/$name$tag.o"
    done
    finish_jobs
    "$LD" -m "$emulation" -e 0 -T "$ROOT/os/kernel/module.ld" --defsym=MODULE_BASE="$core_base" --defsym=MODULE_BITS="$bits" --defsym=bios_transfer="$((kernel_base + 320))" \
        -o "$PART/core$tag.elf" "$PART/console$tag.o" "$PART/keyboard$tag.o" "$PART/pcspeaker$tag.o" \
        "$PART/fat32$tag.o" "$PART/storage$tag.o" --defsym=__core_bss_start=__module_bss_start --defsym=__core_bss_end=__module_bss_end
    "$OBJCOPY" -O binary "$PART/core$tag.elf" "$PART/core$tag.dll"
    python3 "$ROOT/tools/export_symbols.py" "$PART/core$tag.elf" "$PART/core$tag.exports" console_,keyboard_,pcspeaker_,fat32_,storage_,__core_bss_
    "$LD" -m "$emulation" -e 0 -T "$ROOT/os/kernel/module.ld" --defsym=MODULE_BASE="$shell_base" --defsym=MODULE_BITS="$bits" \
        -T "$PART/core$tag.exports" --defsym=enter_dos="$((kernel_base + 256))" \
        -o "$PART/shell$tag.elf" "$PART/shell$tag.o" --defsym=__shell_bss_start=__module_bss_start --defsym=__shell_bss_end=__module_bss_end
    "$OBJCOPY" -O binary "$PART/shell$tag.elf" "$PART/shell$tag.dll"
    python3 "$ROOT/tools/export_symbols.py" "$PART/shell$tag.elf" "$PART/shell$tag.exports" shell_run,shell_firmware,__shell_bss_
    "$LD" -m "$emulation" -T "$ROOT/os/kernel/kernel$bits.ld" --defsym=KERNEL_BASE="$kernel_base" \
        -T "$PART/core$tag.exports" -T "$PART/shell$tag.exports" \
        -o "$PART/kernel$tag.elf" "$PART/entry$tag.o" "$PART/main$tag.o"
    "$OBJCOPY" -O binary "$PART/kernel$tag.elf" "$PART/kernel$tag.bin"
}

for edition in $EDITIONS; do
    case "$edition" in
    32) compile_kernel 32 i386-unknown-none 32 0x20000 0x30000 0x60000 ;;
    64) compile_kernel 64 x86_64-unknown-none 64 0x20000 0x30000 0x60000 ;;
    esac
done
compile_kernel 64 x86_64-unknown-none uefi 0x2000000 0x2100000 0x2200000

compile_runtime() {
    runtime_bits=$1
    runtime_target=$2
    runtime_emulation=$3
    mkdir -p "$PART/runtime$runtime_bits"
    objects=
    for unit in system clock loader dynamic paging files terminal volumes cache memory cpu support \
                usb pci xhci ehci ohci uhci storage ata ahci nvme iso9660 accounts sha256 events bridge power drivers net net6 tcp socket firewall display sched smp random vm fork; do
        spawn "$CC" --target="$runtime_target" -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-sse -mno-sse2 \
            -nostdinc -O2 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/os" \
            -c "$ROOT/os/runtime/$unit.c" -o "$PART/runtime$runtime_bits/$unit.o"
        objects="$objects $PART/runtime$runtime_bits/$unit.o"
    done
    finish_jobs
    "$NASM" -f elf"$runtime_bits" -I"$PART/" "$ROOT/os/runtime/switch$runtime_bits.asm" -o "$PART/runtime$runtime_bits/switch.o"
    "$LD" -m "$runtime_emulation" -T "$ROOT/os/runtime/runtime.ld" --defsym=RUNTIME_ADDRESS=0x4400000 \
        --defsym=RUNTIME_BITS="$runtime_bits" -o "$PART/runtime$runtime_bits/elfapi.elf" \
        $objects "$PART/runtime$runtime_bits/switch.o"
    "$OBJCOPY" -O binary "$PART/runtime$runtime_bits/elfapi.elf" "$PART/elfapi$runtime_bits.dll"
}
"$NASM" -f bin "$ROOT/os/dos/bridge.asm" -o "$PART/bridge.bin"
for edition in $EDITIONS; do
    case "$edition" in
    32) compile_runtime 32 i386-unknown-none elf_i386 ;;
    64) compile_runtime 64 x86_64-unknown-none elf_x86_64 ;;
    esac
done

for edition in $EDITIONS; do
    use_edition "$edition"
    mkdir -p "$out/system"
    spawn "$cc" $flags -I"$ROOT/os/shell" "$ROOT/os/shell/command.c" -o "$out/system/COMMAND.EXE"
    spawn "$cc" $flags -I"$ROOT/os" "$ROOT/os/setup/setup.c" "$ROOT/os/setup/install.c" "$ROOT/os/runtime/sha256.c" "$ROOT/os/setup/target.c" "$ROOT/os/drivers/fat32.c" \
        "$ROOT/os/pkg/dsp.c" "$ROOT/os/pkg/unxz.c" "$ROOT/os/pkg/inflate.c" "$ROOT/os/pkg/deflate.c" -o "$out/system/SETUP.EXE"
    finish_jobs
done
