#!/usr/bin/env python3
"""Create an MBR/FAT16 installer image. Writes regular build files only."""
import json
import struct
import sys
import zipfile
import os
import shutil
from pathlib import Path

BUILD=Path(sys.argv[1]); BITS=sys.argv[2]
VERSION=json.loads((BUILD/"version.json").read_text())
SECTORS=131072; PART=2048; SPF=128; SPC=4; ROOT=PART+4+2*SPF; DATA=ROOT+32
fat=bytearray(SPF*512);struct.pack_into('<HH',fat,0,0xfff8,0xffff)
next_cluster=2
image=bytearray(SECTORS*512)
locations={}

def allocate(data):
    global next_cluster
    count=max(1,(len(data)+2047)//2048)
    first=next_cluster
    next_cluster+=count
    if next_cluster>=((SECTORS-DATA)//4)+2:raise SystemExit('Installer filesystem is full')
    for n in range(count):struct.pack_into('<H',fat,(first+n)*2,0xffff if n==count-1 else first+n+1)
    offset=(DATA+(first-2)*4)*512
    image[offset:offset+len(data)]=data
    return first

def entry(name,cluster,size,directory=False):
    if name in ('.','..'):short=name.encode().ljust(11,b' ')
    else:
        parts=name.upper().split('.')
        assert len(parts[0])<=8 and (len(parts)==1 or len(parts[1])<=3), name
        short=parts[0].encode().ljust(8,b' ')+(parts[1] if len(parts)>1 else '').encode().ljust(3,b' ')
    result=bytearray(32);result[:11]=short;result[11]=0x10 if directory else 0x20
    struct.pack_into('<HH',result,22,12<<11,((2026-1980)<<9)|(11<<5)|6)
    struct.pack_into('<H',result,26,cluster);struct.pack_into('<I',result,28,size)
    return result

def directory(tree,path='',parent=0,root=False):
    cluster=0 if root else allocate(bytes(2048))
    entries=bytearray()
    if not root:entries+=entry('.',cluster,0,True)+entry('..',parent,0,True)
    for name,content in tree.items():
        full=path+'/'+name
        if isinstance(content,dict):child=directory(content,full,cluster);size=0;is_dir=True
        else:
            child=allocate(content);size=len(content);is_dir=False
            locations[full]={'cluster':child,'lba':DATA+(child-2)*4,'size':size}
        entries+=entry(name,child,size,is_dir)
    assert len(entries)<(512*32 if root else 2048), path
    offset=ROOT*512 if root else (DATA+(cluster-2)*4)*512
    image[offset:offset+len(entries)]=entries
    return cluster

mbr=bytearray((BUILD/'mbr.bin').read_bytes())
serial=0xbd050000+int(BITS)
struct.pack_into('<I',mbr,440,serial)
mbr[446:462]=struct.pack('<B3sB3sII',0x80,b'\xfe\xff\xff',0xef,b'\xfe\xff\xff',PART,SECTORS-PART)
system={
 'CMD':{p.name:p.read_bytes() for p in sorted((BUILD/f'cmd{BITS}').glob('*.EXE'))},
 'BATCH':{'BATCH.EXE':(BUILD/f'batch{BITS}.exe').read_bytes()},
 'LOADER.BIN':(BUILD/f'loader{BITS}.bin').read_bytes(),
 'KERNEL.BIN':(BUILD/f'kernel{BITS}.bin').read_bytes(),
 'CORE.DLL':(BUILD/f'core{BITS}.dll').read_bytes(),
 'SHELL.DLL':(BUILD/f'shell{BITS}.dll').read_bytes(),
 'DOSAPI.DLL':(BUILD/'dos-runtime.bin').read_bytes(),
 'DOSDATA.BIN':(BUILD/'dos-data.bin').read_bytes(),
 'FAT32.BIN':(BUILD/'volume32.bin').read_bytes(),
 'INSTALL.EXE':(BUILD/f'install{BITS}.exe').read_bytes(),
 'SETUP.BIN':(BUILD/'setup.bin').read_bytes(),
 'SETUP.CFG':b'SETUP=1\r\n',
 'MBR.BIN':bytes(mbr),
 'KERNELU.BIN':(BUILD/'kerneluefi.bin').read_bytes(),
 'COREU.DLL':(BUILD/'coreuefi.dll').read_bytes(),
 'SHELLU.DLL':(BUILD/'shelluefi.dll').read_bytes(),
 'VERSION.TXT':(VERSION["label"]+"\r\nSystem directory: C:\\SYSTEM\r\n").encode("ascii"),
}
system['CMD'].update({alias:system['CMD'][name] for alias,name in [('MKDIR.EXE','MD.EXE'),('RMDIR.EXE','RD.EXE'),('ERASE.EXE','DEL.EXE'),('RENAME.EXE','REN.EXE')]})
tree={'SYSTEM':system,'EFI':{'BOOT':{'BOOTX64.EFI':(BUILD/'iso64/EFI/BOOT/BOOTX64.EFI').read_bytes()}}}
archive=os.environ.get('DOS_PROGRAMS_ZIP')
if archive:
    with zipfile.ZipFile(archive) as source:
        for item in source.infolist():
            if not item.is_dir():
                name=item.filename.replace('\\','/').rsplit('/',1)[-1].upper()
                if name in tree:raise SystemExit(f'Archive name conflicts with system directory: {name}')
                tree[name]=source.read(item)
directory(tree,root=True)
image[(PART+4)*512:(PART+4+SPF)*512]=fat
image[(PART+4+SPF)*512:(PART+4+2*SPF)*512]=fat
vbr=bytearray((BUILD/'volume.bin').read_bytes())
struct.pack_into('<I',vbr,39,serial)
loader=locations['/SYSTEM/LOADER.BIN']
struct.pack_into('<IHBBI',vbr,0x1a4,loader['lba'],(loader['size']+511)//512,1,int(BITS),DATA+(next_cluster-2)*4)
struct.pack_into('<III',vbr,0x1b0,PART,locations['/SYSTEM/SETUP.CFG']['lba'],locations['/SYSTEM/DOSDATA.BIN']['lba'])
struct.pack_into('<I',vbr,0x1bc,1)
struct.pack_into('<II',vbr,0x1c0,locations['/SYSTEM/SETUP.BIN']['lba'],(locations['/SYSTEM/SETUP.BIN']['size']+2047)//2048)
image[:512]=mbr;image[PART*512:(PART+1)*512]=vbr
out=BUILD/f'BenDOS-Setup-x{BITS}.img'
out.write_bytes(image)
(BUILD/f'filesystem{BITS}.json').write_text(json.dumps(locations,indent=2))
print(f'Built {out} (MBR + FAT16, \\SYSTEM, BIOS/UEFI setup)')

def export(tree, destination):
    destination.mkdir(parents=True,exist_ok=True)
    for name,content in tree.items():
        target=destination/name
        if isinstance(content,dict):export(content,target)
        else:target.write_bytes(content)
export(tree,BUILD/f'iso{BITS}')
if BITS=='64':
    # A compact, valid FAT16 EFI boot image keeps the El Torito length below
    # 65536 sectors, including on firmware that does not implement length zero.
    used=DATA+(next_cluster-2)*4-PART
    efi_sectors=max(32768,((used+2047)//2048)*2048)
    efi=bytearray(image[PART*512:(PART+efi_sectors)*512])
    struct.pack_into('<H',efi,19,efi_sectors if efi_sectors<65536 else 0)
    struct.pack_into('<I',efi,32,0 if efi_sectors<65536 else efi_sectors)
    (BUILD/'iso64/boot/efi.img').write_bytes(efi)

shutil.copyfile(out,BUILD/f'iso{BITS}/boot/system.img')
