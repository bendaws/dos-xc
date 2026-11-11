#!/bin/sh
#
# DOS-XC /scripts/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${BUILD:-"$ROOT/build"}
SELECTION=$BUILD/selection.txt

usage() { sed -n '2,14s/^# \{0,1\}//p' "$0"; }

ALL=
for script in "$ROOT"/*/package.sh; do
    ALL="$ALL $(basename "$(dirname "$script")")"
done
editions="32 64"
components=$ALL
while [ $# -gt 0 ]; do
    case "$1" in
    --select|-s)
        mkdir -p "$BUILD"
        python3 "$ROOT/tools/select_components.py" "$ROOT" "$SELECTION" || exit 1
        . "$SELECTION"
        ;;
    --last)
        [ -f "$SELECTION" ] || { echo "error: nothing chosen yet; use --select" >&2; exit 1; }
        . "$SELECTION"
        ;;
    --only)
        [ $# -ge 2 ] || { echo "error: --only needs a list of packages" >&2; exit 1; }
        components=$(echo "$2" | tr ',' ' ')
        shift
        ;;
    --editions)
        [ $# -ge 2 ] || { echo "error: --editions needs 32, 64 or 32,64" >&2; exit 1; }
        editions=$(echo "$2" | tr ',' ' ')
        shift
        ;;
    --help|-h) usage; exit 0 ;;
    *) echo "error: unknown option $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done
for name in $components; do
    case " $ALL " in
    *" $name "*) ;;
    *) echo "error: no package folder named $name (there are:$ALL)" >&2; exit 1 ;;
    esac
done
for edition in $editions; do
    case "$edition" in 32|64) ;; *) echo "error: an edition is 32 or 64, not $edition" >&2; exit 1 ;; esac
done
[ -n "$editions" ] || { echo "error: choose at least one edition" >&2; exit 1; }
case " $components " in *" coreutils "*) ;; *) components="coreutils $components" ;; esac
DOSXC_EDITIONS=$editions
export DOSXC_EDITIONS

rm -rf "$BUILD/part" "$BUILD/iso"
. "$ROOT/scripts/common.sh"

command -v "$NASM" >/dev/null || { echo "error: NASM is required" >&2; exit 1; }
command -v "$CC" >/dev/null || { echo "error: Clang is required" >&2; exit 1; }
command -v "$LD" >/dev/null || { echo "error: lld is required" >&2; exit 1; }
command -v "$OBJCOPY" >/dev/null || { echo "error: llvm-objcopy is required" >&2; exit 1; }
command -v "$XORRISO" >/dev/null || { echo "error: xorriso is required" >&2; exit 1; }

for edition in $EDITIONS; do
    use_edition "$edition"
done
DOSXC_SYSROOT_READY=1
export DOSXC_SYSROOT_READY

"$ROOT/os/build.sh"
for name in $components; do
    "$ROOT/$name/package.sh"
done
"$ROOT/os/image.sh"
