#!/bin/sh
set -eu

# Build DOS-XC.  Intermediate files (libraries, programs, ISO trees) go to
# build/part; the bootable images go to build/iso as
# DOSXC_<version>_x86.iso and DOSXC_<version>_x64.iso.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/scripts/parallel.sh"
BUILD=${BUILD:-"$ROOT/build"}
PART=$BUILD/part
ISO_DIR=$BUILD/iso
NASM=${NASM:-nasm}
CC=${CC:-clang}
LD=${LD:-ld.lld}
XORRISO=${XORRISO:-xorriso}
OBJCOPY=${OBJCOPY:-llvm-objcopy}
if ! command -v "$OBJCOPY" >/dev/null 2>&1 && [ -x /opt/homebrew/opt/llvm/bin/llvm-objcopy ]; then
    OBJCOPY=/opt/homebrew/opt/llvm/bin/llvm-objcopy
fi

rm -rf "$PART" "$ISO_DIR"
mkdir -p "$ISO_DIR" "$PART/32" "$PART/64" "$PART/iso32/boot" "$PART/iso64/boot" "$PART/iso64/EFI/BOOT"

compile_kernel() {
    bits=$1
    target=$2
    tag=$3
    kernel_base=$4
    core_base=$5
    shell_base=$6
    emulation=elf_x86_64
    [ "$bits" = 32 ] && emulation=elf_i386
    "$NASM" -f elf"$bits" "$ROOT/src/kernel/entry$bits.asm" -o "$PART/entry$tag.o"
    for component in kernel/main kernel/console drivers/keyboard drivers/pcspeaker drivers/fat32 drivers/storage programs/shell; do
        name=${component##*/}
        spawn "$CC" --target="$target" -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-sse -mno-sse2 \
            -nostdinc -I"$ROOT/src" -c "$ROOT/src/$component.c" -o "$PART/$name$tag.o"
    done
    finish_jobs
    "$LD" -m "$emulation" -e 0 -T "$ROOT/src/kernel/module.ld" --defsym=MODULE_BASE="$core_base" --defsym=MODULE_BITS="$bits" --defsym=bios_transfer="$((kernel_base + 320))" \
        -o "$PART/core$tag.elf" "$PART/console$tag.o" "$PART/keyboard$tag.o" "$PART/pcspeaker$tag.o" \
        "$PART/fat32$tag.o" "$PART/storage$tag.o" --defsym=__core_bss_start=__module_bss_start --defsym=__core_bss_end=__module_bss_end
    "$OBJCOPY" -O binary "$PART/core$tag.elf" "$PART/core$tag.dll"
    python3 "$ROOT/tools/export_symbols.py" "$PART/core$tag.elf" "$PART/core$tag.exports" console_,keyboard_,pcspeaker_,fat32_,storage_,__core_bss_
    "$LD" -m "$emulation" -e 0 -T "$ROOT/src/kernel/module.ld" --defsym=MODULE_BASE="$shell_base" --defsym=MODULE_BITS="$bits" \
        -T "$PART/core$tag.exports" --defsym=enter_dos="$((kernel_base + 256))" \
        -o "$PART/shell$tag.elf" "$PART/shell$tag.o" --defsym=__shell_bss_start=__module_bss_start --defsym=__shell_bss_end=__module_bss_end
    "$OBJCOPY" -O binary "$PART/shell$tag.elf" "$PART/shell$tag.dll"
    python3 "$ROOT/tools/export_symbols.py" "$PART/shell$tag.elf" "$PART/shell$tag.exports" shell_run,shell_firmware,__shell_bss_
    "$LD" -m "$emulation" -T "$ROOT/src/kernel/kernel$bits.ld" --defsym=KERNEL_BASE="$kernel_base" \
        -T "$PART/core$tag.exports" -T "$PART/shell$tag.exports" \
        -o "$PART/kernel$tag.elf" "$PART/entry$tag.o" "$PART/main$tag.o"
    "$OBJCOPY" -O binary "$PART/kernel$tag.elf" "$PART/kernel$tag.bin"
}

command -v "$NASM" >/dev/null || { echo "error: NASM is required" >&2; exit 1; }
command -v "$CC" >/dev/null || { echo "error: Clang is required" >&2; exit 1; }
command -v "$LD" >/dev/null || { echo "error: lld is required" >&2; exit 1; }
command -v "$OBJCOPY" >/dev/null || { echo "error: llvm-objcopy is required" >&2; exit 1; }
command -v "$XORRISO" >/dev/null || { echo "error: xorriso is required" >&2; exit 1; }

python3 "$ROOT/tools/version.py" "$PART"
VERSION=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["version"])' "$PART/version.json")
ISO32=$ISO_DIR/DOSXC_${VERSION}_x86.iso
ISO64=$ISO_DIR/DOSXC_${VERSION}_x64.iso

