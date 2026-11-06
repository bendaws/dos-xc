#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD=${BUILD:-"$ROOT/build"}
NASM=${NASM:-nasm}
CC=${CC:-clang}
LD=${LD:-ld.lld}
XORRISO=${XORRISO:-xorriso}
OBJCOPY=${OBJCOPY:-llvm-objcopy}
if ! command -v "$OBJCOPY" >/dev/null 2>&1 && [ -x /opt/homebrew/opt/llvm/bin/llvm-objcopy ]; then
    OBJCOPY=/opt/homebrew/opt/llvm/bin/llvm-objcopy
fi

rm -rf "$BUILD"
mkdir -p "$BUILD/32" "$BUILD/64" "$BUILD/iso32/boot" "$BUILD/iso64/boot" "$BUILD/iso64/EFI/BOOT"

compile_kernel() {
    bits=$1
    target=$2
    tag=$3
    kernel_base=$4
    core_base=$5
    shell_base=$6
    emulation=elf_x86_64
    [ "$bits" = 32 ] && emulation=elf_i386
    "$NASM" -f elf"$bits" "$ROOT/src/kernel/entry$bits.asm" -o "$BUILD/entry$tag.o"
    for component in kernel/main kernel/console drivers/keyboard drivers/pcspeaker drivers/fat32 drivers/storage programs/shell; do
        name=${component##*/}
        "$CC" --target="$target" -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-sse -mno-sse2 \
            -nostdinc -I"$ROOT/src" -c "$ROOT/src/$component.c" -o "$BUILD/$name$tag.o"
    done
    "$LD" -m "$emulation" -e 0 -T "$ROOT/src/kernel/module.ld" --defsym=MODULE_BASE="$core_base" --defsym=MODULE_BITS="$bits" --defsym=bios_transfer="$((kernel_base + 320))" \
        -o "$BUILD/core$tag.elf" "$BUILD/console$tag.o" "$BUILD/keyboard$tag.o" "$BUILD/pcspeaker$tag.o" \
        "$BUILD/fat32$tag.o" "$BUILD/storage$tag.o" --defsym=__core_bss_start=__module_bss_start --defsym=__core_bss_end=__module_bss_end
    "$OBJCOPY" -O binary "$BUILD/core$tag.elf" "$BUILD/core$tag.dll"
    python3 "$ROOT/tools/export_symbols.py" "$BUILD/core$tag.elf" "$BUILD/core$tag.exports" console_,keyboard_,pcspeaker_,fat32_,storage_,__core_bss_
    "$LD" -m "$emulation" -e 0 -T "$ROOT/src/kernel/module.ld" --defsym=MODULE_BASE="$shell_base" --defsym=MODULE_BITS="$bits" \
        -T "$BUILD/core$tag.exports" --defsym=enter_dos="$((kernel_base + 256))" \
        -o "$BUILD/shell$tag.elf" "$BUILD/shell$tag.o" --defsym=__shell_bss_start=__module_bss_start --defsym=__shell_bss_end=__module_bss_end
    "$OBJCOPY" -O binary "$BUILD/shell$tag.elf" "$BUILD/shell$tag.dll"
    python3 "$ROOT/tools/export_symbols.py" "$BUILD/shell$tag.elf" "$BUILD/shell$tag.exports" shell_run,__shell_bss_
    "$LD" -m "$emulation" -T "$ROOT/src/kernel/kernel$bits.ld" --defsym=KERNEL_BASE="$kernel_base" \
        -T "$BUILD/core$tag.exports" -T "$BUILD/shell$tag.exports" \
        -o "$BUILD/kernel$tag.elf" "$BUILD/entry$tag.o" "$BUILD/main$tag.o"
    "$OBJCOPY" -O binary "$BUILD/kernel$tag.elf" "$BUILD/kernel$tag.bin"
}

command -v "$NASM" >/dev/null || { echo "error: NASM is required" >&2; exit 1; }
command -v "$CC" >/dev/null || { echo "error: Clang is required" >&2; exit 1; }
command -v "$LD" >/dev/null || { echo "error: lld is required" >&2; exit 1; }
command -v "$OBJCOPY" >/dev/null || { echo "error: llvm-objcopy is required" >&2; exit 1; }
command -v "$XORRISO" >/dev/null || { echo "error: xorriso is required" >&2; exit 1; }

python3 "$ROOT/tools/version.py" "$BUILD"

compile_kernel 32 i386-unknown-none 32 0x20000 0x30000 0x60000
compile_kernel 64 x86_64-unknown-none 64 0x20000 0x30000 0x60000
compile_kernel 64 x86_64-unknown-none uefi 0x2000000 0x2100000 0x2200000

compile_commands() {
    command_bits=$1
    command_target=$2
    command_emulation=$3
    mkdir -p "$BUILD/cmd$command_bits"
    command_id=0
    for command_name in HELP DIR TYPE ECHO CLS COLOR VER BEEP WRITE COPY MOVE REN DEL MD RD EDIT SET SPEED PATH EXIT; do
        command_id=$((command_id+1))
        "$CC" --target="$command_target" -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-sse -mno-sse2 \
            -nostdinc -DCOMMAND_ID="$command_id" -I"$ROOT/src" -c "$ROOT/src/programs/commands.c" -o "$BUILD/cmd$command_bits/$command_name.o"
        "$LD" -m "$command_emulation" -T "$ROOT/src/kernel/program.ld" --defsym=PROGRAM_ADDRESS=0x4000000 --defsym=PROGRAM_BITS="$command_bits" --defsym=PROGRAM_RESUME=0 \
            -o "$BUILD/cmd$command_bits/$command_name.elf" "$BUILD/cmd$command_bits/$command_name.o"
        "$OBJCOPY" -O binary "$BUILD/cmd$command_bits/$command_name.elf" "$BUILD/cmd$command_bits/$command_name.EXE"
    done
    "$CC" --target="$command_target" -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-sse -mno-sse2 \
        -nostdinc -I"$ROOT/src" -c "$ROOT/src/programs/batch.c" -o "$BUILD/batch$command_bits.o"
    "$LD" -m "$command_emulation" -T "$ROOT/src/kernel/program.ld" --defsym=PROGRAM_ADDRESS=0x4100000 --defsym=PROGRAM_BITS="$command_bits" --defsym=PROGRAM_RESUME=program_resume \
        -o "$BUILD/batch$command_bits.elf" "$BUILD/batch$command_bits.o"
    "$OBJCOPY" -O binary "$BUILD/batch$command_bits.elf" "$BUILD/batch$command_bits.exe"
}
compile_commands 32 i386-unknown-none elf_i386
compile_commands 64 x86_64-unknown-none elf_x86_64

compile_installer() {
    installer_bits=$1
    installer_target=$2
    installer_emulation=$3
    for unit in install program; do
        "$CC" --target="$installer_target" -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-sse -mno-sse2 -nostdinc \
            -I"$ROOT/src" -c "$ROOT/src/setup/$unit.c" -o "$BUILD/setup-$unit$installer_bits.o"
    done
    "$LD" -m "$installer_emulation" -T "$ROOT/src/kernel/program.ld" -T "$BUILD/core$installer_bits.exports" \
        --defsym=PROGRAM_ADDRESS=0x4000000 --defsym=PROGRAM_BITS="$installer_bits" --defsym=PROGRAM_RESUME=0 \
        -o "$BUILD/install$installer_bits.elf" "$BUILD/setup-install$installer_bits.o" "$BUILD/setup-program$installer_bits.o"
    "$OBJCOPY" -O binary "$BUILD/install$installer_bits.elf" "$BUILD/install$installer_bits.exe"
}
compile_installer 32 i386-unknown-none elf_i386
compile_installer 64 x86_64-unknown-none elf_x86_64
python3 "$ROOT/tools/pack_dos.py" "$BUILD"
"$NASM" -f bin -I"$BUILD/" -I"$ROOT/src/dos/" "$ROOT/src/dos/runtime.asm" -o "$BUILD/dos-runtime.bin"

"$CC" --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -c \
    "$ROOT/src/boot/uefi.c" -o "$BUILD/uefi.o"
"$CC" --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -nostdinc \
    -I"$ROOT/src" -c "$ROOT/src/drivers/fat32.c" -o "$BUILD/uefi-fat32.o"
"$CC" --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -nostdinc -I"$ROOT/src" \
    -c "$ROOT/src/setup/install.c" -o "$BUILD/uefi-install.o"
"$LD" -flavor link /subsystem:efi_application /entry:efi_main /out:"$BUILD/iso64/EFI/BOOT/BOOTX64.EFI" \
    "$BUILD/uefi.o" "$BUILD/uefi-fat32.o" "$BUILD/uefi-install.o"

"$NASM" -f bin "$ROOT/src/boot/mbr.asm" -o "$BUILD/mbr.bin"
"$NASM" -f bin "$ROOT/src/boot/volume.asm" -o "$BUILD/volume.bin"
"$NASM" -f bin -dFAT32 "$ROOT/src/boot/volume.asm" -o "$BUILD/volume32.bin"
"$NASM" -f bin -I"$BUILD/" "$ROOT/src/setup/setup.asm" -o "$BUILD/setup.bin"
"$NASM" -f bin "$ROOT/src/boot/fsloader.asm" -o "$BUILD/loader32.bin"
"$NASM" -f bin -dBEN_DOS_64 "$ROOT/src/boot/fsloader.asm" -o "$BUILD/loader64.bin"
python3 "$ROOT/tools/make_disk.py" "$BUILD" 32
python3 "$ROOT/tools/make_disk.py" "$BUILD" 64
"$NASM" -f bin "$ROOT/src/boot/boot.asm" -o "$BUILD/boot.bin"
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
    core_sectors=$((($(wc -c < "$BUILD/core$bits.dll") + 2047) / 2048))
    shell_sectors=$((($(wc -c < "$BUILD/shell$bits.dll") + 2047) / 2048))
    core_lba=$((97 + $(wc -c < "$BUILD/dos-data.bin") / 2048))
    shell_lba=$((core_lba + core_sectors))
    dos_sectors=$((($(wc -c < "$BUILD/dos-runtime.bin") + 2047) / 2048))
    stage2_flags="-dDOS_SECTORS=$dos_sectors -dKERNEL_SECTORS=$kernel_sectors -dCORE_LBA=$core_lba -dCORE_SECTORS=$core_sectors -dSHELL_LBA=$shell_lba -dSHELL_SECTORS=$shell_sectors"
    [ "$bits" = 64 ] && stage2_flags="$stage2_flags -dBEN_DOS_64"
    stage2_flags="$stage2_flags -dKERNEL_LBA=33"
    "$NASM" -f bin $stage2_flags "$stage2" -o "$BUILD/stage2$bits.bin"
    stage2_size=$(wc -c < "$BUILD/stage2$bits.bin")
    stage2_padding=$((128 * 512 - stage2_size))
    if [ "$stage2_size" -gt 32768 ]; then
        echo "error: stage two exceeds its 32 KiB boot read buffer" >&2
        exit 1
    fi
    dd if=/dev/zero of="$BUILD/stage2$bits.pad" bs=1 count="$stage2_padding" status=none
    cat "$BUILD/stage2$bits.bin" "$BUILD/stage2$bits.pad" "$kernel" > "$payload"
    kernel_padding=$((kernel_sectors * 2048 - kernel_size))
    dd if=/dev/zero bs=1 count="$kernel_padding" status=none >> "$payload"
    # Kernel space ends at CD block 65. The native DOS runtime occupies
    # blocks 65..96; optional user-supplied game files begin at block 97.
    dd if=/dev/zero bs=2048 count="$((32 - kernel_sectors))" status=none >> "$payload"
    cat "$BUILD/dos-runtime.bin" >> "$payload"
    dd if=/dev/zero bs=1 count="$((65536 - $(wc -c < "$BUILD/dos-runtime.bin")))" status=none >> "$payload"
    cat "$BUILD/dos-data.bin" "$BUILD/core$bits.dll" >> "$payload"
    dd if=/dev/zero bs=1 count="$((core_sectors * 2048 - $(wc -c < "$BUILD/core$bits.dll")))" status=none >> "$payload"
    cat "$BUILD/shell$bits.dll" >> "$payload"
    dd if=/dev/zero bs=1 count="$((shell_sectors * 2048 - $(wc -c < "$BUILD/shell$bits.dll")))" status=none >> "$payload"

}
build_payload 32 "$BUILD/kernel32.bin" "$ROOT/src/boot/stage2.asm" "$BUILD/32/payload.bin"
build_payload 64 "$BUILD/kernel64.bin" "$ROOT/src/boot/stage2.asm" "$BUILD/64/payload.bin"
cat "$BUILD/boot.bin" "$BUILD/32/payload.bin" > "$BUILD/iso32/boot/bendos.img"
cat "$BUILD/boot.bin" "$BUILD/64/payload.bin" > "$BUILD/iso64/boot/bendos.img"

"$XORRISO" -as mkisofs -b boot/bendos.img -no-emul-boot -boot-info-table -boot-load-size 4 \
    -o "$BUILD/BenDOS-x32.iso" "$BUILD/iso32"
"$XORRISO" -as mkisofs -b boot/bendos.img -no-emul-boot -boot-info-table -boot-load-size 4 \
    -eltorito-alt-boot -e boot/efi.img -no-emul-boot \
    -o "$BUILD/BenDOS-x64.iso" "$BUILD/iso64"
printf 'Built %s and %s\n' "$BUILD/BenDOS-x32.iso" "$BUILD/BenDOS-x64.iso"


python3 "$ROOT/tools/finalize_iso.py" "$BUILD/BenDOS-x32.iso"
python3 "$ROOT/tools/finalize_iso.py" "$BUILD/BenDOS-x64.iso"
