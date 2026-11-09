#!/usr/bin/env python3
"""Adapt CPython's standard library to DOS-XC.

  ports/python-dosxc.py PYTHON-SOURCE

CPython runs here as a POSIX system (os.name is "posix"), but DOS-XC paths
start with a drive letter, PATH is separated by ";", there are no symbolic
links, and processes are created with posix_spawn because there is no fork().
"""
import pathlib
import sys

SOURCE = pathlib.Path(sys.argv[1])


def patch(name, *edits, append=''):
    path = SOURCE / name
    text = path.read_text(encoding='utf-8')
    for old, new in edits:
        if text.count(old) != 1:
            raise SystemExit(f'error: {name}: expected one copy of: {old!r}')
        text = text.replace(old, new)
    path.write_text(text + append, encoding='utf-8')


patch('Modules/getpath.py',
      ("    DELIM = ':'\n", "    DELIM = ';'\n"))

patch('Python/fileutils.c',
      ("    return (path[0] == SEP);\n",
       "    return (path[0] == SEP || (path[0] && path[1] == L':' && (path[2] == SEP || path[2] == L'\\\\')));\n"))

patch('Lib/importlib/_bootstrap_external.py',
      ("""        \"\"\"Replacement for os.path.isabs.\"\"\"
        return path.startswith(path_separators)
""", """        \"\"\"Replacement for os.path.isabs.\"\"\"
        return path.startswith(path_separators) or path[1:3] in (':/', ':\\\\')
"""))

patch('Lib/posixpath.py',
      ("pathsep = ':'\n", "pathsep = ';'\n"),
      ("defpath = '/bin:/usr/bin'\n", "defpath = '.'\n"),
      ("""    sep = _get_sep(s)
    return s.startswith(sep)
""", """    sep = _get_sep(s)
    drive = (b':/', b':\\\\') if isinstance(s, bytes) else (':/', ':\\\\')
    return s.startswith(sep) or s[1:3] in drive
"""),
      ("            if b.startswith(sep) or not path:\n", "            if isabs(b) or not path:\n"),
      ("        if not path.startswith(b'/'):\n", "        if not isabs(path):\n"),
      ("        if not path.startswith('/'):\n", "        if not isabs(path):\n"),
      append='''

# DOS-XC has no symbolic links, and a path may begin with a drive letter.
def realpath(filename, *, strict=False):
    """Return the absolute form of a path."""
    if strict is True:
        os.stat(filename)
    return abspath(filename)
''')

patch('Lib/subprocess.py',
      ("""            if shell:
                # On Android the default shell is at '/system/bin/sh'.
""", """            if shell and sys.platform == 'dosxc':
                # There is no /bin/sh: run the words of the command directly.
                import shlex
                lexer = shlex.shlex(args[0], posix=True)
                lexer.escape = ''
                lexer.whitespace_split = True
                args = list(lexer) + args[1:]
                shell = False

            if shell:
                # On Android the default shell is at '/system/bin/sh'.
"""),
      ("""            if (_USE_POSIX_SPAWN
                    and os.path.dirname(executable)
""", """            if sys.platform == 'dosxc':
                # No fork(): the child runs to completion inside posix_spawn,
                # so changing directory around the call is safe.
                import shutil
                found = shutil.which(executable, path=(env or os.environ).get('PATH'))
                if found is None:
                    raise FileNotFoundError(errno.ENOENT, os.strerror(errno.ENOENT), executable)
                saved_cwd = os.getcwd() if cwd is not None else None
                if saved_cwd is not None:
                    os.chdir(cwd)
                try:
                    self._posix_spawn(args, found, env, False, False,
                                      p2cread, p2cwrite,
                                      c2pread, c2pwrite,
                                      errread, errwrite)
                finally:
                    if saved_cwd is not None:
                        os.chdir(saved_cwd)
                return

            if (_USE_POSIX_SPAWN
                    and os.path.dirname(executable)
"""))

# Programs are named *.EXE.
patch('Lib/shutil.py',
      ("        files = [cmd]\n",
       "        files = [cmd, cmd + '.EXE'] if sys.platform == 'dosxc' and not use_bytes else [cmd]\n"))

# socketpair() does not exist here.  The self-pipe only wakes the event loop
# from other threads, and there are none.
patch('Lib/asyncio/selector_events.py',
      ("""    def _close_self_pipe(self):
        self._remove_reader(self._ssock.fileno())
""", """    def _close_self_pipe(self):
        if self._ssock is None:
            return
        self._remove_reader(self._ssock.fileno())
"""),
      ("""        # A self-socket, really. :-)
        self._ssock, self._csock = socket.socketpair()
""", """        # A self-socket, really. :-)
        import sys
        if sys.platform == 'dosxc':
            self._ssock = self._csock = None
            return
        self._ssock, self._csock = socket.socketpair()
"""))
