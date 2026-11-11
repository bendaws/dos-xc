#
# DOS-XC /scripts/common.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$HERE")
. "$ROOT/scripts/parallel.sh"
BUILD=${BUILD:-"$ROOT/build"}
PART=$BUILD/part
PACKAGES=$BUILD/packages
NASM=${NASM:-nasm}
CC=${CC:-clang}
LD=${LD:-ld.lld}
XORRISO=${XORRISO:-xorriso}
OBJCOPY=${OBJCOPY:-llvm-objcopy}
if ! command -v "$OBJCOPY" >/dev/null 2>&1 && [ -x /opt/homebrew/opt/llvm/bin/llvm-objcopy ]; then
    OBJCOPY=/opt/homebrew/opt/llvm/bin/llvm-objcopy
fi
CROSS=${CROSS:-"$ROOT/toolchain/cross/bin"}
export BUILD NASM CC LD XORRISO OBJCOPY CROSS
# package.sh [BITS...]: the editions to build; both unless named or chosen
# through scripts/build.sh.
EDITIONS=${*:-${DOSXC_EDITIONS:-32 64}}
mkdir -p "$PART" "$PACKAGES"
[ -f "$PART/version.json" ] || python3 "$ROOT/tools/version.py" "$PART"
VERSION=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["version"])' "$PART/version.json")

# Select an edition: sets bits, triple, arch, cc, sysroot, out and flags.  The
# global build makes the C library once; a package built alone makes its own.
use_edition() {
    bits=$1
    case "$bits" in
    32) triple=i686-dosxc arch=i686 libc_arch=i386 ;;
    64) triple=x86_64-dosxc arch=x86_64 libc_arch=x86_64 ;;
    *) echo "error: an edition is 32 or 64, not $bits" >&2; exit 1 ;;
    esac
    cc=$CROSS/$triple-gcc
    [ -x "$cc" ] || { echo "error: $cc not found; run gcc/build.sh first" >&2; exit 1; }
    sysroot=$PART/sysroot$bits
    out=$PART/user$bits
    flags="--sysroot=$sysroot -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation -Wno-misleading-indentation -Wno-stringop-truncation -s"
    driver_flags="--sysroot=$sysroot -shared -fPIC -nostdlib -ffreestanding -mno-red-zone -mno-sse -mno-mmx -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -s -Wl,-z,max-page-size=4096 -Wl,--hash-style=both"
    [ -n "${DOSXC_SYSROOT_READY:-}" ] || "$ROOT/os/libc/build.sh" "$libc_arch" "$sysroot" clang >/dev/null
    mkdir -p "$out"
}

# Put build/packages/FILE on the media of this build, if it exists.  Packages
# left over from earlier builds are only used when something asks for them.
on_media() {
    [ -f "$PACKAGES/$1" ] && echo "$1" >> "$PART/media.list" || :
}

pack() {
    _pack_tree=$1
    _pack_file=$2
    shift 2
    python3 "$ROOT/tools/dsp.py" pack "$_pack_tree" "$PACKAGES/$_pack_file" "$@" >/dev/null
    on_media "$_pack_file"
}

# A third-party port, built by its build.sh into build/ports/ARCH/NAME: pack
# it if it exists and has changed.  package_port NAME KEY=VALUE...
package_port() {
    _port_name=$1
    shift
    _port_tree=$BUILD/ports/$arch/$_port_name
    _port_target=$PACKAGES/$_port_name-$bits.dsp
    if [ -d "$_port_tree" ] && { [ ! -f "$_port_target" ] || [ -n "$(find "$_port_tree" -newer "$_port_target" -print -quit)" ]; }; then
        pack "$_port_tree" "$_port_name-$bits.dsp" Name="$_port_name" Arch="$bits" "$@"
    else
        on_media "$_port_name-$bits.dsp"
    fi
}