compile_kernel 32 i386-unknown-none 32 0x20000 0x30000 0x60000
compile_kernel 64 x86_64-unknown-none 64 0x20000 0x30000 0x60000
compile_kernel 64 x86_64-unknown-none uefi 0x2000000 0x2100000 0x2200000

# \SYSTEM\ELFAPI.DLL: loader and system services for 32/64-bit ELF programs.
compile_runtime() {
    runtime_bits=$1
    runtime_target=$2
    runtime_emulation=$3
    mkdir -p "$PART/runtime$runtime_bits"
    objects=
    for unit in system loader dynamic paging files terminal volumes cache memory cpu support \
                usb pci xhci ehci ohci uhci bridge power drivers net tcp socket display sched random; do
        spawn "$CC" --target="$runtime_target" -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone -mno-sse -mno-sse2 \
            -nostdinc -O2 -Wall -Wextra -Wno-unused-parameter -I"$ROOT/src" \
            -c "$ROOT/src/runtime/$unit.c" -o "$PART/runtime$runtime_bits/$unit.o"
        objects="$objects $PART/runtime$runtime_bits/$unit.o"
    done
    finish_jobs
    "$NASM" -f elf"$runtime_bits" -I"$PART/" "$ROOT/src/runtime/switch$runtime_bits.asm" -o "$PART/runtime$runtime_bits/switch.o"
    "$LD" -m "$runtime_emulation" -T "$ROOT/src/runtime/runtime.ld" --defsym=RUNTIME_ADDRESS=0x4400000 \
        --defsym=RUNTIME_BITS="$runtime_bits" -o "$PART/runtime$runtime_bits/elfapi.elf" \
        $objects "$PART/runtime$runtime_bits/switch.o"
    "$OBJCOPY" -O binary "$PART/runtime$runtime_bits/elfapi.elf" "$PART/elfapi$runtime_bits.dll"
}
# The real-mode USB bridge the runtime installs at 0x1000 (src/dos/bridge.asm).
"$NASM" -f bin "$ROOT/src/dos/bridge.asm" -o "$PART/bridge.bin"
compile_runtime 32 i386-unknown-none elf_i386
compile_runtime 64 x86_64-unknown-none elf_x86_64

