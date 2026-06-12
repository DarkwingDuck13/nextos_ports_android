#!/usr/bin/env python3
"""
DYSMANTLE pak fixer — APKs modados ("APK_Award") removem os JPEG/PNG de UI
deixando size=0, esperando a versão .ktx (ETC2, GLES3). Mali-450 Utgard é GLES2
(sem ETC2) -> a engine cai no .jpg/.png VAZIO -> crash no compilador/loader.
Este script decodifica os .ktx ETC2 -> JPEG/PNG e preenche os slots vazios:
 - in-place se couber no slot do .ktx (mesmo offset)
 - senão anexa no fim (move o índice, atualiza idx_offset+filesize)
Formato pak: magic "PAK\0V11\0"(8) + idx_offset(u32) + filesize(u32);
índice em idx_offset: por entrada = nome\0 + offset(u32)+size(u32)+hash(u32)+pad(u32).
Requer: pip install --break-system-packages texture2ddecoder pillow
Uso: python3 fix_empty_textures.py <pak> [<pak2> ...]
"""
import struct, zlib, io, sys, texture2ddecoder
from PIL import Image
def parse(d):
    idx=struct.unpack('<I',d[8:12])[0]; p=idx; ent=[]
    while p<len(d)-16:
        e=d.find(b'\x00',p)
        if e<0 or e-p>250: break
        ent.append([d[p:e].decode('latin1'), e+1, *struct.unpack('<II',d[e+1:e+9])]); p=e+17
    return idx, ent
def dec_ktx(d,off,size):
    raw=zlib.decompress(bytes(d[off:off+size]))
    if raw[:4]!=b'\xabKTX': return None
    fmt,_,w,h=struct.unpack('<IIII',raw[28:44]); kv=struct.unpack('<I',raw[60:64])[0]
    q=64+kv; isz=struct.unpack('<I',raw[q:q+4])[0]; q+=4; data=raw[q:q+isz]
    rgba=texture2ddecoder.decode_etc2a8(data,w,h) if fmt==0x9278 else texture2ddecoder.decode_etc2(data,w,h)
    img=Image.frombytes('RGBA',(w,h),rgba,'raw','BGRA')
    return img if fmt==0x9278 else img.convert('RGB')
def fix(pak):
    d=bytearray(open(pak,'rb').read()); idx,ent=parse(d); bn={e[0]:e for e in ent}
    blob=io.BytesIO(); patches=[]; inplace=0
    for name,fpos,off,size in ent:
        if size!=0 or not (name.endswith('.jpg') or name.endswith('.png')): continue
        k=name+'.ktx'
        img = dec_ktx(d,bn[k][2],bn[k][3]) if k in bn else None
        if img is None: img=Image.new('RGBA',(4,4),(0,0,0,0))
        fmt='PNG' if name.endswith('.png') else 'JPEG'
        ks = bn[k][3] if k in bn else 0
        buf=io.BytesIO(); img.save(buf,fmt,quality=90); blobd=buf.getvalue()
        if fmt=='JPEG' and len(blobd)<=ks and off==bn[k][2]:  # cabe in-place
            d[off:off+len(blobd)]=blobd; struct.pack_into('<I',d,fpos+4,len(blobd)); inplace+=1
        else:
            rel=blob.tell(); blob.write(blobd); patches.append((fpos, idx+rel, len(blobd)))
    bd=blob.getvalue()
    if patches:
        sh=len(bd); out=bytearray(d[:idx])+bytearray(bd)+bytearray(d[idx:])
        out[8:12]=struct.pack('<I',idx+sh); out[12:16]=struct.pack('<I',len(out))
        for fp,no,ns in patches: struct.pack_into('<II',out,fp+sh,no,ns)
        d=out
    open(pak,'wb').write(d)
    print(f'{pak}: in-place={inplace} anexados={len(patches)} size={len(d)}')
for pak in sys.argv[1:]: fix(pak)
