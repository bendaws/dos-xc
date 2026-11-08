#!/usr/bin/env python3
"""Create the MBR/FAT16 image of the live installation media.

  tools/make_disk.py BUILD BITS PACKAGES

The image holds \\SYSTEM, the EFI loader, empty \\TEMP and \\Home folders,
\\Programs with the coreutils and batch packages installed, \\Packages
with every package for this edition (for Setup and PKG), any extra trees
named in DOSXC_EXTRA_TREES or DOSXC_EXTRA_TREES_32/_64 (host directories
separated by os.pathsep) and optional DOS programs from DOS_PROGRAMS_ZIP.
The FAT16 geometry grows with the content and keeps free space for
programs that write files on the live (read-only) media.
"""
import json
import os
import shutil
import struct
import sys
import zipfile
from pathlib import Path

BUILD = Path(sys.argv[1])
BITS = sys.argv[2]
PACKAGES = Path(sys.argv[3])
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dsp  # noqa: E402
ARCH = 'x86' if BITS == '32' else 'x64'
VERSION = json.loads((BUILD / "version.json").read_text())
PART = 2048
RESERVED = 4
ROOT_ENTRIES = 512
FREE_SPACE = 64 << 20          # writable headroom on live media
MIN_SECTORS = 131072           # 64 MiB, the installer's minimum
DATE = ((2026 - 1980) << 9) | (11 << 5) | 6
TIME = 12 << 11
SHORT_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'()-@^_`{}~")


def is_short(name):
    """An exact upper-case 8.3 name needs no long-name entries."""
    if name in ('.', '..'):
        return True
    base, dot, ext = name.partition('.')
    return (1 <= len(base) <= 8 and len(ext) <= 3 and '.' not in ext and
            all(c in SHORT_CHARS for c in base + ext) and (ext or not dot))


def short_alias(name, taken):
    if is_short(name):
        base, _, ext = name.partition('.')
        alias = base.ljust(8) + ext.ljust(3)
        taken.add(alias)
        return alias.encode('ascii')
    clean = lambda s: ''.join(c if c in SHORT_CHARS else '_' for c in s.upper().replace(' ', '').replace('.', ''))
    stem, dot, ext = name.rpartition('.')
    if not dot or not stem:
        stem, ext = name, ''
    base, ext = clean(stem) or '_', clean(ext)[:3]
    for n in range(1, 1000000):
        tail = f'~{n}'
        alias = (base[:8 - len(tail)] + tail).ljust(8) + ext.ljust(3)
        if alias not in taken:
            taken.add(alias)
            return alias.encode('ascii')
    raise SystemExit(f'too many similar names near {name}')


def checksum(alias):
    total = 0
    for b in alias:
        total = (((total & 1) << 7) + (total >> 1) + b) & 0xff
    return total


def entries_for(name, alias, cluster, size, directory):
    """Directory entries (long-name entries first) for one file."""
    result = bytearray()
    if not is_short(name):
        units = list(name.encode('utf-16-le'))
        chars = [units[i] | (units[i + 1] << 8) for i in range(0, len(units), 2)]
        pieces = (len(chars) + 12) // 13
        chars += [0] + [0xffff] * (pieces * 13 - len(chars) - 1) if len(chars) % 13 else []
        for number in range(pieces, 0, -1):
            e = bytearray(32)
            e[0] = number | (0x40 if number == pieces else 0)
            e[11] = 0x0f
            e[13] = checksum(alias)
            part = chars[(number - 1) * 13:number * 13]
            for i, offset in enumerate((1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)):
                struct.pack_into('<H', e, offset, part[i])
            result += e
    e = bytearray(32)
    e[:11] = alias
    e[11] = 0x10 if directory else 0x20
    struct.pack_into('<HH', e, 22, TIME, DATE)
    struct.pack_into('<H', e, 26, cluster)
    struct.pack_into('<I', e, 28, size)
    return result + e


