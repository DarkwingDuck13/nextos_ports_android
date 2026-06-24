# DYSMANTLE v5.1 — HANDOFF (2026-06-18) — LEAN PAK (corta disco) + muro do "ETC1 puro"

## ✅ ENTREGUE E VALIDADO NO DEVICE (.164)
- **`DYSMANTLE v5.zip`** (3.7MB, diretório de entregas) — BYO-DATA, padrão PortMaster.
- Device .164 deixado PRONTO p/ abrir pela ES (install já convertido, markers
  `.setup_done/.textures_fixed/.etc1_cached` setados → NÃO re-bakeia; ES rodando).
- Validação E2E no device: o **próprio `texbake.aarch64` do device** rodou `--leanpak`
  (output byte-idêntico ao host: data.pak 465801651, gfx 76910815) → boot → **MENU
  renderiza perfeito** (logo, personagem, PLAY/SAVE/OPTIONS/CREDITS, thumbnails, store de
  DLC UNDERWORLD) — zero blob, sem crash, cache 15436 carregado.

## 🧱 "ETC1 PURO SEM CACHE" (igual SOR4) = MURO REAL DO MOTOR FECHADO (confirmado)
Tentei o caminho nativo (engine carregar `.ktx` ETC1 direto, sem cache). **Inviável:**
- A engine seleciona o **loader de textura pelo formato do MANIFEST** (manifest-data.xml),
  **NÃO pela extensão do arquivo**. Renomear `.jpg`→`.ktx` (KTX_REDIRECT) faz ela tratar o
  KTX como JPEG → `LoadBitmapInternal` retorna **0** p/ toda textura → crash downstream
  (sig=11, mesmos endereços game@0x4957dc/0x4650e0/0x465060 das sessões anteriores).
- Descobri a 1ª pista certa: o `.ktx` ETC1 que a gente gerava tinha **mipLevels=1** vs **9**
  do original (offset 56 do header KTX). Gerei o transcode ETC2-RGB→ETC1 **preservando a
  cadeia de mips inteira** (`texbake --etc1mip`, mantido no código p/ histórico) — KTX
  byte-estruturalmente idêntico ao original. **MESMO ASSIM crasha** → não era mip, é o
  loader fixado no manifest. Muro fundamental; só cairia com RE profundo do motor (incerto).
- Conclusão: como no SOR4 o ETC1 PRECISA morar onde o hook leia (o "cache" = store ETC1,
  equivalente ao texcache que o próprio SOR4 usa). Não dá pra "não ter cache".

## 🗜️ O QUE DEU PRA MELHORAR: LEAN PAK (cortar a DUPLICAÇÃO de disco)
Descoberta de disco: o pak tem ~15 mil texturas; só ~643 vinham vazias (com `.ktx`), o
resto é JPEG/PNG real. O cache ETC1 (602MB) **duplica** essas texturas. O custo real não era
"JPEG 600 + cache 600"; era cache(602MB, irredutível) + ~120MB de JPEG duplicado.
- **`texbake --leanpak <cache>`** (novo, em `src/texbake.c`): REPACK do pak — texturas
  cobertas pelo cache viram **placeholder sólido full-dim** (~1-3KB; o jogo lê os pixels
  REAIS do cache no hook, só as DIMS importam e batem), as de alpha vêm reais (preenche o
  slot vazio do `.ktx` se preciso), e os `.ktx` ETC2 (mortos no runtime) são **DROPADOS**.
  - data.pak 543→466MB, gfx 144→77MB → **~145MB a menos**; sem `.ktx` mortos.
  - **ATÔMICO**: só sobrescreve o pak se a escrita inteira deu certo (ferror/fflush);
    senão preserva o original e o progressor cai pro `fixpak` (modo seguro). Guard de disco
    (`FREE_KB>550000`) antes de tentar.
- **Runtime IDÊNTICO ao v5 validado**: o hook lê o cache por nome com guarda de dimensão; o
  placeholder (full-dim) casa as dims → sobe a ETC1 do cache. Igual a antes, só sem o JPEG
  real duplicado.

## 🔧 MUDANÇAS DESTA SESSÃO
- `src/texbake.c`: `--leanpak <cache>` (repack atômico c/ placeholders), `--etc1mip`
  (transcode nativo c/ mips — histórico/futuro), encoder PNG + crc32 + jpeg_encode_rgba.
- `src/imports.c` `my_glCompressedTexImage2D`: bloco **ETC1NAT** (sobe 0x9274/75 como
  0x8D64 nativo; só é exercitado no caminho nativo, no-op no caminho cache). Resto intacto.
- `tools/dysmantle_extract.src` (progressor): extract → **apaga APK cedo** (libera disco) →
  cache (`--sidetable`) → **leanpak** (fallback `fixpak` se disco apertar). Markers no fim.
- `DYSMANTLE.sh`: com cache presente força **`DYSMANTLE_TEXSCALE=1.0`** (TEXSCALE reduz as
  dims ANTES do lookup → cache-miss → encode em runtime/stutter; ETC1 já é 4bpp).
- `build_tools_aarch64.sh`: texbake aarch64 agora linka `jpeg_enc.c` (leanpak precisa).
- `CHANGELOG.md`: seção v5.1.

## 📦 COMO BUILDAR / RODAR
- Binário (GLIBC 2.27): `SR=~/NextOS-Elite-Edition/build.*Amlogic-old*/.../sysroot;
  docker run --rm -v "$PWD":/repo -v "$SR":/sysroot:ro debian:bullseye bash /repo/build_compat_gcc.sh`
- Tools aarch64: `docker run --rm -v "$PWD":/repo debian:bullseye bash /repo/build_tools_aarch64.sh`
- texbake host (teste): `gcc -O2 -I src -o /tmp/texbake src/texbake.c src/etc2_decode.c
  src/etc1_encode.c src/jpeg_enc.c -ldl -lm -lpthread`
- Fluxo offline (host): `texbake d.pak g.pak --scale 1.0 --sidetable etc1.cache` então
  `texbake d.pak g.pak --leanpak etc1.cache`.
- Device .164 (root/**emuelec**): install `/storage/roms/ports/dysmantle`, launcher
  `/storage/roms/ports_scripts/DYSMANTLE.sh`. Run cache-mode (env): `DYSMANTLE_ETC1CACHE=
  .../etc1.cache DYSMANTLE_ASSETS=assets DYSMANTLE_GLVER=2.0 DYSMANTLE_TEXSCALE=1.0
  LD_LIBRARY_PATH=/usr/lib:$PWD ./dysmantle`. Screenshot: `touch /dev/shm/dys_shot` →
  /dev/shm/dys_shot.raw (RGBA 1280x720, flip-V).

## 🚨 REGRA: matar+confirmar 0 instâncias por `/proc/*/exe` antes de lançar (comm=Main).

## FALTA
- Abrir pela ES no device (já pronto) e confirmar gameplay de OUVIDO/OLHO na TV.
- (Opcional) commitar no master quando solicitado (commits limpos, zero co-autor).
