#!/usr/bin/env python3
"""Drive a DOS-XC session in QEMU and print the console transcript.

The kernel mirrors console text to the QEMU debug port (0xE9); keystrokes
are typed through the QEMU monitor.

Example:
  tests/qemu_session.py --iso build/iso/DOSXC_0.2_x64.iso -c VER -c "HELLO.EXE a b"
Exit status is 0 when every command returned to the prompt in time.

A command starting with "!" is a QEMU monitor command instead (for example
"!device_add usb-storage,bus=xhci.0,drive=stick,id=s1" to plug in a USB
disk), "~SECONDS" just waits, and "&TEXT" types TEXT (no Enter) without
waiting for a prompt, for programs that read the keyboard.  --qemu-arg adds QEMU options, e.g.
  --qemu-arg="-device qemu-xhci,id=xhci" --qemu-arg="-device usb-kbd,bus=xhci.0"
"""
import argparse
import os
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import time

KEYS = {' ': 'spc', '\n': 'ret', '\\': 'backslash', '/': 'slash', '.': 'dot', '-': 'minus',
        '_': 'shift-minus', ':': 'shift-semicolon', ';': 'semicolon', '=': 'equal', '+': 'shift-equal',
        '"': 'shift-apostrophe', "'": 'apostrophe', ',': 'comma', '<': 'shift-comma', '>': 'shift-dot',
        '*': 'shift-8', '(': 'shift-9', ')': 'shift-0', '!': 'shift-1', '@': 'shift-2', '#': 'shift-3',
        '$': 'shift-4', '%': 'shift-5', '^': 'shift-6', '&': 'shift-7', '[': 'bracket_left',
        ']': 'bracket_right', '{': 'shift-bracket_left', '}': 'shift-bracket_right', '|': 'shift-backslash',
        '?': 'shift-slash', '`': 'grave_accent', '~': 'shift-grave_accent'}
PROMPT = re.compile(r'[A-Z]:\\[^\r\n>]*>$')


def key_name(c):
    if c in KEYS:
        return KEYS[c]
    if c.isdigit() or c.islower():
        return c
    if c.isupper():
        return 'shift-' + c.lower()
    raise ValueError(f'cannot type {c!r}')


class Session:
    def __init__(self, args):
        self.dir = tempfile.mkdtemp(prefix='dosxc-qemu-')
        self.log = pathlib.Path(self.dir, 'console.txt')
        self.monitor_path = os.path.join(self.dir, 'monitor')
        media = args.iso or args.hda or ''
        qemu = args.qemu or ('qemu-system-i386' if 'x86' in pathlib.Path(media).name else 'qemu-system-x86_64')
        command = [qemu, '-m', str(args.memory), '-display', 'none', '-no-reboot',
                   '-debugcon', f'file:{self.log}', '-monitor', f'unix:{self.monitor_path},server,nowait',
                   ]
        if args.iso:
            command += ['-cdrom', args.iso, '-boot', 'd']
        if args.hda:
            command += ['-drive', f'file={args.hda},format=raw,if=ide,index=0']
        if args.uefi:
            command += ['-drive', f'if=pflash,format=raw,readonly=on,file={args.uefi}', '-machine', 'q35']
        for extra in args.qemu_arg:
            command += extra.split()
        self.process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(200):
            if os.path.exists(self.monitor_path) or self.process.poll() is not None:
                break
            time.sleep(0.05)
        if self.process.poll() is not None:
            raise SystemExit('error: QEMU exited: ' + self.process.stderr.read().decode(errors='replace'))
        self.monitor = socket.socket(socket.AF_UNIX)
        self.monitor.connect(self.monitor_path)
        self.monitor.settimeout(0.2)
        self.drain()

    def drain(self):
        try:
            while self.monitor.recv(65536):
                pass
        except (socket.timeout, BlockingIOError):
            pass

    def hmp(self, line):
        self.monitor.sendall(line.encode() + b'\n')
        time.sleep(0.03)
        self.drain()

    def text(self):
        return self.log.read_bytes().decode('latin-1') if self.log.exists() else ''

    def type(self, text):
        for c in text:
            self.hmp('sendkey ' + key_name(c))

    def wait_prompt(self, after, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.process.poll() is not None:
                return False
            body = self.text()
            if len(body) > after and PROMPT.search(body[after:].rstrip(' ')):
                return True
            time.sleep(0.2)
        return False

    def close(self):
        try:
            self.hmp('quit')
        except OSError:
            pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--iso', help='boot CD image')
    parser.add_argument('--qemu')
    parser.add_argument('--memory', type=int, default=1024)
    parser.add_argument('--hda', help='raw disk image attached as the first hard disk')
    parser.add_argument('--uefi', help='firmware image (e.g. OVMF) for UEFI boot')
    parser.add_argument('-c', '--command', action='append', default=[])
    parser.add_argument('--timeout', type=float, default=300, help='seconds allowed per command')
    parser.add_argument('--boot-timeout', type=float, default=60)
    parser.add_argument('--screendump', help='write a PPM screenshot here when finished')
    parser.add_argument('--qemu-arg', action='append', default=[], help='extra QEMU options (split at spaces)')
    args = parser.parse_args()

    session = Session(args)
    ok = True
    try:
        deadline = time.time() + args.boot_timeout
        while time.time() < deadline and not PROMPT.search(session.text().rstrip(' ')):
            time.sleep(1.5)
        if not PROMPT.search(session.text().rstrip(' ')):
            print('error: no shell prompt after boot', file=sys.stderr)
            ok = False
        for command in args.command if ok else []:
            if command.startswith('!'):
                session.hmp(command[1:])
                print(f'[ monitor] {command[1:]}', file=sys.stderr)
                time.sleep(0.5)
                continue
            if command.startswith('~'):
                time.sleep(float(command[1:]))
                continue
            if command.startswith('&'):             # type into a running program, no prompt expected
                session.type(command[1:])
                time.sleep(0.5)
                continue
            mark = len(session.text())
            started = time.time()
            session.type(command + '\n')
            finished = session.wait_prompt(mark + len(command), args.timeout)
            print(f'[{time.time() - started:6.1f}s] {command}', file=sys.stderr)
            if not finished:
                print(f'error: command did not finish: {command}', file=sys.stderr)
                ok = False
                break
        if args.screendump:
            session.hmp(f'screendump {os.path.abspath(args.screendump)}')
            time.sleep(0.5)
    finally:
        transcript = session.text()
        session.close()
    sys.stdout.write(transcript.replace('\r\n', '\n').replace('\r', '\n'))
    if not transcript.endswith('\n'):
        sys.stdout.write('\n')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
