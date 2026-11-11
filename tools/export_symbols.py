#!/usr/bin/env python3
#
# DOS-XC /tools/export_symbols.py
# Copyright (c) 2026 Ben Daws - see LICENSE.txt
#

import re
import struct
import sys
from pathlib import Path
blob=Path(sys.argv[1]).read_bytes()
bits=blob[4]
if bits==1:
    off=struct.unpack_from('<I',blob,32)[0]
    size,count=struct.unpack_from('<HH',blob,46)
    sections=[struct.unpack_from('<10I',blob,off+i*size) for i in range(count)]
else:
    off=struct.unpack_from('<Q',blob,40)[0]
    size,count=struct.unpack_from('<HH',blob,58)
    sections=[struct.unpack_from('<II4QII2Q',blob,off+i*size) for i in range(count)]
prefixes=tuple(sys.argv[3].split(','))
exports=[]
for section in sections:
    if section[1]!=2:continue
    strings_section=sections[section[6]]
    strings=blob[strings_section[4]:strings_section[4]+strings_section[5]]
    for at in range(section[4],section[4]+section[5],section[9]):
        if bits==1:name,value,_,info,_,index=struct.unpack_from('<IIIBBH',blob,at)
        else:name,info,_,index,value,_=struct.unpack_from('<IBBHQQ',blob,at)
        label=strings[name:].split(b'\0',1)[0].decode('ascii')
        if info>>4 in (1,2) and index and label.startswith(prefixes) and re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*',label):
            exports.append(f'{label} = {value:#x};\n')
Path(sys.argv[2]).write_text(''.join(exports))
