#!/bin/sh
# Teach pristine GNU source trees about the *-dosxc target and host.
# Usage: patch-sources.sh SRC_DIR   (containing gcc-*, binutils-*, gmp-*, ...)
# Every edit is idempotent, so the script may be re-run on a patched tree.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC=$1
GCC=$(ls -d "$SRC"/gcc-* | head -1)
BINUTILS=$(ls -d "$SRC"/binutils-* | head -1)

cp "$HERE/gcc-config/dosxc.h" "$GCC/gcc/config/dosxc.h"
cp "$HERE/gcc-config/xm-dosxc.h" "$GCC/gcc/config/xm-dosxc.h"
cp "$HERE/gcc-config/dosxc.opt" "$GCC/gcc/config/dosxc.opt"
cp "$HERE/gcc-config/dosxc.opt.urls" "$GCC/gcc/config/dosxc.opt.urls"

python3 -I - "$SRC" "$GCC" "$BINUTILS" <<'EOF'
import pathlib, re, sys
src, gcc, binutils = (pathlib.Path(p) for p in sys.argv[1:4])

def edit(path, marker, anchor, text, after=False):
    """Insert TEXT before (or after) the first line matching ANCHOR unless
    MARKER is already present."""
    body = path.read_text()
    if marker in body:
        return
    lines = body.splitlines(keepends=True)
    for i, line in enumerate(lines):
        if re.search(anchor, line):
            lines.insert(i + 1 if after else i, text)
            path.write_text(''.join(lines))
            return
    sys.exit(f'error: anchor {anchor!r} not found in {path}')

def substitute(path, marker, old, new):
    body = path.read_text()
    if marker in body:
        return
    if old not in body:
        sys.exit(f'error: {old!r} not found in {path}')
    path.write_text(body.replace(old, new, 1))

# 1. config.sub: accept "dosxc" as an operating system.
for sub in list(src.rglob('config.sub')) + list(src.rglob('configfsf.sub')):
    if '| rtems* ' in sub.read_text():
        substitute(sub, 'dosxc', '| rtems* ', '| rtems* | dosxc* ')

# 2. binutils: plain ELF vectors and emulations.
for name, pattern in (('bfd/config.bfd', '  {}-*-elf* | {}-*-rtems* '),
                      ('ld/configure.tgt', '{}-*-elf* | {}-*-rtems* ')):
    path = binutils / name
    for cpu in ('x86_64', 'i[3-7]86'):
        old = pattern.format(cpu, cpu)
        new = old.replace('-*-elf* |', f'-*-elf* | {cpu}-*-dosxc* |')
        substitute(path, f'{cpu}-*-dosxc*', old, new)
edit(binutils / 'gas/configure.tgt', 'dosxc', r'^  i386-\*-elf\*\)',
     '  i386-*-dosxc*)\t\t\tfmt=elf ;;\n')

# 3. GCC target description.
edit(gcc / 'gcc/config.gcc', '*-*-dosxc*)', r'^\*-\*-fuchsia\*\)', '''\
*-*-dosxc*)
  gas=yes
  gnu_ld=yes
  default_use_cxa_atexit=yes
  use_gcc_stdint=wrap
  ;;
''')
edit(gcc / 'gcc/config.gcc', 'x86_64-*-dosxc*)', r'^i\[34567\]86-\*-elf\*\)', '''\
i[34567]86-*-dosxc*)
\ttm_file="${tm_file} i386/unix.h i386/att.h elfos.h glibc-stdint.h i386/i386elf.h dosxc.h"
\t;;
x86_64-*-dosxc*)
\ttm_file="${tm_file} i386/unix.h i386/att.h elfos.h glibc-stdint.h i386/i386elf.h i386/x86-64.h dosxc.h"
\t;;
''')

# -pthread is accepted (threads are in the C library).
edit(gcc / 'gcc/config.gcc', 'dosxc.opt', r'^\*-\*-dosxc\*\)$', 'extra_options="${extra_options} dosxc.opt"\n', after=True)

# Threads are always linked, so gthreads calls them directly, not through weak symbols.
edit(gcc / 'libgcc/gthr.h', '__dosxc__', r'^#ifndef GTHREAD_USE_WEAK', """#ifdef __dosxc__
#define GTHREAD_USE_WEAK 0
#endif
""")