def entry_count(name):
    return 1 if is_short(name) else 1 + (len(name.encode('utf-16-le')) // 2 + 12) // 13


def load_tree(path):
    tree = {}
    for child in sorted(Path(path).iterdir(), key=lambda p: p.name.lower()):
        if child.name.startswith('.'):
            continue
        tree[child.name] = load_tree(child) if child.is_dir() else child.read_bytes()
    return tree


def merge(into, extra, where=''):
    for name, content in extra.items():
        existing = next((k for k in into if k.upper() == name.upper()), None)
        if existing is None:
            into[name] = content
        elif isinstance(into[existing], dict) and isinstance(content, dict):
            merge(into[existing], content, where + '/' + name)
        else:
            raise SystemExit(f'extra tree conflicts with {where}/{name}')


def clusters_needed(tree, cluster_bytes, root=False):
    total = 0
    if not root:
        slots = 2 + sum(entry_count(n) for n in tree)
        total += max(1, (slots * 32 + cluster_bytes - 1) // cluster_bytes)
    for content in tree.values():
        if isinstance(content, dict):
            total += clusters_needed(content, cluster_bytes)
        else:
            total += max(1, (len(content) + cluster_bytes - 1) // cluster_bytes)
    return total


# ---- content -----------------------------------------------------------
mbr = bytearray((BUILD / 'mbr.bin').read_bytes())
serial = 0xbd050000 + int(BITS)
struct.pack_into('<I', mbr, 440, serial)
user = BUILD / f'user{BITS}' / 'system'
system = {
    'COMMAND.EXE': (user / 'COMMAND.EXE').read_bytes(),
    'PKG.EXE': (user / 'PKG.EXE').read_bytes(),
    'LIB': {'libc.so': (BUILD / f'sysroot{BITS}' / 'lib' / 'libc.so').read_bytes()},
    'Drivers': {},
    'SETUP.EXE': (user / 'SETUP.EXE').read_bytes(),
    'LOADER.BIN': (BUILD / f'loader{BITS}.bin').read_bytes(),
    'KERNEL.BIN': (BUILD / f'kernel{BITS}.bin').read_bytes(),
    'CORE.DLL': (BUILD / f'core{BITS}.dll').read_bytes(),
    'SHELL.DLL': (BUILD / f'shell{BITS}.dll').read_bytes(),
    'DOSAPI.DLL': (BUILD / 'dos-runtime.bin').read_bytes(),
    'DOSDATA.BIN': (BUILD / 'dos-data.bin').read_bytes(),
    'FAT32.BIN': (BUILD / 'volume32.bin').read_bytes(),
    'ELFAPI.DLL': (BUILD / f'elfapi{BITS}.dll').read_bytes(),
    'SETUP.CFG': b'SETUP=1\r\n',
    'MBR.BIN': bytes(mbr),
    'KERNELU.BIN': (BUILD / 'kerneluefi.bin').read_bytes(),
    'COREU.DLL': (BUILD / 'coreuefi.dll').read_bytes(),
    'SHELLU.DLL': (BUILD / 'shelluefi.dll').read_bytes(),
    'VERSION.TXT': (VERSION["label"] + "\r\nSystem directory: C:\\SYSTEM\r\n").encode("ascii"),
}
tree = {'SYSTEM': system, 'EFI': {'BOOT': {'BOOTX64.EFI': (BUILD / 'iso64/EFI/BOOT/BOOTX64.EFI').read_bytes()}},
        'TEMP': {}, 'Home': {}, 'Programs': {}, 'Packages': {}}

# Packages for this edition (and 16-bit ones), named <Name>.dsp on the media.
for package in sorted(PACKAGES.glob('*.dsp')):
    stem, _, bits = package.stem.rpartition('-')
    if bits in (BITS, '16'):
        tree['Packages'][stem + '.dsp'] = package.read_bytes()
# The live system comes with coreutils and batch installed.
staging = BUILD / f'live{BITS}'
shutil.rmtree(staging, ignore_errors=True)
for name in ('coreutils', 'batch'):
    dsp.install(PACKAGES / f'{name}-{BITS}.dsp', staging)
tree['Programs'] = load_tree(staging / 'Programs')
extra_trees = os.pathsep.join(os.environ.get(name, '') for name in
                              ('DOSXC_EXTRA_TREES', f'DOSXC_EXTRA_TREES_{BITS}'))
for extra in filter(None, extra_trees.split(os.pathsep)):
    merge(tree, load_tree(extra))
archive = os.environ.get('DOS_PROGRAMS_ZIP')
if archive:
    with zipfile.ZipFile(archive) as source:
        for item in source.infolist():
            if not item.is_dir():
                name = item.filename.replace('\\', '/').rsplit('/', 1)[-1].upper()
                if name in tree:
                    raise SystemExit(f'Archive name conflicts with system directory: {name}')
                tree[name] = source.read(item)

# ---- geometry: smallest FAT16 cluster that fits content + headroom ------
root_slots = sum(entry_count(n) for n in tree)
if root_slots > ROOT_ENTRIES:
    raise SystemExit('Too many entries in the root directory')
for SPC in (4, 8, 16, 32, 64):
    cluster_bytes = SPC * 512
    used = clusters_needed(tree, cluster_bytes, root=True)
    clusters = max(used + FREE_SPACE // cluster_bytes, (MIN_SECTORS - PART) // SPC)
    if clusters <= 65524:
        break
else:
    raise SystemExit('Content does not fit a FAT16 file system')
SPF = (2 * (clusters + 2) + 511) // 512
ROOT = PART + RESERVED + 2 * SPF
DATA = ROOT + ROOT_ENTRIES * 32 // 512
clusters = min(clusters, 65524)
SECTORS = DATA + clusters * SPC
if SECTORS < MIN_SECTORS:
    SECTORS = MIN_SECTORS
    clusters = min((SECTORS - DATA) // SPC, 65524)
fat = bytearray(SPF * 512)
struct.pack_into('<HH', fat, 0, 0xfff8, 0xffff)
image = bytearray(SECTORS * 512)
next_cluster = 2
locations = {}


def lba_of(cluster):
    return DATA + (cluster - 2) * SPC


def allocate(size):
    global next_cluster
    count = max(1, (size + cluster_bytes - 1) // cluster_bytes)
    first = next_cluster
    next_cluster += count
    if next_cluster > clusters + 2:
        raise SystemExit('Installer filesystem is full')
    for n in range(count):
        struct.pack_into('<H', fat, (first + n) * 2, 0xffff if n == count - 1 else first + n + 1)
    return first


def directory(tree, path='', parent=0, root=False):
    slots = 2 + sum(entry_count(n) for n in tree)
    cluster = 0 if root else allocate(slots * 32)
    taken = set()
    entries = bytearray()
    if not root:
        entries += entries_for('.', b'.          ', cluster, 0, True)
        entries += entries_for('..', b'..         ', parent, 0, True)
    for name, content in tree.items():
        alias = short_alias(name, taken)
        full = path + '/' + name
        if isinstance(content, dict):
            child, size, is_dir = directory(content, full, cluster), 0, True
        else:
            child, size, is_dir = allocate(len(content)), len(content), False
            offset = lba_of(child) * 512
            image[offset:offset + size] = content
            locations[full] = {'cluster': child, 'lba': lba_of(child), 'size': size}
        entries += entries_for(name, alias, child, size, is_dir)
    offset = ROOT * 512 if root else lba_of(cluster) * 512
    image[offset:offset + len(entries)] = entries
    return cluster


mbr[446:462] = struct.pack('<B3sB3sII', 0x80, b'\xfe\xff\xff', 0xef, b'\xfe\xff\xff', PART, SECTORS - PART)
directory(tree, root=True)
image[(PART + RESERVED) * 512:(PART + RESERVED + SPF) * 512] = fat
image[(PART + RESERVED + SPF) * 512:(PART + RESERVED + 2 * SPF) * 512] = fat
vbr = bytearray((BUILD / 'volume.bin').read_bytes())
vbr[13] = SPC
struct.pack_into('<H', vbr, 14, RESERVED)
struct.pack_into('<H', vbr, 17, ROOT_ENTRIES)
struct.pack_into('<H', vbr, 19, 0)
struct.pack_into('<H', vbr, 22, SPF)
struct.pack_into('<I', vbr, 32, SECTORS - PART)
struct.pack_into('<I', vbr, 39, serial)
loader = locations['/SYSTEM/LOADER.BIN']
struct.pack_into('<IHBBI', vbr, 0x1a4, loader['lba'], (loader['size'] + 511) // 512, 1, int(BITS), lba_of(next_cluster))
struct.pack_into('<III', vbr, 0x1b0, PART, locations['/SYSTEM/SETUP.CFG']['lba'], locations['/SYSTEM/DOSDATA.BIN']['lba'])
struct.pack_into('<I', vbr, 0x1bc, 1)
struct.pack_into('<II', vbr, 0x1c0, 0, 0)
image[:512] = mbr
image[PART * 512:(PART + 1) * 512] = vbr
out = BUILD / f'DOS-XC-Setup-{ARCH}.img'
out.write_bytes(image)
(BUILD / f'filesystem{BITS}.json').write_text(json.dumps(locations, indent=2))
print(f'Built {out} (MBR + FAT16, {SECTORS // 2048} MiB, {cluster_bytes // 1024} KiB clusters, '
      f'{(clusters + 2 - next_cluster) * cluster_bytes >> 20} MiB free)')


def export(tree, destination):
    destination.mkdir(parents=True, exist_ok=True)
    for name, content in tree.items():
        target = destination / name
        if isinstance(content, dict):
            export(content, target)
        else:
            target.write_bytes(content)


# Browsable copies on the CD: the system files only (extra trees live in the image).
export({k: v for k, v in tree.items() if k in ('SYSTEM', 'EFI')}, BUILD / f'iso{BITS}')
if BITS == '64':
    # The El Torito EFI image is the partition itself.  Small images stay
    # below 65536 sectors for firmware that cannot handle a zero length.
    used = lba_of(next_cluster) - PART
    efi_sectors = max(32768, ((used + 2047) // 2048) * 2048)
    if efi_sectors > 65535:
        efi_sectors = SECTORS - PART      # large content: keep the free space too
    efi = bytearray(image[PART * 512:(PART + efi_sectors) * 512])
    struct.pack_into('<H', efi, 19, efi_sectors if efi_sectors < 65536 else 0)
    struct.pack_into('<I', efi, 32, 0 if efi_sectors < 65536 else efi_sectors)
    (BUILD / 'iso64/boot/efi.img').write_bytes(efi)

shutil.copyfile(out, BUILD / f'iso{BITS}/boot/system.img')
