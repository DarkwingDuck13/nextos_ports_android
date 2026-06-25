#!/usr/bin/env python3
# Converte XNB Texture2D (SurfaceFormat.Color RGBA) -> XNB Rgba8Etc2 (format 94), descomprimido.
# So GLES3/ETC2 (R36S). Mantem o manifest de readers; troca so o payload da textura.
import sys, os, struct, subprocess, tempfile

TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "etcconv")

def read_7bit(b, i):
    res = 0; shift = 0
    while True:
        v = b[i]; i += 1
        res |= (v & 0x7F) << shift
        if not (v & 0x80): break
        shift += 7
    return res, i

def write_7bit(n):
    out = bytearray()
    while True:
        v = n & 0x7F; n >>= 7
        if n: out.append(v | 0x80)
        else: out.append(v); break
    return bytes(out)

def lz4_decompress(data, outsize):
    import lz4.block
    return lz4.block.decompress(data, uncompressed_size=outsize)

def load_content(path):
    raw = open(path, "rb").read()
    assert raw[0:3] == b"XNB", "nao XNB"
    platform = raw[3]; version = raw[4]; flags = raw[5]
    filesize = struct.unpack_from("<I", raw, 6)[0]
    if flags & 0x40:   # LZ4
        decomp = struct.unpack_from("<I", raw, 10)[0]
        content = lz4_decompress(raw[14:filesize], decomp)
    elif flags & 0x80: # LZX
        raise RuntimeError("LZX nao suportado")
    else:
        content = raw[10:filesize]
    return platform, version, flags, content

def parse_texture(content):
    i = 0
    cnt, i = read_7bit(content, i)
    readers = []
    for _ in range(cnt):
        slen, i = read_7bit(content, i)
        name = content[i:i+slen].decode("utf-8"); i += slen
        ver = struct.unpack_from("<i", content, i)[0]; i += 4
        readers.append(name)
    manifest_end = i
    shared, i = read_7bit(content, i)
    obj_start = i
    tri, i = read_7bit(content, i)        # type reader index do objeto primario
    fmt = struct.unpack_from("<i", content, i)[0]; i += 4
    w = struct.unpack_from("<i", content, i)[0]; i += 4
    h = struct.unpack_from("<i", content, i)[0]; i += 4
    levels = struct.unpack_from("<i", content, i)[0]; i += 4
    sz = struct.unpack_from("<i", content, i)[0]; i += 4
    data = content[i:i+sz]; i += sz
    # pula mips restantes (so usaremos level0)
    for _ in range(levels - 1):
        s2 = struct.unpack_from("<i", content, i)[0]; i += 4 + s2
    is_tex = any("Texture2DReader" in r for r in readers)
    # so converte se: e Texture2D, sem shared resources, e consumimos TODO o content
    # (garante objeto unico simples -> rebuild seguro)
    safe = is_tex and shared == 0 and i == len(content)
    # header_blob = tudo ate (e incluindo) o type reader index do objeto primario
    header_blob = content[:obj_start] + write_7bit(tri)
    return safe, fmt, w, h, levels, data, header_blob

def convert(path, outpath, effort=40):
    platform, version, flags, content = load_content(path)
    safe, fmt, w, h, levels, data, header_blob = parse_texture(content)
    if not safe or fmt != 0:
        return None  # so Texture2D Color, objeto unico simples
    # RGBA cru level0 -> etc2
    with tempfile.NamedTemporaryFile(suffix=".rgba", delete=False) as tf:
        tf.write(data[:w*h*4]); rawp = tf.name
    etcp = rawp + ".etc2"
    r = subprocess.run([TOOL, rawp, str(w), str(h), etcp, str(effort)], capture_output=True)
    if r.returncode != 0:
        os.unlink(rawp); raise RuntimeError("etcconv: " + r.stderr.decode())
    etc = open(etcp, "rb").read()
    os.unlink(rawp); os.unlink(etcp)
    # novo content: header_blob + format(94) + w + h + levels(1) + size + etc
    new = bytearray(header_blob)
    new += struct.pack("<iiii", 94, w, h, 1)
    new += struct.pack("<i", len(etc))
    new += etc
    # XNB descomprimido (limpa bits de compressao)
    nf = flags & ~0x40 & ~0x80
    fsize = 10 + len(new)
    out = bytearray(b"XNB"); out.append(platform); out.append(version); out.append(nf)
    out += struct.pack("<I", fsize); out += new
    open(outpath, "wb").write(out)
    return (w, h, len(data), len(etc))

def batch(srcdir, dstdir, effort=40):
    import shutil
    conv = skip = err = 0
    saved_rgba = saved_etc = 0
    for root, _, files in os.walk(srcdir):
        for fn in files:
            sp = os.path.join(root, fn)
            rel = os.path.relpath(sp, srcdir)
            dp = os.path.join(dstdir, rel)
            os.makedirs(os.path.dirname(dp), exist_ok=True)
            if not fn.lower().endswith(".xnb"):
                shutil.copy2(sp, dp); continue
            try:
                r = convert(sp, dp, effort)
            except Exception as e:
                r = None; print("ERR", rel, str(e)[:80]); err += 1
            if r:
                conv += 1; saved_rgba += r[2]; saved_etc += r[3]
                if conv % 25 == 0:
                    print(f"  {conv} convertidos... (GPU {saved_rgba//1048576}->{saved_etc//1048576} MB)")
            else:
                shutil.copy2(sp, dp); skip += 1   # nao-Texture2D-Color: copia original
    print(f"FEITO: {conv} ETC2, {skip} copiados, {err} erros")
    print(f"GPU textura: {saved_rgba//1048576} MB RGBA -> {saved_etc//1048576} MB ETC2 (-{100-saved_etc*100//max(saved_rgba,1)}%)")

if __name__ == "__main__":
    if sys.argv[1] == "--batch":
        batch(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 40)
    else:
        res = convert(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 40)
        print(res)