# 4. GCC host description: programs are *.exe.
edit(gcc / 'gcc/config.host', 'dosxc', r'^  i\[34567\]86-pc-msdosdjgpp\*\)', '''\
  *-*-dosxc*)
    host_xm_file=xm-dosxc.h
    host_exeext=.exe
    ;;
''')

# 5. libgcc: PIC crtstuff, frame-registry unwinding (no dl_iterate_phdr).
edit(gcc / 'libgcc/config.host', '*-*-dosxc*)', r'^\*-\*-dragonfly\*\)', '''\
*-*-dosxc*)
  tmake_file="$tmake_file t-crtstuff-pic t-libgcc-pic"
  extra_parts="crtbegin.o crtend.o crtbeginS.o crtendS.o"
  ;;
''')
edit(gcc / 'libgcc/config.host', 'x86_64-*-dosxc* |', r'^x86_64-\*-elf\* \| x86_64-\*-rtems\*\)', '''\
x86_64-*-dosxc* | i[34567]86-*-dosxc*)
\ttmake_file="$tmake_file i386/t-crtstuff"
\t;;
''')

# 6. A PIE host compiler (mandatory on Apple silicon) links the PIC libiberty,
#    which is otherwise only built for the LTO plugin.  Build it whenever the
#    host executables are PIE.
edit(gcc / 'configure', 'libiberty for PIE hosts', r'^# Sometimes we have special requirements for the host zlib\.', '''\
# DOS-XC: libiberty for PIE hosts.
if test x"$host_pie" = xyes || test x"$host_shared" = xyes; then
  extra_host_libiberty_configure_flags=--enable-shared
fi

''')

# 7. Case-insensitive hosts: match lowercase suffixes ignoring case first,
#    so FOO.C is compiled as C (GCC normally treats .C as C++).
substitute(gcc / 'gcc/gcc.cc', 'GCC_CASE_INSENSITIVE_SUFFIXES', '''  /* Look for a suffix.  */
  for (cp = compilers + n_compilers - 1; cp >= compilers; cp--)''', '''#ifdef GCC_CASE_INSENSITIVE_SUFFIXES
  for (cp = compilers + n_compilers - 1; cp >= compilers; cp--)
    if (cp->suffix[0] == '.' && strlen (cp->suffix) < length
	&& !strpbrk (cp->suffix, "ABCDEFGHIJKLMNOPQRSTUVWXYZ")
	&& !strcasecmp (cp->suffix, name + length - strlen (cp->suffix)))
      break;
  if (cp < compilers)
#endif
  /* Look for a suffix.  */
  for (cp = compilers + n_compilers - 1; cp >= compilers; cp--)''')

# 8. Paths have drive letters ("C:/DIR") and '\\' separators, and PATH uses
#    ';', as on DJGPP: DOS-XC is a DOS-based file system.
for tree in (gcc, binutils):
    substitute(tree / 'include/filenames.h', '__dosxc__',
               '#if defined(__MSDOS__) || (defined(_WIN32) && ! defined(__CYGWIN__)) || \\\n    defined(__OS2__)',
               '#if defined(__MSDOS__) || (defined(_WIN32) && ! defined(__CYGWIN__)) || \\\n    defined(__OS2__) || defined(__dosxc__)')
    substitute(tree / 'libiberty/make-relative-prefix.c', '__dosxc__',
               '    || defined (__DJGPP__) || defined (__OS2__)',
               '    || defined (__DJGPP__) || defined (__OS2__) || defined (__dosxc__)')
substitute(binutils / 'ld/lexsup.c', '__dosxc__',
           '#if defined (__MSDOS__) || (defined (_WIN32) && ! defined (__CYGWIN32__))',
           '#if defined (__MSDOS__) || (defined (_WIN32) && ! defined (__CYGWIN32__)) || defined (__dosxc__)')

# 9. libstdc++: probe the C library with link tests like other hosted OSes.
linux = '  *-linux* | *-uclinux* | *-gnu* | *-kfreebsd*-gnu | *-cygwin* | *-solaris*)'
for name in ('libstdc++-v3/crossconfig.m4', 'libstdc++-v3/configure'):
    substitute(gcc / name, '*-dosxc*', linux, linux[:-1] + ' | *-dosxc*)')
EOF
echo "Patched $GCC and $BINUTILS for *-dosxc"
