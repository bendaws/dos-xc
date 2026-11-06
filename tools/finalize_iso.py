#!/usr/bin/env python3
"""Bind the BIOS loader to the ISO filesystem image and update its checksum."""
from pathlib import Path
import struct
import sys
path=Path(sys.argv[1])
with path.open('r+b')as f:
    f.seek(16*2048);pvd=f.read(2048)
    assert pvd[1:6]==b'CD001'
    def directory(extent,size):
        f.seek(extent*2048);data=f.read(size);at=0
        while at<len(data):
            if not data[at]:at=((at//2048)+1)*2048;continue
            record=data[at:at+data[at]];at+=data[at]
            yield record[33:33+record[32]].decode('ascii').split(';')[0],struct.unpack_from('<I',record,2)[0],struct.unpack_from('<I',record,10)[0]
    root=struct.unpack_from('<I',pvd,158)[0],struct.unpack_from('<I',pvd,166)[0]
    def find(parts):
        extent,size=root
        for part in parts:
            matches=[(a,b)for name,a,b in directory(extent,size)if name.upper()==part.upper()]
            assert len(matches)==1,(parts,part)
            extent,size=matches[0]
        return extent,size
    filesystem,_=find(['BOOT','SYSTEM.IMG'])
    boot,size=find(['BOOT','BENDOS.IMG'])
    f.seek(boot*2048);data=bytearray(f.read(size))
    marker=data.find(b'FSIM',2048,2048+65536)
    assert marker>=0 and data.find(b'FSIM',marker+1,2048+65536)<0
    struct.pack_into('<I',data,marker+4,filesystem)
    padded=data[64:]+bytes(-len(data[64:])%4)
    checksum=sum(value[0]for value in struct.iter_unpack('<I',padded))&0xffffffff
    struct.pack_into('<I',data,20,checksum)
    f.seek(boot*2048);f.write(data)
print(f'Bound BIOS filesystem in {path.name}')
