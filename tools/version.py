#!/usr/bin/env python3
"""Generate non-C version resources from the single kernel header definition."""
import json
from pathlib import Path
import re
import sys
header=(Path(__file__).resolve().parents[1]/'src/kernel/bootinfo.h').read_text()
def value(name):
    match=re.search(r'^#define\s+'+name+r'\s+"([^"\n]+)"\s*$',header,re.M)
    if not match:raise SystemExit(f'Missing string definition: {name}')
    return match[1]
name,version=value('BENDOS_NAME'),value('BENDOS_VERSION')
if not re.fullmatch(r'[0-9A-Za-z.\-]+',version):raise SystemExit('Invalid version format')
build=Path(sys.argv[1])
(build/'version.json').write_text(json.dumps({'name':name,'version':version,'label':f'{name} v{version}'}))
major=int(re.search(r'^#define\s+BENDOS_DOS_COMPAT_MAJOR\s+(\d+)',header,re.M)[1])
minor=int(re.search(r'^#define\s+BENDOS_DOS_COMPAT_MINOR\s+(\d+)',header,re.M)[1])
(build/'version.inc').write_text(f'%define BENDOS_VERSION "{version}"\n%define BENDOS_DOS_COMPAT_MAJOR {major}\n%define BENDOS_DOS_COMPAT_MINOR {minor}\n')
