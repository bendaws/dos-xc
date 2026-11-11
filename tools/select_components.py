#!/usr/bin/env python3
#
# DOS-XC /tools/select_components.py
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

import curses
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1])
SELECTION = Path(sys.argv[2])
REQUIRED = {'coreutils'}

def title(folder):
    """The heading of the folder's README."""
    readme = folder / 'README.md'
    if readme.exists():
        match = re.search(r'^# (.+)$', readme.read_text(), re.M)
        if match:
            return match[1]
    return folder.name

def saved():
    """The previous choice, or None."""
    if not SELECTION.exists():
        return None
    return dict(re.findall(r'^(\w+)="(.*)"$', SELECTION.read_text(), re.M))

class Item:
    def __init__(self, key, label, on, locked=False):
        self.key, self.label, self.on, self.locked = key, label, on or locked, locked

def build_items():
    previous = saved()
    chosen = set(previous['components'].split()) if previous else None
    editions = set(previous['editions'].split()) if previous else {'32', '64'}
    items = [Item('32', '32-bit edition (x86)', '32' in editions), Item('64', '64-bit edition (x64)', '64' in editions)]
    names = sorted(p.parent for p in ROOT.glob('*/package.sh'))
    packages = [Item(p.name, f'{p.name:<12} {title(p)}', chosen is None or p.name in chosen, p.name in REQUIRED)
                for p in names]
    return items, packages

def run(screen, editions, packages):
    curses.curs_set(0)
    rows = [None, *editions, None, *packages]
    headings = {0: 'Editions', len(editions) + 1: 'Packages'}
    selectable = [i for i, row in enumerate(rows) if row]
    cursor, top = 0, 0
    message = ''
    while True:
        screen.erase()
        height, width = screen.getmaxyx()
        def put(y, text, attribute=0):
            if 0 <= y < height:
                screen.addnstr(y, 0, text, max(width - 1, 0), attribute)
        put(0, 'DOS-XC build: the core system is always included', curses.A_BOLD)
        put(1, 'Up/Down move   Space toggle   A all   N none   Enter build   Q cancel')
        area = max(height - 4, 1)
        at = selectable[cursor]
        if at < top:
            top = at
        elif at >= top + area:
            top = at - area + 1
        for offset, row in enumerate(rows[top:top + area]):
            index = top + offset
            y = 3 + offset
            if row is None:
                put(y, headings[index], curses.A_UNDERLINE)
                continue
            mark = '[*]' if row.locked else '[x]' if row.on else '[ ]'
            note = '  (required)' if row.locked else ''
            put(y, f'  {mark} {row.label}{note}', curses.A_REVERSE if index == at else 0)
        if message:
            put(height - 1, message, curses.A_BOLD)
        screen.refresh()
        key = screen.getch()
        message = ''
        row = rows[at]
        if key in (curses.KEY_UP, ord('k')):
            cursor = max(cursor - 1, 0)
        elif key in (curses.KEY_DOWN, ord('j')):
            cursor = min(cursor + 1, len(selectable) - 1)
        elif key == ord(' '):
            if not row.locked:
                row.on = not row.on
        elif key in (ord('a'), ord('A')):
            for item in packages:
                item.on = True
        elif key in (ord('n'), ord('N')):
            for item in packages:
                item.on = item.locked
        elif key in (10, 13, curses.KEY_ENTER):
            if any(item.on for item in editions):
                return True
            message = 'Choose at least one edition.'
        elif key in (ord('q'), ord('Q')):           # not Esc: it starts arrow-key sequences
            return False

def main():
    if not (sys.stdin.isatty() and sys.stdout.isatty()):
        raise SystemExit('error: the checklist needs a terminal; use --only and --editions instead')
    editions, packages = build_items()
    if not curses.wrapper(run, editions, packages):
        print('Build cancelled.', file=sys.stderr)
        return 1
    chosen_editions = ' '.join(item.key for item in editions if item.on)
    chosen_packages = ' '.join(item.key for item in packages if item.on)
    SELECTION.write_text(f'editions="{chosen_editions}"\ncomponents="{chosen_packages}"\n')
    print(f'Building editions: {chosen_editions}\nBuilding packages: {chosen_packages}')
    return 0

if __name__ == '__main__':
    sys.exit(main())
