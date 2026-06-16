#!/usr/bin/env python3
# wwise_extract.py - parser OFFLINE dos soundbanks Wwise do SoR4.
# Mapeia event_name -> wem(s) (via HIRC) e extrai cada wem embarcado (DIDX) como .opus
# (o chunk "data" do wem 0x3040 JA E um Ogg Opus padrao -> toca direto na libopus/OpenAL).
# Saida: <outdir>/<wem_id>.opus  +  manifest.txt (linhas "event_name <tab> wem_id[,wem_id...]")
import struct, sys, os, glob

def fnv1_32(name):
    h = 2166136261
    for c in name.lower().encode("ascii"):
        h = (h * 16777619) & 0xFFFFFFFF
        h ^= c
    return h

def parse_bank(path):
    d = open(path, "rb").read()
    version = None; didx = {}; data_off = None; hirc_objs = {}
    i = 0
    while i < len(d) - 8:
        tag = d[i:i+4]; sz = struct.unpack("<I", d[i+4:i+8])[0]; body = d[i+8:i+8+sz]
        if tag == b"BKHD":
            version = struct.unpack("<I", body[0:4])[0]
        elif tag == b"DIDX":
            for k in range(sz // 12):
                wid, off, wsz = struct.unpack("<III", body[k*12:k*12+12])
                didx[wid] = (off, wsz)
        elif tag == b"DATA":
            data_off = i + 8
        elif tag == b"HIRC":
            num = struct.unpack("<I", body[0:4])[0]; p = 4
            for _ in range(num):
                otype = body[p]; osize = struct.unpack("<I", body[p+1:p+5])[0]
                ob = body[p+5:p+5+osize]
                oid = struct.unpack("<I", ob[0:4])[0]
                hirc_objs[oid] = (otype, ob)
                p += 5 + osize
        i += 8 + sz
    return d, version, didx, data_off, hirc_objs

def parse_event_actions(ob):
    # Event(type 0x04): id(4), count, count*actionId(4). count = u8 (v>=113) ou varint.
    oid = struct.unpack("<I", ob[0:4])[0]
    cnt = ob[4]; acts = []
    p = 5
    for _ in range(cnt):
        if p+4 > len(ob): break
        acts.append(struct.unpack("<I", ob[p:p+4])[0]); p += 4
    return acts

def parse_action_target(ob):
    # Action(type 0x03): id(4), u16 actionType, u32 idExt(target), ...
    if len(ob) < 10: return None
    atype = struct.unpack("<H", ob[4:6])[0]
    target = struct.unpack("<I", ob[6:10])[0]
    return atype, target

def parse_sound_source(ob):
    # Sound(type 0x02): id(4), u32 pluginID, u8 streamType, u32 sourceID, u32 fileID, ...
    if len(ob) < 14: return None
    pluginID = struct.unpack("<I", ob[4:8])[0]
    streamType = ob[8]
    sourceID = struct.unpack("<I", ob[9:13])[0]
    return streamType, sourceID

def collect_wems_for_object(oid, hirc, seen=None, depth=0):
    # desce recursivamente: Action->target, Sound->source, Containers->children(best effort)
    if seen is None: seen = set()
    if oid in seen or depth > 12: return []
    seen.add(oid)
    if oid not in hirc: return []
    otype, ob = hirc[oid]
    res = []
    if otype == 0x02:  # Sound
        s = parse_sound_source(ob)
        if s: res.append((s[0], s[1]))  # (streamType, sourceID)
    elif otype == 0x03:  # Action
        t = parse_action_target(ob)
        if t: res += collect_wems_for_object(t[1], hirc, seen, depth+1)
    # Containers (RanSeq 0x05, Switch 0x06, ActorMixer 0x07, MusicSeg 0x0A, MusicTrack 0x0B, MusicSwitch 0x0C, MusicRanSeq 0x0D)
    # best-effort: varrer ids referenciados no corpo (procura ids conhecidos do HIRC)
    elif otype in (0x05,0x06,0x07,0x0A,0x0B,0x0C,0x0D,0x09,0x0E):
        if otype == 0x0B:  # MusicTrack: tem fontes diretas
            # tenta achar source ids varrendo (streamType+sourceID padroes)
            pass
        # varre o corpo por u32 que sejam ids de filhos no HIRC
        for off in range(4, len(ob)-4):
            cid = struct.unpack("<I", ob[off:off+4])[0]
            if cid in hirc and cid != oid and cid not in seen:
                child = collect_wems_for_object(cid, hirc, seen, depth+1)
                res += child
    return res

def extract_wem(src, WEM_SRC, outdir, extracted):
    if src not in WEM_SRC or src in extracted: return src in extracted
    d, pos, wsz = WEM_SRC[src]
    wem = d[pos:pos+wsz]
    j = 12; data=None
    while j < len(wem)-8:
        t = wem[j:j+4]; s = struct.unpack("<I", wem[j+4:j+8])[0]
        if t == b"data": data = wem[j+8:j+8+s]; break
        j += 8+s+(s&1)
    if data and data[:4]==b"OggS":
        open(os.path.join(outdir,"%d.opus"%src),"wb").write(data); extracted.add(src); return True
    return False

def main():
    banks = sys.argv[1].split(",")
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    ALL_HIRC = {}; WEM_SRC = {}
    for bp in banks:
        d, ver, didx, data_off, hirc = parse_bank(bp)
        sys.stderr.write("bank %s ver=%s didx=%d hirc=%d\n" % (os.path.basename(bp), ver, len(didx), len(hirc)))
        ALL_HIRC.update(hirc)
        if data_off is not None:
            for wid,(off,wsz) in didx.items():
                WEM_SRC[wid] = (d, data_off+off, wsz)
    # ITERA TODOS os Events (type 0x04). manifest = event_id -> [wem_ids embarcados]
    extracted = set(); manifest = []; nstream=0
    for eid,(otype,ob) in ALL_HIRC.items():
        if otype != 0x04: continue
        acts = parse_event_actions(ob)
        wems = []
        for a in acts:
            wems += collect_wems_for_object(a, ALL_HIRC)
        emb = []; seen=set()
        for streamType, src in wems:
            if src in seen: continue
            seen.add(src)
            if src in WEM_SRC:
                if extract_wem(src, WEM_SRC, outdir, extracted): emb.append(src)
            else:
                nstream+=1
        if emb:
            manifest.append((eid, emb))
    with open(os.path.join(outdir,"manifest.txt"),"w") as f:
        for eid, ids in manifest:
            f.write("%d\t%s\n"%(eid, ",".join(str(x) for x in ids)))
    sys.stderr.write("eventos c/ SFX embarcado=%d  wems extraidos=%d  refs streamed=%d -> %s\n"%(len(manifest),len(extracted),nstream,outdir))

if __name__=="__main__": main()
