# DYSMANTLE v5 — HANDOFF (2026-06-18) — conversão de textura OFFLINE

## 🎯 PRÓXIMA SESSÃO — FAÇA ISSO (em ordem)

**A abordagem do CACHE JÁ FUNCIONA e foi validada no gameplay (confirmado).** O
caminho mais rápido pra ENTREGAR a v5 é fechá-la com o cache. NÃO recomece do zero.

1. **LER este handoff inteiro + a REGRA** (matar+confirmar 0 instâncias antes de lançar
   o jogo — regra importante, já foi violada).
2. **Perguntar ao porter:** quer fechar a v5 com **cache** (funciona, mas pak+cache=1.2GB
   no disco) ou insiste no **"só ETC1 nativo"** (sem cache/JPEG, limpo como SOR4, MAS
   depende de rachar um crash do motor fechado = RE arriscado, talvez impossível no ES2)?
3. **Se for CACHE (recomendado):**
   a. Device .164 (root/**emuelec**): matar+confirmar 0, limpar `/storage/roms/ports/dysmantle`,
      deployar `~/dysmantle-v5/dysmantle/` (rsync `-rL --no-perms --no-owner --no-group`,
      SEM `-a` senão dá erro de chown) + o APK.
   b. Rodar o fluxo do progressor (`tools/dysmantle_extract.src` faz extract→fixpak→texbake)
      DETACHED (nohup) + polling por flag (~17min; SSH cai em comando longo).
   c. Abrir o jogo (env `DYSMANTLE_ETC1CACHE`), screenshot do menu E do gameplay → confirmar
      imagem completa (sem rosa/branco/preto) + sem stutter.
   d. Zipar: `cd ~/dysmantle-v5 && zip -r "DYSMANTLE v5.zip" DYSMANTLE.sh dysmantle/` → pôr
      no diretório de entregas. FIM.
4. **Se for "só ETC1 nativo" (arriscado):** ver seção "MURO" abaixo — investigar o crash do
   `KtxImageLoader` (`game@0x4957dc...` no `libNativeGame.so` via Ghidra `~/re-tools`).
   **Time-box: ~1h.** Se não rachar, voltar pro cache.

⚠️ **Tudo abaixo é o histórico/detalhe.** O essencial está aqui em cima.

---



> Sessão longa. Objetivo do porter: **zip v5** que na 1ª execução **converte TODAS as
> texturas offline** (fixpak + ETC1), aplica os fixes, e abre o jogo **LIMPO, sem
> conversão durante o gameplay** (igual SOR4). Binário glibc velha (2.27, roda em 2.30+).

---

## ✅ O QUE JÁ FUNCIONA (abordagem CACHE — VALIDADA ponta-a-ponta, gameplay confirmado)

**Resultado: imagem COMPLETA (menu + gameplay: chão/personagem/árvores/mato com cor), ZERO
stutter, ETC1 na VRAM. Confirmado "tá tudo normal agora" no gameplay.**

### Como a solução do cache funciona (estilo SOR4, controlando NA NOSSA camada)
O motor (10tons NX) é **caixa-preta fechada**: só aceita carregar **.jpg (ES2)** ou
**.ktx ETC2 (ES3)**. Não dá pra ele carregar ETC1 nativo (ver "MURO" abaixo). Então:
1. **Offline (progressor, 1x):** `texbake --sidetable etc1.cache` gera um arquivo
   `etc1.cache` = mapa **nome→ETC1** (só texturas OPACAS). Formato:
   `"ETC1CACH"(8) + u32 count + u32 data_off + índice[nome\0,u16 w,u16 h,u32 blob_off,u32 size] ORDENADO + blobs ETC1`.
2. **Runtime:** o motor carrega o **.jpg normal** (imagem completa). No `glTexImage2D`
   (`imports.c` `try_upload_etc1`), o hook pega o **nome** da textura
   (`bk_last_bmp_name()` em main.c), faz **bsearch no cache**, e se achar (E as dims
   batem = guarda de tamanho) sobe a **ETC1 pronta** com `glCompressedTexImage2D(0x8D64)`
   → **ZERO encode/decode em runtime**. Senão, fallback = encode em runtime.

### Fixes-chave descobertos (NÃO repetir os becos)
- 🌑 **normals/specular/heights NÃO podem virar ETC1** (ficam RGBA8): ETC1 é lossy e
  correlaciona RGB → destrói os mapas de luz → **chão/personagem/árvores PRETOS**. Fix em
  `try_upload_etc1`: `if (strstr(nm,"normal"|"specular"|"height"|...)) return 0;`.
- 🛡️ **Guarda de tamanho** no lookup (`e->w==w && e->h==h`): sem ela, nome velho de
  FBO/mip sobe ETC1 errada → branco/preto/lixo.
- 🔑 **FORMATO DO PAK (crucial p/ qualquer mexida no pak):** o índice começa com
  **`u32 COUNT`** (nº de entradas), as entradas vêm **ORDENADAS por nome** (o motor faz
  **BUSCA BINÁRIA**), e cada entrada = `nome\0 + off(4) + size(4) + m0(4 mtime) + m1(4 =
  FLAG ZLIB: 1=comprimido, 0=cru)`. O `texbake`/`fixpak` PRECISAM: escrever o count,
  ORDENAR, e pôr `m1=1` nos `.ktx` (zlib). Errar isso = "source doesn't exist" (rosa).
- 🎨 **fixpak preenche .jpg/.png VAZIOS** do APK modado (ETC2.ktx→JPEG): sem isso, as
  ground-tiles vêm "0xff 0xd9" (JPEG vazio) → "Not a JPEG" → chão branco/preto. O
  `texbake` NÃO preenche (só faz cache) — **rodar fixpak ANTES** do texbake.
- 🪶 **texbake STREAMING + MULTI-THREAD + nice** (senão trava o device de 1GB):
  - streaming: grava cada ETC1 num `.tmpdata` na hora (NÃO acumula 600MB na RAM → OOM).
    RSS fica ~60MB. No fim concatena os temps no cache.
  - threads: `cores-1`. ⚠️ `sysconf(_SC_NPROCESSORS_ONLN)` MENTE (=1) no device; use
    `_SC_NPROCESSORS_CONF` (=4) OU conte `/sys/devices/system/cpu/cpu*`. Sem isso = 1
    thread = lento.
  - `nice -n 10`: senão os 4 cores a 100% deixam o device/sshd **irresponsivo** (parece
    travado). Com nice fica 25% idle = responsivo.
  - **Tempo no device (.164, Mali-450 4×A53): ~12.5min** o cache (602MB, 15436 texturas).
    1º boot total ≈ 17min (extract 3 + fixpak 2 + texbake 12.5). "demora" aceita.

### 1 binário universal
- Acabou o esquema 2-binários. **Um só** `dysmantle` = `dysmantle.compat.gcc` (GLIBC_2.27,
  build no Docker `debian:bullseye` via `build_compat_gcc.sh`). Roda em qualquer aarch64
  ≥2.27. Launcher sem detecção de glibc (BIN="dysmantle").

### Encoder ETC1 rápido
- `etc1_encode.c`: modo rápido (flip por heurística + sem refino), ~4-5× + rápido,
  qualidade ~igual. `etc1_set_fast(0)` volta pro exaustivo.

---

## 🔴 O QUE O PORTER QUER (ABERTO — não resolvido): "SEM CACHE" / só ETC1 nativo (igual SOR4)
O porter quer o pak **só com ETC1** (sem o arquivo de cache, sem JPEG) — o SOR4 faz isso
(ASTC→ETC1, no fim só ETC1). **O custo do cache atual: duplica no disco (JPEG ~600MB +
cache ETC1 ~600MB = 1.2GB)**; o SOR4 tem metade.

### Por que NÃO conseguimos (ainda) — o MURO
- **SOR4 controla o motor dele** (host MonoGame que NÓS escrevemos) → manda carregar ETC1.
- **DYSMANTLE = motor FECHADO.** Tentado pôr ETC1 no pak:
  - `.ktx` rotulado **ETC1 (0x8D64)** → motor **REJEITA** (CTEX=0, tela ROSA).
  - `.ktx` rotulado **ETC2 (0x9274)** com conteúdo ETC1 → motor **ACEITA mas CRASHA**
    (sig=11 addr=0x4a04) no `KtxImageLoader`, ANTES do `glCompressedTexImage2D` (CTEX=0).
- Hipótese: muro FUNDAMENTAL do ES2 (motor foi feito p/ JPEG-no-ES2 / ETC2-no-ES3; não tem
  caminho ETC1-no-ES2). Mesmo consertando o crash, o motor chamaria `glCompressedTexImage2D
  (0x9274 ETC2)` que o Mali-450 não amostra (nosso hook poderia subir como ETC1, mas o
  crash é ANTES).

### Onde a investigação parou
- Estava **capturando o local exato do crash** (rodar com `DYSMANTLE_KTX_REDIRECT=1
  DYSMANTLE_FORCE_ETC2=1` + pak ETC1-0x9274 + crash handler `game@offset`) p/ ver se é
  consertável. Pak ETC1 já gerado: `~/dysmantle-bake/n_data.pak` (753MB, 8233 .ktx ETC1).
  Crash anterior: stack tinha `game@0x4957dc / 0x4650e0 / 0x465060 / 0x75cce4...`.
- **PRÓXIMO PASSO se for atrás disso:** rodar a captura, pegar os `game@offset`, abrir o
  `libNativeGame.so` no Ghidra (`~/re-tools`), achar a função do `KtxImageLoader` que
  deref-NULL em +0x4a04, ver se dá p/ patchar (no-op / pular). Time-box: se for multi-muro,
  desistir e ficar com o cache.

### Alternativa "meio-termo" (sem cache-file, ~metade do disco) — NÃO testada
- texbake põe ETC1 no `.ktx` no pak + troca os `.jpg` grandes por **placeholder JPEG mínimo
  (1-3KB) das MESMAS dims** → o hook lê a ETC1 do pak (por nome) no upload. Resultado: pak
  ~630MB (vs 1.2GB), sem arquivo de cache separado, JPEG vira placeholder trivial. Ainda usa
  nosso hook (não é "ETC1 puro" como SOR4, mas é limpo e metade do disco).

---

## 📦 ESTADO DOS ARQUIVOS / COMO RETOMAR

### Repo: `~/nextos_ports_android/ports/dysmantle/`
- `src/imports.c` — **cache ETC1** (`etc1cache_load` via mmap, `etc1cache_find` bsearch,
  integrado no `try_upload_etc1`) + **guarda de tamanho** + **exclusão normals/specular**.
- `src/main.c` — `bk_last_bmp_name()` (expõe o nome da textura atual). KTX_REDIRECT +
  FORCE_ETC2 (`hook_bitmaploader`, `my_setbmpname`, `patch_func_ret1 IsTextureFormatSupported`).
- `src/texbake.c` — modos: bake_one (reescreve pak), **--sidetable** (cache, STREAMING +
  THREADED + flag --threads). `M_ETC1`=opaca→ETC1(rotulado **0x9274**!)/alpha→4444/5551.
- `src/etc1_encode.c` (fast), `src/etc2_decode.c`, `src/stb_image.h`, `src/fixpak.c`,
  `src/jpeg_enc.c`.
- `build_compat_gcc.sh` (binário Docker), `build_tools_aarch64.sh` (texbake+fixpak aarch64).
- `DYSMANTLE.sh` (launcher v5: BIN único, env `DYSMANTLE_ETC1CACHE`, redes de segurança
  fixpak+texbake com nice), `tools/dysmantle_extract.src` (v5: extract→fixpak→texbake→cache).
- `README.md`/`CHANGELOG.md`/`port.json` — já atualizados p/ v5.
- `ESTUDO-TEXTURAS-OFFLINE.md` — estudo original (com a opção "ETC1+alpha plane" guardada).

### Staging do zip v5: `~/dysmantle-v5/` (PRONTO p/ zipar)
- `DYSMANTLE.sh` + `dysmantle/` (binário `dysmantle`, `texbake`+`fixpak` aarch64, `libc++_shared.so`,
  `dysmantle.gptk`, tools/, port.json, README, CHANGELOG, cover/screenshot/splash, licenses).
- **Falta só:** validar o teste-limpo 100% (estava rodando) e `cd ~/dysmantle-v5 && zip -r
  "DYSMANTLE v5.zip" DYSMANTLE.sh dysmantle/` → pôr no Desktop.

### Build (sempre Docker p/ o binário e os tools aarch64)
- Binário: `SYSROOT=~/NextOS-Elite-Edition/build.*Amlogic-old*/toolchain/aarch64-*/sysroot;
  docker run --rm -v "$PWD":/repo -v "$SYSROOT":/sysroot:ro debian:bullseye bash /repo/build_compat_gcc.sh`
  → `dysmantle.compat.gcc` (GLIBC_2.27). PRECISA do `./dysmantle` nativo p/ extrair símbolos.
- Tools: `docker run --rm -v "$PWD":/repo debian:bullseye bash /repo/build_tools_aarch64.sh`
  → `texbake.aarch64` + `fixpak.aarch64`.
- texbake host (teste rápido): `gcc -O2 -I src -o /tmp/texbake src/texbake.c src/etc2_decode.c
  src/etc1_encode.c -ldl -lm -lpthread`.

### Device .164 (⚠️ login root/<senha>)
- `ssh root@<device-ip>` senha **<senha>**. Mali-450 Utgard (Amlogic Gxbb/S905), 832MB
  RAM, **4 cores** (mas sysconf online=1). SSH cai em comandos longos → rodar **detached
  (nohup) + polling por flag**. Install em `/storage/roms/ports/dysmantle/`. APK guardado:
  pode estar em `/storage/roms/_dys_apk_keep.apk` ou no dir. PSX apagada (47GB livres).
- **Rodar o jogo (env do cache):** `DYSMANTLE_ETC1CACHE=.../etc1.cache DYSMANTLE_ASSETS=assets
  DYSMANTLE_GLVER=2.0 LD_LIBRARY_PATH=/usr/lib:$PWD ./dysmantle`. Screenshot: `dd if=/dev/fb0
  of=fb.raw bs=1M count=8` → host: PIL `frombytes('RGBA',(1280,720))` + trocar BGR→RGB.

### 🚨 REGRA (crítica — já na memória)
**MATAR + CONFIRMAR 0 instâncias ANTES de lançar o jogo.** Kill por `/proc/*/exe`
(`pkill -x dysmantle` e `pkill -f caminho` NÃO casam: engine vira `{Main}`, launch é
`./dysmantle`). Ver `feedback_matar_confirmar_jogo_antes_de_lancar.md`.

### Referências
- v4 (base): `DYSMANTLE v4.zip` (diretório de entregas). SOR4 (modelo): `StreetsOfRage4.zip`
  (texconv ASTC→ETC1 + LZ4, host .NET, controla o motor MonoGame).
- APKs: `~/Downloads/com.dysmantle53.soco.GP_1.4.1.12-APK_Award.apk` (o analisado).
- `~/dysmantle-bake/` = paks de trabalho no host. `/tmp/apk_orig/` = paks originais (⚠️ /tmp é
  tmpfs, some no reboot — re-extrair do APK se preciso).

---

## DECISÃO PENDENTE (porter)
Cache (FUNCIONA, mas 1.2GB disco) **vs** só-ETC1 nativo (limpo como SOR4, mas crash do motor
fechado = RE incerto, talvez muro do ES2). O porter pediu "sem cache" mas disse "faça seu
melhor do seu jeito". **Recomendação:** se o crash não rachar rápido, fechar a v5 com o
cache (validada) — entrega imagem completa + sem stutter HOJE; deixar o "só ETC1" como
investigação futura (alto risco).