# ---- 32/64-bit user programs, built with the DOS-XC cross compiler ------
CROSS=${CROSS:-"$ROOT/toolchain/cross/bin"}
PACKAGES=$BUILD/packages
mkdir -p "$PACKAGES"
COREUTILS="HELP DIR TYPE MORE CLS COLOR VER BEEP WRITE COPY REN DEL MD RD EDIT SPEED RESTART SHUTDOWN EJECT IPCONFIG PING DRIVERS"
compile_userland() {
    bits=$1
    triple=$2
    libc_arch=$3
    cc="$CROSS/$triple-gcc"
    [ -x "$cc" ] || { echo "error: $cc not found; run toolchain/build-toolchain.sh first" >&2; exit 1; }
    sysroot=$PART/sysroot$bits
    out=$PART/user$bits
    "$ROOT/src/libc/build.sh" "$libc_arch" "$sysroot" clang >/dev/null
    mkdir -p "$out/system" "$out/coreutils" "$out/batch"
    flags="--sysroot=$sysroot -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation -Wno-misleading-indentation -Wno-stringop-truncation -s"
    spawn "$cc" $flags -I"$ROOT/src/shell" "$ROOT/src/shell/command.c" -o "$out/system/COMMAND.EXE"
    spawn "$cc" $flags "$ROOT/src/pkg/pkg.c" "$ROOT/src/pkg/dsp.c" "$ROOT/src/pkg/inflate.c" "$ROOT/src/pkg/deflate.c" \
        -o "$out/system/PKG.EXE"
    spawn "$cc" $flags -I"$ROOT/src" "$ROOT/src/setup/setup.c" "$ROOT/src/setup/install.c" "$ROOT/src/setup/target.c" "$ROOT/src/drivers/fat32.c" \
        "$ROOT/src/pkg/dsp.c" "$ROOT/src/pkg/inflate.c" "$ROOT/src/pkg/deflate.c" -o "$out/system/SETUP.EXE"
    for command in $COREUTILS; do
        source=$(echo "$command" | tr A-Z a-z)
        spawn "$cc" $flags "$ROOT/src/coreutils/$source.c" -o "$out/coreutils/$command.EXE"
    done
    spawn "$cc" $flags "$ROOT/src/coreutils/copy.c" -o "$out/coreutils/MOVE.EXE"
    finish_jobs
    for alias in MKDIR:MD RMDIR:RD ERASE:DEL RENAME:REN REBOOT:RESTART; do
        cp "$out/coreutils/${alias#*:}.EXE" "$out/coreutils/${alias%%:*}.EXE"
    done
    spawn "$cc" $flags "$ROOT/src/batch/batch.c" -o "$out/batch/BATCH.EXE"
    # The desktop package: DESKTOP.EXE and, in Apps, a process per program.
    desktop=$ROOT/src/desktop
    desktop_out=$out/desktop
    rm -rf "$desktop_out"
    mkdir -p "$desktop_out/Apps" "$desktop_out/Wallpapers"
    cp "$desktop"/wallpapers/*.jpg "$desktop_out/Wallpapers/"
    flags="$flags -Wno-missing-field-initializers"
    spawn "$cc" $flags -I"$desktop" "$desktop/desktop.c" "$desktop/gfx.c" "$desktop/apps.c" "$desktop/widgets.c" "$desktop/dialogs.c" \
        "$desktop/image.c" "$ROOT/src/pkg/inflate.c" -I"$ROOT/src/pkg" -o "$desktop_out/DESKTOP.EXE"
    gui="$desktop/gfx.c $desktop/gui.c $desktop/widgets.c $desktop/dialogs.c $desktop/image.c $ROOT/src/pkg/inflate.c"
    for app in explorer:EXPLORER taskmgr:TASKMGR calc:CALC editor:EDITOR paint:PAINT solitaire:SOLITAIRE mines:MINES chess:CHESS terminal:TERMINAL; do
        [ -f "$desktop/${app%%:*}.c" ] || continue
        spawn "$cc" $flags -I"$desktop" "$desktop/${app%%:*}.c" $gui -I"$ROOT/src/pkg" -lm -o "$desktop_out/Apps/${app#*:}.EXE"
    done
    spawn "$cc" $flags -I"$desktop" -I"$ROOT/src" "$desktop/pkgmgr.c" $gui "$ROOT/src/pkg/dsp.c" "$ROOT/src/pkg/deflate.c" -I"$ROOT/src/pkg" -lm \
        -o "$desktop_out/Apps/PKGMGR.EXE"
    spawn "$cc" $flags -I"$desktop" -I"$ROOT/src" "$desktop/setupgui.c" $gui "$ROOT/src/setup/install.c" "$ROOT/src/setup/target.c" \
        "$ROOT/src/drivers/fat32.c" "$ROOT/src/pkg/dsp.c" "$ROOT/src/pkg/deflate.c" -I"$ROOT/src/pkg" -lm -o "$desktop_out/Apps/SETUPGUI.EXE"
    spawn "$cc" $flags -I"$ROOT/src" "$ROOT/src/vdm/cpu.c" "$ROOT/src/vdm/bios.c" "$ROOT/src/vdm/video.c" "$ROOT/src/vdm/dos.c" "$ROOT/src/vdm/host.c" \
        -o "$desktop_out/Apps/DOSVM.EXE"
    finish_jobs
    python3 "$ROOT/tools/dsp.py" pack "$desktop_out" "$PACKAGES/desktop-$bits.dsp" Name="DOS-XC Desktop" Version="$VERSION" \
        Author="Ben Daws" Description="Graphical desktop and its programs" Arch="$bits" Path=. >/dev/null
    # Packages: coreutils and batch every build; the larger ones when their
    # files exist and have changed (toolchain/dist, build/ports).
    python3 "$ROOT/tools/dsp.py" pack "$out/coreutils" "$PACKAGES/coreutils-$bits.dsp" Name=coreutils Version="$VERSION" \
        Author="Ben Daws" Description="Basic DOS-XC commands (DIR, COPY, EDIT ...)" Arch="$bits" Path=. >/dev/null
    python3 "$ROOT/tools/dsp.py" pack "$out/batch" "$PACKAGES/batch-$bits.dsp" Name=batch Version="$VERSION" \
        Author="Ben Daws" Description="Batch file processor" Arch="$bits" Path=. >/dev/null
    # Device drivers: a shared object per adapter family, in one package.
    mkdir -p "$out/ethernet"
    for driver in e1000 tg3 virtio-net; do
        [ -f "$ROOT/src/netdrivers/$driver.c" ] || continue
        name=$(echo "$driver" | tr a-z A-Z | tr -d -)
        spawn "$cc" --sysroot="$sysroot" -shared -fPIC -nostdlib -ffreestanding -mno-red-zone -mno-sse -mno-mmx -O2 -Wall -Wextra \
            -Wno-unused-parameter -Wno-sign-compare -s -Wl,-z,max-page-size=4096 -Wl,--hash-style=both \
            "$ROOT/src/netdrivers/$driver.c" -o "$out/ethernet/$name.DRV"
    done
    mkdir -p "$out/hyperv"
    spawn "$cc" --sysroot="$sysroot" -shared -fPIC -nostdlib -ffreestanding -mno-red-zone -mno-sse -mno-mmx -O2 -Wall -Wextra \
        -Wno-unused-parameter -Wno-sign-compare -fvisibility=hidden -s -Wl,-z,max-page-size=4096 -Wl,--hash-style=both \
        -I"$ROOT/src/hyperv" "$ROOT"/src/hyperv/*.c -o "$out/hyperv/HYPERV.DRV"
    finish_jobs
    python3 "$ROOT/tools/dsp.py" pack "$out/hyperv" "$PACKAGES/hyperv-$bits.dsp" Name=hyperv Version=1.0 \
        Author="Ben Daws" Description="Hyper-V network, disks, keyboard and mouse" Arch="$bits" Driver=HYPERV.DRV >/dev/null
    python3 "$ROOT/tools/dsp.py" pack "$out/ethernet" "$PACKAGES/ethernet-$bits.dsp" Name=ethernet Version=1.0 \
        Author="Ben Daws" Description="Ethernet drivers (Intel, Broadcom, virtio)" Arch="$bits" \
        Driver=$(cd "$out/ethernet" && ls *.DRV | tr '\n' ';' | sed 's/;$//') >/dev/null
}
compile_userland 32 i686-dosxc i386
compile_userland 64 x86_64-dosxc x86_64

# GCC: toolchain/dist/ARCH/GCC (toolchain/build-toolchain.sh ARCH), with the
# current C library.
package_gcc() {
    bits=$1
    arch=$2
    dist=$ROOT/toolchain/dist/$arch/GCC
    target=$PACKAGES/GCC-$bits.dsp
    [ -d "$dist" ] || return 0
    if [ -f "$target" ] && [ -z "$(find "$dist" "$PART/sysroot$bits/lib/libc.a" -newer "$target" -print -quit)" ]; then
        return 0
    fi
    echo "Packing GCC for the $bits-bit edition"
    rm -rf "$PART/gcc$bits"
    cp -R "$dist" "$PART/gcc$bits"
    cp -R "$PART/sysroot$bits/include/." "$PART/gcc$bits/include/"
    cp "$PART/sysroot$bits/lib/"*.o "$PART/sysroot$bits/lib/"*.a "$PART/sysroot$bits/lib/"*.so "$PART/gcc$bits/lib/"
    python3 "$ROOT/tools/dsp.py" pack "$PART/gcc$bits" "$target" Name=GCC Version=15.2.0 \
        Author="Free Software Foundation" Description="GNU Compiler Collection" Arch="$bits" Path=BIN >/dev/null
    rm -rf "$PART/gcc$bits"
}
package_gcc 32 i686
package_gcc 64 x86_64

# Ports (ports/build-ports.sh ARCH): build/ports/ARCH/<Name>.
package_port() {
    bits=$1
    arch=$2
    name=$3
    shift 3
    tree=$BUILD/ports/$arch/$name
    target=$PACKAGES/$name-$bits.dsp
    [ -d "$tree" ] || return 0
    if [ -f "$target" ] && [ -z "$(find "$tree" -newer "$target" -print -quit)" ]; then return 0; fi
    python3 "$ROOT/tools/dsp.py" pack "$tree" "$target" Name="$name" Arch="$bits" "$@" >/dev/null
}
for bits_arch in 32:i686 64:x86_64; do
    package_port "${bits_arch%%:*}" "${bits_arch#*:}" xz Version=5.8.1 Author="The Tukaani Project" \
        Description="Compression utility" Path=BIN
    package_port "${bits_arch%%:*}" "${bits_arch#*:}" curl Version=8.11.1 Author="Daniel Stenberg and the curl contributors" \
        Description="HTTP and HTTPS transfer tool" Path=BIN Env=CURL_CA_BUNDLE=%PACKAGE%/CACERT.PEM
    package_port "${bits_arch%%:*}" "${bits_arch#*:}" Vim Version=9.1.1000 Author="Bram Moolenaar and the Vim community" \
        Description="Fast, keyboard-only terminal text editor" Path=BIN Env=VIM=%PACKAGE%/share/vim
    package_port "${bits_arch%%:*}" "${bits_arch#*:}" python3 Version=3.14.6 Author="Python Software Foundation" \
        Description="Python programming language" Path=BIN Env=PYTHONHOME=%PACKAGE% Env=SSL_CERT_FILE=%PACKAGE%/CACERT.PEM
done

# The Oregon Trail, a 16-bit DOS game, from OREGON.ZIP when it is present.
# Its INSTALL.EXE (the floppy installer) is not needed.
if [ -f "$ROOT/OREGON.ZIP" ] && { [ ! -f "$PACKAGES/Oregon-16.dsp" ] || [ "$ROOT/OREGON.ZIP" -nt "$PACKAGES/Oregon-16.dsp" ]; }; then
    rm -rf "$PART/oregon"
    python3 -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); [z.extract(i, sys.argv[2]) for i in z.infolist() if i.filename.upper() != "INSTALL.EXE"]' \
        "$ROOT/OREGON.ZIP" "$PART/oregon"
    python3 "$ROOT/tools/dsp.py" pack "$PART/oregon" "$PACKAGES/Oregon-16.dsp" Name=Oregon Version=1.0 Author=MECC \
        Description="The Oregon Trail" Arch=16 Path=. StartIn=program >/dev/null
fi

python3 "$ROOT/tools/pack_dos.py" "$PART"
"$NASM" -f bin -I"$PART/" -I"$ROOT/src/dos/" "$ROOT/src/dos/runtime.asm" -o "$PART/dos-runtime.bin"

"$CC" --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -c \
    "$ROOT/src/boot/uefi.c" -o "$PART/uefi.o"
"$CC" --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -nostdinc \
    -I"$ROOT/src" -c "$ROOT/src/drivers/fat32.c" -o "$PART/uefi-fat32.o"
"$LD" -flavor link /subsystem:efi_application /entry:efi_main /out:"$PART/iso64/EFI/BOOT/BOOTX64.EFI" \
    "$PART/uefi.o" "$PART/uefi-fat32.o"

"$NASM" -f bin "$ROOT/src/boot/mbr.asm" -o "$PART/mbr.bin"
"$NASM" -f bin "$ROOT/src/boot/volume.asm" -o "$PART/volume.bin"
"$NASM" -f bin -dFAT32 "$ROOT/src/boot/volume.asm" -o "$PART/volume32.bin"
"$NASM" -f bin "$ROOT/src/boot/fsloader.asm" -o "$PART/loader32.bin"
"$NASM" -f bin -dDOS_XC_64 "$ROOT/src/boot/fsloader.asm" -o "$PART/loader64.bin"
for bits in 32 64; do
    python3 "$ROOT/tools/make_disk.py" "$PART" "$bits" "$PACKAGES"
done
"$NASM" -f bin "$ROOT/src/boot/boot.asm" -o "$PART/boot.bin"
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
build_payload 32 "$PART/kernel32.bin" "$ROOT/src/boot/stage2.asm" "$PART/32/payload.bin"
build_payload 64 "$PART/kernel64.bin" "$ROOT/src/boot/stage2.asm" "$PART/64/payload.bin"
cat "$PART/boot.bin" "$PART/32/payload.bin" > "$PART/iso32/boot/dosxc.img"
cat "$PART/boot.bin" "$PART/64/payload.bin" > "$PART/iso64/boot/dosxc.img"

"$XORRISO" -as mkisofs -b boot/dosxc.img -no-emul-boot -boot-info-table -boot-load-size 4 \
    -o "$ISO32" "$PART/iso32"
"$XORRISO" -as mkisofs -b boot/dosxc.img -no-emul-boot -boot-info-table -boot-load-size 4 \
    -eltorito-alt-boot -e boot/efi.img -no-emul-boot \
    -o "$ISO64" "$PART/iso64"
printf 'Built %s and %s\n' "$ISO32" "$ISO64"


python3 "$ROOT/tools/finalize_iso.py" "$ISO32"
python3 "$ROOT/tools/finalize_iso.py" "$ISO64"
