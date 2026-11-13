"""Validate DSP archives without extracting files or buffering their payloads."""
import hashlib
import lzma
import re
import struct
import zipfile
import zlib
from pathlib import PurePosixPath

CHUNK = 64 * 1024
MAX_UNPACKED = 2 * 1024**3


def exact(stream, size):
    data = stream.read(size)
    if len(data) != size:
        raise ValueError('Truncated package.')
    return data


def safe_name(name):
    path = PurePosixPath(name)
    if (not name or name.rstrip('/') != path.as_posix() or str(path) == '.'
            or len(name.encode('utf-8')) > 259 or path.is_absolute()
            or '\\' in name or ':' in name or '..' in path.parts
            or any(ord(c) < 32 or ord(c) == 127 for c in name)
            or name.startswith('/') or any(p.endswith((' ', '.')) for p in path.parts)):
        raise ValueError('Unsafe filename inside package.')


def inspect_package(path):
    try:
        return _inspect(path)
    except (EOFError, struct.error, UnicodeError, lzma.LZMAError,
            zipfile.BadZipFile, NotImplementedError, RuntimeError, zlib.error) as exc:
        raise ValueError('Invalid or damaged DSP archive.') from exc


def _inspect(path):
    with open(path, 'rb') as f:
        magic, version, flags, length = struct.unpack('<4sHHI', exact(f, 12))
        if magic != b'DSPK' or version not in (1, 2) or flags or not 0 < length <= 65536:
            raise ValueError('Unsupported DSP header.')
        text = exact(f, length).decode('utf-8')
        if any(ord(c) < 32 and c not in '\r\n' for c in text) or '\x7f' in text:
            raise ValueError('Invalid metadata characters.')
        info = {}
        for line in text.splitlines():
            if not line or line.startswith((';', '#')):
                continue
            key, sep, value = line.partition('=')
            if sep:
                key = key.lower()
                if key in info and key != 'env':
                    raise ValueError('Duplicate metadata field.')
                info[key] = info.get(key, '') + ('\n' if key in info else '') + value
        name, release, arch = (info.get(k, '') for k in ('name', 'version', 'arch'))
        if (not re.fullmatch(r'[A-Za-z0-9_-][A-Za-z0-9 _.\-]{0,39}', name)
                or name.endswith(' ') or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.+\-]{0,30}', release)
                or arch not in ('16', '32', '64')):
            raise ValueError('Invalid Name, Version or Arch metadata.')
        for key, limit in {'author': 95, 'description': 159, 'path': 255, 'startin': 31,
                           'driver': 255, 'provides': 127, 'requires': 255,
                           'firewall': 255, 'env': 510}.items():
            if len(info.get(key, '').encode('utf-8')) > limit:
                raise ValueError(f'{key} metadata exceeds the package manager limit.')
        if version == 2:
            count, = struct.unpack('<I', exact(f, 4))
            if count > 100000:
                raise ValueError('Too many package files.')
            entries, names, total = [], set(), 0
            for _ in range(count):
                nlen, size, crc = struct.unpack('<HII', exact(f, 10))
                if not 0 < nlen <= 259:
                    raise ValueError('Invalid package filename length.')
                filename = exact(f, nlen).decode('utf-8')
                safe_name(filename)
                if filename.casefold() in names:
                    raise ValueError('Duplicate package filename.')
                names.add(filename.casefold())
                total += size
                if total > MAX_UNPACKED:
                    raise ValueError('Unpacked package exceeds 2 GiB.')
                entries.append((size, crc))
            # Bounded output and dictionary size, including malicious XZ streams.
            decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=64 * 1024**2)
            pending = b''

            def read_output():
                if decoder.eof:
                    return b''
                data = f.read(CHUNK) if decoder.needs_input else b''
                if decoder.needs_input and not data:
                    raise ValueError('Truncated compressed payload.')
                return decoder.decompress(data, max_length=CHUNK)

            for size, expected in entries:
                remaining, crc = size, 0
                while remaining:
                    if not pending:
                        pending = read_output()
                        if not pending:
                            if decoder.eof:
                                raise ValueError('Truncated file payload.')
                            continue
                    part, pending = pending[:remaining], pending[remaining:]
                    remaining -= len(part)
                    crc = zlib.crc32(part, crc)
                if crc != expected:
                    raise ValueError('File CRC mismatch.')
            if pending:
                raise ValueError('Unexpected data after package files.')
            while not decoder.eof:
                if read_output():
                    raise ValueError('Unexpected data after package files.')
            if decoder.unused_data or f.read(1):
                raise ValueError('Unexpected data after XZ stream.')
        else:
            with zipfile.ZipFile(f) as archive:
                entries = archive.infolist()
                if len(entries) > 100000 or sum(e.file_size for e in entries) > MAX_UNPACKED:
                    raise ValueError('Archive exceeds package limits.')
                names = set()
                for entry in entries:
                    safe_name(entry.filename)
                    if entry.filename.casefold() in names or entry.compress_type not in (0, 8):
                        raise ValueError('Duplicate filename or unsupported ZIP compression.')
                    names.add(entry.filename.casefold())
                    with archive.open(entry) as body:
                        while body.read(CHUNK):
                            pass  # ZipFile validates sizes and CRC on read.
        f.seek(0)
        digest = hashlib.file_digest(f, 'sha256').hexdigest()
    return info, digest


def compare_versions(a, b):
    """Match dsp_compare_versions: numeric runs as numbers, other chars folded."""
    i = j = 0
    while i < len(a) or j < len(b):
        if i < len(a) and j < len(b) and a[i].isdigit() and b[j].isdigit():
            x, y = i, j
            while i < len(a) and a[i].isdigit():
                i += 1
            while j < len(b) and b[j].isdigit():
                j += 1
            left, right = int(a[x:i]), int(b[y:j])
        else:
            left = ord(a[i].lower()) if i < len(a) else 0
            right = ord(b[j].lower()) if j < len(b) else 0
            i += i < len(a)
            j += j < len(b)
        if left != right:
            return (left > right) - (left < right)
    return 0
