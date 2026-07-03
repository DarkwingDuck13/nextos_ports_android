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
import os
import UnityPy, lz4.block

VM = 202012090
VMB = struct.pack('<I', VM)
GLES2_SENTINEL = VMB + struct.pack('<I', 1) + (b'\x00' * 20)

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

def find_glsl(sub):
    hits = []
    for marker in (b'#ifdef', b'#version'):
        start = 0
        while True:
            h = sub.find(marker, start)
            if h < 0:
                break
            hits.append(h)
            start = h + 1
    for h in sorted(set(hits)):
        if h < 4:
            continue
        glen = struct.unpack('<i', sub[h-4:h])[0]
        if 0 < glen <= len(sub) - h:
            return h, glen
    return -1, 0

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
    new_subs = []
    index_map = {}
    for i in range(count):
        start, end = bounds[i]
        sub = bytearray(raw[start:end])
        pt = struct.unpack('<i', sub[4:8])[0]
        # GLES2 real no Unity 2021 guarda apenas os subprogramas combinados
        # (os antigos GLES3 programType=4), precedidos pelo sentinela type=1.
        # Os vertex-only/fragment-only type=2/3 ficam fora do blob GLES2.
        if pt != 4:
            continue
        sub[4:8] = struct.pack('<i', 5)
        h, glen = find_glsl(sub)
        if h < 0:
            continue
        glsl = bytes(sub[h:h+glen])
        head = bytes(sub[:h-4])
        newglsl = xlate_glsl(glsl)
        nb = head + struct.pack('<i', len(newglsl)) + newglsl
        while len(nb) % 4 != 0: nb += b'\x00'
        index_map[i] = len(new_subs) + 1
        new_subs.append(nb)
    if not new_subs:
        return struct.pack('<I', 0), index_map
    # rebuild: header + subprogramas (em ordem de indice), offsets atualizados.
    # GLES2 real (Unity 2021) sempre traz um subprograma sentinela inicial:
    # version=202012090, programType=1, 20 bytes zero. Sem ele o Unity aceita a
    # platform 5 mas descarta os variants e cai no Hidden/InternalErrorShader.
    out_count = len(new_subs) + 1
    off = 4 + out_count*12
    newoff = {0: off}
    body = GLES2_SENTINEL
    off += len(GLES2_SENTINEL)
    newlens = {0: len(GLES2_SENTINEL)}
    for i, sub in enumerate(new_subs):
        idx = i + 1
        newoff[idx] = off; newlens[idx] = len(sub)
        body += sub; off += len(sub)
    out = struct.pack('<I', out_count)
    out += struct.pack('<III', newoff[0], newlens[0], 0)
    for i, sub in enumerate(new_subs):
        idx = i + 1
        out += struct.pack('<III', newoff[idx], newlens[idx], 0)
    return out + body, index_map

def patch_parsed_form_platforms(d, index_map):
    def patch_player_subprograms(prog):
        if not isinstance(prog, dict):
            return
        for variants in prog.get('m_PlayerSubPrograms') or []:
            if not isinstance(variants, list):
                continue
            dupes = []
            for sp in variants:
                if not isinstance(sp, dict):
                    continue
                if sp.get('m_GpuProgramType') == 4 and sp.get('m_BlobIndex') in index_map:
                    dup = dict(sp)
                    dup['m_BlobIndex'] = index_map[sp['m_BlobIndex']]
                    dup['m_GpuProgramType'] = 5
                    dupes.append(dup)
            if dupes:
                variants[:0] = dupes

    pf = d.get('m_ParsedForm')
    if not isinstance(pf, dict):
        return
    for ss in pf.get('m_SubShaders') or []:
        for p in ss.get('m_Passes') or []:
            if isinstance(p.get('m_Platforms'), list):
                new_platforms = []
                for x in p['m_Platforms']:
                    if x == 9 and 5 not in p['m_Platforms'] and 5 not in new_platforms:
                        new_platforms.append(5)
                    new_platforms.append(x)
                p['m_Platforms'] = new_platforms
            for key in ('progVertex', 'progFragment', 'progGeometry',
                        'progHull', 'progDomain', 'progRayTracing'):
                patch_player_subprograms(p.get(key))

def main():
    path = sys.argv[1]
    skip_names = {s for s in os.environ.get('MMX_SKIP_SHADERS', '').split(',') if s}
    only_names = {s for s in os.environ.get('MMX_ONLY_SHADERS', '').split(',') if s}
    env = UnityPy.load(path)
    ns = 0
    for o in env.objects:
        if o.type.name != 'Shader': continue
        d = o.read_typetree()
        name = d.get('m_Name') or d.get('m_ParsedForm', {}).get('m_Name') or ''
        if only_names and name not in only_names:
            continue
        if name in skip_names:
            continue
        pls = d.get('platforms')
        if not pls or 9 not in pls: continue
        if 5 in pls and not os.environ.get('MMX_REGEN_GLES2'):
            continue
        cb = bytes(d['compressedBlob']); offs = d['offsets']; cl = d['compressedLengths']; dl = d['decompressedLengths']
        newcb = b''; newoffs = []; newcl = []; newdl = []; newpls = []
        blob_index_map = {}
        old_stage = d.get('stageCounts')
        new_stage = [] if isinstance(old_stage, list) else None
        has_platform5 = 5 in pls
        for pi, plat in enumerate(pls):
            o2 = offs[pi][0] if isinstance(offs[pi], list) else offs[pi]
            c = cl[pi][0] if isinstance(cl[pi], list) else cl[pi]
            dd = dl[pi][0] if isinstance(dl[pi], list) else dl[pi]
            raw = lz4.block.decompress(cb[o2:o2+c], uncompressed_size=dd)
            if plat == 9 and not has_platform5:
                raw5, blob_index_map = transpile_blob(raw)
                comp = lz4.block.compress(raw5, store_size=False)
                newoffs.append([len(newcb)]); newcl.append([len(comp)]); newdl.append([len(raw5)]); newcb += comp
                newpls.append(5)
                if new_stage is not None:
                    new_stage.append(old_stage[pi])
            comp = lz4.block.compress(raw, store_size=False)
            newoffs.append([len(newcb)]); newcl.append([len(comp)]); newdl.append([len(raw)]); newcb += comp
            newpls.append(plat)
            if new_stage is not None:
                new_stage.append(old_stage[pi])
        d['compressedBlob'] = list(newcb)
        d['offsets'] = newoffs; d['compressedLengths'] = newcl; d['decompressedLengths'] = newdl
        d['platforms'] = newpls
        if new_stage is not None:
            d['stageCounts'] = new_stage
        patch_parsed_form_platforms(d, blob_index_map)
        o.save_typetree(d); ns += 1
    open(path, 'wb').write(env.file.save(packer='lz4'))
    print(f'transpilados {ns} shaders -> GLES2 (platform 5)')

main()
