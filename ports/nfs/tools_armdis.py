import sys,struct,subprocess
from capstone import *
L=sys.argv[1] if len(sys.argv)>1 else '/home/root/Downloads/nfs-analysis/lib/armeabi-v7a/libapp.so'
start=int(sys.argv[2],16); end=int(sys.argv[3],16)
data=open(L,'rb').read()
RE='/home/root/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-ng.aarch64-4/toolchain/bin/armv8a-emuelec-linux-gnueabihf-readelf'
rd=subprocess.run([RE,'-S',L],capture_output=True,text=True).stdout
ta=to=ts=None; rodata=None
secs=[]
for ln in rd.splitlines():
    if 'PROGBITS' in ln:
        p=ln.split(); i=p.index('PROGBITS')
        try: secs.append((p[p.index('PROGBITS')-1], int(p[i+1],16), int(p[i+2],16), int(p[i+3],16)))
        except: pass
def f2v(off): 
    for nm,a,o,s in secs:
        if o<=off<o+s: return a+(off-o)
    return None
def v2f(va):
    for nm,a,o,s in secs:
        if a<=va<a+s: return o+(va-a)
    return None
def rdstr(va):
    o=v2f(va)
    if o is None: return None
    e=data.find(b'\x00',o)
    try: return data[o:e].decode('latin1')
    except: return None
md=Cs(CS_ARCH_ARM,CS_MODE_ARM); md.detail=True
fo=v2f(start)
code=data[fo:fo+(end-start)]
reg_str={}  # reg -> resolved string addr (after ldr[pc]+add pc)
ldr_pend={} # reg -> loaded literal (offset)
for ins in md.disasm(code,start):
    line=f"{ins.address:08x}: {ins.mnemonic:6} {ins.op_str}"
    ann=""
    # ldr rX,[pc,#imm]
    if ins.mnemonic=='ldr' and 'pc' in ins.op_str and '#' in ins.op_str:
        try:
            rt=ins.op_str.split(',')[0].strip()
            # literal addr
            imm=int(ins.op_str.split('#')[1].rstrip(']'),0)
            pool=(ins.address&~3)+8+imm
            pf=v2f(pool)
            if pf is not None:
                Lv=struct.unpack_from('<I',data,pf)[0]
                ldr_pend[rt]=Lv
        except Exception as e: pass
    if ins.mnemonic=='add' and ', pc' in (','+ins.op_str.replace(' ','')):
        pass
    # add rd, pc, rm  -> resolve string
    if ins.mnemonic=='add':
        parts=[x.strip() for x in ins.op_str.split(',')]
        if len(parts)==3 and parts[1]=='pc' and parts[2] in ldr_pend:
            tgt=(ins.address+8+ldr_pend[parts[2]])&0xFFFFFFFF
            s=rdstr(tgt)
            if s is not None: ann=f"   ; \"{s[:48]}\" @{tgt:#x}"
            reg_str[parts[0]]=tgt
    print(line+ann)
