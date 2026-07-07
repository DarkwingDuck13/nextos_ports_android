import zipfile, os, sys
src = "/home/felipe/nextos_ports_android/ports/ninjago/_apk_extract/data.zip"
out = "/mnt/ARQUIVOS/TRABALHO CLAUDE CODE/99-TEMP-CLAUDE/claude-1000/claude-1000/-home-felipe/7a43b998-7dae-4221-a2c6-a2e3bde1febc/scratchpad/ninjago_gamedata"
skip_cutscenes = True
os.makedirs(out, exist_ok=True)
z = zipfile.ZipFile(src)
marker = "/assets/"
n=0; total=0
for info in z.infolist():
    name = info.filename
    i = name.find(marker)
    if i < 0: continue
    rel = name[i+len(marker):]
    if not rel: continue
    if rel.endswith("/"): continue
    if skip_cutscenes and rel.startswith("cutscenes/"): continue
    dst = os.path.join(out, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with z.open(info) as fsrc, open(dst,"wb") as fdst:
        while True:
            b = fsrc.read(1<<20)
            if not b: break
            fdst.write(b)
    n+=1; total+=info.file_size
print(f"flattened {n} files, {total} bytes -> {out}")
