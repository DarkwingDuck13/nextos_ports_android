#!/usr/bin/env python3
# Transpila os shaders GLES3 (platform 9) do Mega Man X p/ GLES2 (platform 5) IN-PLACE no data.unity3d.
# - traduz o GLSL HLSLcc #version 300 es -> #version 100 (in/out->attribute/varying, UBO flatten,
#   layout(location) out -> gl_FragColor, texture()->texture2D())
# - muda o programType interno GLES3(2/3/4) -> GLES2(5)
# - relabela platform 9 -> 5
# Formato do ShaderSubProgramBlob (Unity 2021.2 / version 202012090):
#   uint count; count*(uint offset, 8 bytes extra);  subprogramas concatenados
#   subprograma: uint version; uint programType; ...reflection/keywords...; uint glslLen; glsl; align4
import sys, struct, re
import UnityPy, lz4.block

VM = 202012090
VMB = struct.pack('<I', VM)

def xlate_section(src, stage):
    """traduz UMA secao GLSL (stage='vert'|'frag') de #version 300 es p/ #version 100."""
    s = src
    s = s.replace('#version 300 es', '#version 100')
    # 1) remove o preambulo de macros HLSLcc/Unity (bloco exato que o HLSLcc emite)
    s = re.sub(
        r'#define HLSLCC_ENABLE_UNIFORM_BUFFERS \d+\n'
        r'#if HLSLCC_ENABLE_UNIFORM_BUFFERS\n#define UNITY_UNIFORM\n#else\n#define UNITY_UNIFORM uniform\n#endif\n'
        r'#define UNITY_SUPPORTS_UNIFORM_LOCATION \d+\n'
        r'#if UNITY_SUPPORTS_UNIFORM_LOCATION\n#define UNITY_LOCATION\(x\) layout\(location = x\)\n'
        r'#define UNITY_BINDING\(x\) layout\(binding = x, std140\)\n#else\n'
        r'#define UNITY_LOCATION\(x\)\n#define UNITY_BINDING\(x\) layout\(std140\)\n#endif\n',
        '', s)
    # 2) expande as macros manualmente: UNITY_UNIFORM->'', UNITY_LOCATION(x)/UNITY_BINDING(x)->''
    s = re.sub(r'\bUNITY_LOCATION\s*\([^)]*\)', '', s)
    s = re.sub(r'\bUNITY_BINDING\s*\([^)]*\)', '', s)
    s = re.sub(r'\bUNITY_UNIFORM\b', '', s)
    # 3) flatten de blocos uniform (UBO): "[layout(..)] uniform NAME { <membros> };" -> membros como uniform
    def flat_ubo(m):
        out = []
        for line in m.group(1).split('\n'):
            line = line.strip()
            if not line or line in ('{', '}'): continue
            if line.startswith('}'): continue
            if not line.endswith(';'): line += ';'
            out.append('uniform ' + line)
        return '\n'.join(out) + '\n'
    s = re.sub(r'(?:layout\s*\([^)]*\)\s*)?uniform\s+\w+\s*\{([^}]*)\}\s*;', flat_ubo, s)
    # 4) remove qualquer layout(...) restante
    s = re.sub(r'layout\s*\([^)]*\)\s*', '', s)
    # 4b) GLES2 nao tem 'flat' nem interpolation qualifiers
    s = re.sub(r'^\s*flat\s+', '', s, flags=re.M)
    # 5) in/out por stage
    if stage == 'vert':
        s = re.sub(r'^\s*in\s+', 'attribute ', s, flags=re.M)
        s = re.sub(r'^\s*out\s+', 'varying ', s, flags=re.M)
    else:  # frag
        s = re.sub(r'^\s*in\s+', 'varying ', s, flags=re.M)
        # out [prec] vec4 NAME; -> remove decl; NAME -> gl_FragColor
        for mo in re.finditer(r'^\s*out\s+(?:\w+\s+)?vec4\s+(\w+)\s*;', s, flags=re.M):
            name = mo.group(1)
            s = re.sub(r'\b' + re.escape(name) + r'\b', 'gl_FragColor', s)
        s = re.sub(r'^\s*out\s+(?:\w+\s+)?vec4\s+gl_FragColor\s*;\s*$', '', s, flags=re.M)
    # 6) texture()->texture2D()
    s = re.sub(r'\btextureLod\s*\(', 'texture2DLod(', s)
    s = re.sub(r'\btexture\s*\(', 'texture2D(', s)
    # 7) precisao no fragment se faltar
    if stage == 'frag' and 'precision ' not in s:
        s = s.replace('#version 100\n', '#version 100\nprecision highp float;\n', 1)
    return s

