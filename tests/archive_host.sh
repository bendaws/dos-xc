#!/bin/zsh
#
# DOS-XC /tests/archive_host.sh
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

set -u
ROOT=${0:a:h:h}
WORK=$(mktemp -d)
export COPYFILE_DISABLE=1
A=$WORK/arctool
clang -O1 -g -fsanitize=address,undefined -I"$ROOT/desktop/archive" "$ROOT"/desktop/archive/*.c "$ROOT/tests/programs/arctool.c" -o $A || exit 1
mkdir -p $WORK/src/data/sub/deeper $WORK/src/data/emptydir
python3 -I - $WORK/src/data <<'PY'
import os, random, sys
os.chdir(sys.argv[1]); random.seed(1)
open('text.txt', 'w').write(''.join(f'line {i} the quick brown fox {i * i % 97}\n' for i in range(20000)))
open('random.bin', 'wb').write(os.urandom(300000))
open('empty.txt', 'w').close()
b = bytearray()
while len(b) < 6_000_000:
    b += bytes([random.randrange(4)] * random.randrange(1, 8)) + b'ABCDEFGH' * random.randrange(1, 6) + os.urandom(random.randrange(0, 6))
open('sub/big.dat', 'wb').write(bytes(b[:6_000_000]))
open('sub/deeper/' + 'x' * 120 + '.txt', 'w').write('long name\n')
open('sub/one.bin', 'wb').write(b'\xe8\x00\x00\x00\x00' * 1000)
PY
failed=0
check() { if diff -r $WORK/src/data "$1" >/dev/null; then echo "PASS $2"; else echo "FAIL $2"; failed=1; fi; }
cd $WORK
for f in "1 zip" "2 7z" "4 tar" "5 tar.gz" "6 tar.xz"; do
    set -- ${=f}
    $A c $1 a.$2 src/data || { echo "FAIL make $2"; failed=1; continue; }
    mkdir own_$2 ext_$2
    $A x a.$2 own_$2 && check own_$2/data "$2 read back"
    case $2 in zip|7z) (cd ext_$2 && 7zz x -y ../a.$2 >/dev/null) ;; *) (cd ext_$2 && bsdtar -xf ../a.$2) ;; esac
    check ext_$2/data "$2 read by another tool"
done
xz -t a.tar.xz && gzip -t a.tar.gz && 7zz t a.7z >/dev/null && 7zz t a.zip >/dev/null && echo "PASS integrity tests" || { echo "FAIL integrity tests"; failed=1; }
(cd src && 7zz a -bd ../e.7z data >/dev/null && 7zz a -bd -m0=lzma -mf=BCJ ../e_bcj.7z data >/dev/null &&
 7zz a -bd -m0=delta:4 -m1=lzma2 ../e_delta.7z data >/dev/null && 7zz a -bd -m0=deflate ../e_deflate.7z data >/dev/null &&
 7zz a -bd -tzip ../e.zip data >/dev/null && 7zz a -bd -tzip -mm=lzma ../e_lzma.zip data >/dev/null &&
 bsdtar -czf ../e.tar.gz data && bsdtar -cJf ../e.tar.xz data && bsdtar --format=pax -cf ../e.tar data)
for e in e.7z e_bcj.7z e_delta.7z e_deflate.7z e.zip e_lzma.zip e.tar.gz e.tar.xz e.tar; do
    mkdir x_$e && $A x $e x_$e && check x_$e/data "read $e" || { echo "FAIL read $e"; failed=1; }
done
rm -rf $WORK
exit $failed
