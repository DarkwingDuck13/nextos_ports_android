#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Streets of Rage 4 -- extrator BYO-DATA: tira do APK (sua copia legal) tudo que o
# port precisa, igual ao modelo Bully/Dysmantle (so que aqui o SOR4.dll vem do
# assembly-store XABA+LZ4 do .NET-Android, nao e unzip simples).
#
# Saida (em GAMEDIR):
#   gameassets/<assets inteiro>   <- TUDO de assets/ do APK (bigfile, .xnb, .wem,
#                                    .bnk, fontes, videos, shader, sem-extensao...).
#                                    O AssetManager do jogo le a raiz = gameassets.
#   host_pkg/libs/libWwise.real.so
#   host_pkg/<fontes .ttf/.otf>   <- copia das fontes tb p/ o host (SharpFont)
#   SOR4.dll                      <- extraido do assembly-store (XALZ/LZ4 puro-python)
#
# uso: sor4_apkextract.py <apk> <gamedir>
import sys, os, struct, zipfile, shutil

def log(msg): print(msg, flush=True)
def pct(p):
    n = p // 5
    print("    [%s] %d%%" % ("#"*n + "."*(20-n), p), flush=True)

# ---------- LZ4 block decompress (puro python; sem modulo) ----------
def lz4_block_decompress(src, usize):
    out = bytearray(usize); op = 0; ip = 0; n = len(src)
    while ip < n:
        token = src[ip]; ip += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[ip]; ip += 1; lit += b
                if b != 255: break
        out[op:op+lit] = src[ip:ip+lit]; op += lit; ip += lit
        if ip >= n: break
        off = src[ip] | (src[ip+1] << 8); ip += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[ip]; ip += 1; ml += b
                if b != 255: break
        ml += 4; mp = op - off
        if off >= ml:
            out[op:op+ml] = out[mp:mp+ml]; op += ml
        else:
            for _ in range(ml):
                out[op] = out[mp]; op += 1; mp += 1
    return bytes(out[:op])

# ---------- acha e descomprime o SOR4.dll no blob (XALZ com usize alvo) ----------
SOR4_USIZE = 1491968  # SOR4.dll v1.4.5 (descomprimido)
def extract_sor4_dll(blob, out_path):
    pos = 0; cands = []
    while True:
        i = blob.find(b"XALZ", pos)
        if i < 0: break
        idx, usize = struct.unpack_from("<II", blob, i + 4)
        cands.append((i, usize)); pos = i + 4
    target = [c for c in cands if c[1] == SOR4_USIZE]
    order = target if target else sorted(cands, key=lambda c: -c[1])
    for (off, usize) in order:
        ds = off + 12
        nxt = blob.find(b"XALZ", ds)
        comp = blob[ds: nxt if nxt > 0 else len(blob)]
        try:
            dec = lz4_block_decompress(comp, usize)
        except Exception:
            continue
        if dec[:2] == b"MZ" and b"SOR4" in dec:
            with open(out_path, "wb") as f: f.write(dec)
            return True
    return False

def main():
    if len(sys.argv) < 3:
        log("uso: sor4_apkextract.py <apk> <gamedir>"); return 2
    apk = sys.argv[1]; gd = sys.argv[2]
    if not os.path.isfile(apk):
        log("APK nao encontrado: " + apk); return 1
    libs = os.path.join(gd, "host_pkg", "libs")
    ga   = os.path.join(gd, "gameassets")
    hp   = os.path.join(gd, "host_pkg")
    for d in (libs, ga, hp): os.makedirs(d, exist_ok=True)

    # --libs-only: extrai SO libWwise + SOR4.dll + fontes (o texconv --apk faz os assets).
    libs_only = "--libs-only" in sys.argv

    z = zipfile.ZipFile(apk)
    names = z.namelist()
    assets = [n for n in names if n.startswith("assets/") and not n.endswith("/")]
    fonts  = [n for n in assets if n.lower().endswith((".ttf", ".otf"))]

    # 1) libWwise real
    log(">> Biblioteca de audio (libWwise)..."); pct(1)
    try:
        with z.open("lib/arm64-v8a/libWwise.so") as s, open(os.path.join(libs, "libWwise.real.so"), "wb") as o:
            shutil.copyfileobj(s, o)
    except KeyError:
        log("  (libWwise.so nao achada no APK -- audio pode falhar)")

    # 2) SOR4.dll do assembly-store
    log(">> Codigo do jogo (SOR4.dll do assembly-store)..."); pct(3)
    with z.open("lib/arm64-v8a/libassemblies.arm64-v8a.blob.so") as s:
        blob = s.read()
    if not extract_sor4_dll(blob, os.path.join(gd, "SOR4.dll")):
        log("ERRO: nao consegui extrair o SOR4.dll do assembly-store."); return 3
    log("  SOR4.dll OK")
    del blob

    # 3) assets/ INTEIRO -> gameassets/ (bigfile, .xnb, .wem, .bnk, fontes, videos, ...)
    if not libs_only:
        total = len(assets); done = 0
        log(">> Dados do jogo (%d arquivos: texturas, audio, bigfile, videos...)..." % total)
        for n in assets:
            rel = n[len("assets/"):]
            dst = os.path.join(ga, rel)
            d = os.path.dirname(dst)
            if d: os.makedirs(d, exist_ok=True)
            with z.open(n) as s, open(dst, "wb") as o:
                shutil.copyfileobj(s, o)
            done += 1
            p = (done * 100) // max(total, 1)
            # formato: "N/TOTAL  PCT%  caminho" (1 linha por arquivo, SEM barra)
            print("%d/%d  %d%%  %s" % (done, total, p, rel), flush=True)

    # fontes tambem no host_pkg (o host/SharpFont procura ali)
    for n in fonts:
        with z.open(n) as s, open(os.path.join(hp, os.path.basename(n)), "wb") as o:
            shutil.copyfileobj(s, o)

    z.close()
    pct(96)
    log(">> Extracao do APK concluida.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
