#!/bin/sh
#
# DOS-XC /firefox/build.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$HERE")
COMMIT=47f2a88cbab9072c8f8f156494b92d6891e488fb
RUST=1.99.1
SRC=$ROOT/toolchain/work/firefox
export ROOT
export RUSTUP_HOME=$ROOT/toolchain/work/rustup CARGO_HOME=$ROOT/toolchain/work/cargo

if [ ! -d "$SRC/.git" ]; then
    git init -q "$SRC"
    git -C "$SRC" remote add origin https://github.com/mozilla-firefox/firefox
    git -C "$SRC" fetch --depth 1 origin "$COMMIT"
    git -C "$SRC" checkout -q FETCH_HEAD
fi

stamp=$SRC/.git/dosxc-patches
wanted=$(cat "$HERE"/patches/*.patch | cksum)
if [ "$(cat "$stamp" 2>/dev/null || true)" != "$wanted" ]; then
    git -C "$SRC" reset -q --hard "$COMMIT"
    git -C "$SRC" clean -fdq
    for patch in "$HERE"/patches/*.patch; do
        git -C "$SRC" apply "$patch"
    done
    echo "$wanted" > "$stamp"
fi

rustup run "$RUST" rustc --version >/dev/null 2>&1 ||
    rustup toolchain install "$RUST" --profile minimal --target x86_64-unknown-linux-musl --no-self-update
[ -x "$CARGO_HOME/bin/cbindgen" ] || rustup run "$RUST" cargo install cbindgen --locked
rust_bin=$(dirname "$(rustup which --toolchain "$RUST" rustc)")

llvm_bin=$(command -v llvm-config || true)
[ -n "$llvm_bin" ] || llvm_bin=$(ls /opt/homebrew/opt/llvm/bin/llvm-config /usr/lib/llvm-*/bin/llvm-config 2>/dev/null | head -1)
[ -n "$llvm_bin" ] || { echo "error: LLVM (llvm-config, libclang) is needed on the build machine" >&2; exit 1; }
DOSXC_LIBCLANG=$("$llvm_bin" --libdir)
export DOSXC_LIBCLANG
export PATH="$rust_bin:$CARGO_HOME/bin:$ROOT/toolchain/cross/bin:$(dirname "$llvm_bin"):$PATH"
export MOZCONFIG=$HERE/mozconfig MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE=none
export RUSTC=$rust_bin/rustc CARGO=$rust_bin/cargo
export HOST_CC=clang HOST_CXX=clang++

cd "$SRC"
exec ./mach "${@:-build}"