def xlate_glsl(glsl):
    """o programCode tem #ifdef VERTEX ... #endif #ifdef FRAGMENT ... #endif (com #ifdef aninhados
    dentro). Split POSICIONAL nos marcadores VERTEX/FRAGMENT (nao por #endif, que aninha)."""
    txt = glsl.decode('utf8', 'replace')
    vi = txt.find('#ifdef VERTEX'); fi = txt.find('#ifdef FRAGMENT')
    if vi < 0 or fi < 0:
        return xlate_section(txt, 'frag').encode('utf8')
    def do(part, kind, stage):
        body = part[len('#ifdef ' + kind):].lstrip('\n')
        li = body.rfind('#endif')
        if li >= 0: body = body[:li]
        return '#ifdef %s\n%s\n#endif\n' % (kind, xlate_section(body, stage))
    vpart = txt[vi:fi]; fpart = txt[fi:]
    return (do(vpart, 'VERTEX', 'vert') + do(fpart, 'FRAGMENT', 'frag')).encode('utf8')

def transpile_blob(raw):
    # ShaderProgram: uint count; count * (uint offset, 8 bytes extra). Subprogramas nos offsets.
    count = struct.unpack('<I', raw[0:4])[0]
    entries = [bytearray(raw[4 + i*12: 4 + i*12 + 12]) for i in range(count)]
    offs = [struct.unpack('<I', e[0:4])[0] for e in entries]
    # fronteiras dos subprogramas = offsets ORDENADOS (usa a tabela, NAO version magic — evita
    # falsos positivos do 202012090 na reflection/constant-buffer).
    order = sorted(range(count), key=lambda i: offs[i])
    bounds = {}
    for k, i in enumerate(order):
        start = offs[i]
        end = offs[order[k+1]] if k + 1 < count else len(raw)
        bounds[i] = (start, end)
    new_subs = {}
    for i in range(count):
        start, end = bounds[i]
        sub = bytearray(raw[start:end])
        pt = struct.unpack('<i', sub[4:8])[0]
        # programType GLES3(2/3/4)->GLES2(5) SEMPRE (mesmo variantes sem GLSL), senao o device ES2 pula
        if pt in (2, 3, 4): sub[4:8] = struct.pack('<i', 5)
        h = sub.find(b'#')
        if h < 0:
            new_subs[i] = bytes(sub); continue
        glen = struct.unpack('<i', sub[h-4:h])[0]
        glsl = bytes(sub[h:h+glen])
        head = bytes(sub[:h-4])
        newglsl = xlate_glsl(glsl)
        nb = head + struct.pack('<i', len(newglsl)) + newglsl
        while len(nb) % 4 != 0: nb += b'\x00'
        new_subs[i] = nb
    # rebuild: header + subprogramas (em ordem de indice), offsets atualizados
    off = 4 + count*12
    newoff = {}
    body = b''
    for i in range(count):
        newoff[i] = off; body += new_subs[i]; off += len(new_subs[i])
    for i in range(count):
        entries[i][0:4] = struct.pack('<I', newoff[i])
        entries[i][4:8] = struct.pack('<I', len(new_subs[i]))  # 🔑 extra1 = TAMANHO do subprograma (mudou c/ a traducao)
    out = struct.pack('<I', count)
    for e in entries: out += bytes(e)
    return out + body

def main():
    path = sys.argv[1]
    env = UnityPy.load(path)
    ns = 0
    for o in env.objects:
        if o.type.name != 'Shader': continue
        d = o.read_typetree()
        pls = d.get('platforms')
        if not pls or 9 not in pls: continue
        cb = bytes(d['compressedBlob']); offs = d['offsets']; cl = d['compressedLengths']; dl = d['decompressedLengths']
        newcb = b''; newoffs = []; newcl = []; newdl = []
        for pi, plat in enumerate(pls):
            o2 = offs[pi][0] if isinstance(offs[pi], list) else offs[pi]
            c = cl[pi][0] if isinstance(cl[pi], list) else cl[pi]
            dd = dl[pi][0] if isinstance(dl[pi], list) else dl[pi]
            raw = lz4.block.decompress(cb[o2:o2+c], uncompressed_size=dd)
            if plat == 9:
                raw = transpile_blob(raw)
            comp = lz4.block.compress(raw, store_size=False)
            newoffs.append([len(newcb)]); newcl.append([len(comp)]); newdl.append([len(raw)]); newcb += comp
        d['compressedBlob'] = list(newcb)
        d['offsets'] = newoffs; d['compressedLengths'] = newcl; d['decompressedLengths'] = newdl
        d['platforms'] = [5 if p == 9 else p for p in pls]
        o.save_typetree(d); ns += 1
    open(path, 'wb').write(env.file.save(packer='lz4'))
    print(f'transpilados {ns} shaders -> GLES2 (platform 5)')

main()
