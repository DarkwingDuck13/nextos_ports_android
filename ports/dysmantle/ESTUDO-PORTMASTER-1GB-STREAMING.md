# ESTUDO — DYSMANTLE: PortMaster universal + 1GB + texturas NATIVAS via streaming (2026-07-02)

## 0. Objetivo (o pedido)
1. Rodar em **TODOS os devices PortMaster** (áudio + vídeo), incluindo **R36S 1GB**.
2. **Texturas nativas** (acabar com o borrado do TEXSCALE 3.0 em low-RAM) — igual o Bully
   conseguiu com o streaming assíncrono.
3. Fixes de imagem valendo em todo device (mundo branco, magenta, shadows, ETC2).
4. Fazer o jogo **"achar que está num celular de 1GB"** (presets/pools menores da engine).
5. **Streaming de textura estilo Bully** (BULLY_PAGE async) adaptado pro Dysmantle.

---

## 1. Estado atual (fatos verificados no código, 2026-07-02)

### O port
- so-loader GameActivity/AGDK (`libNativeGame.so`, engine 10tons NX), `android_main`.
- Launcher **já é PortMaster-compatível**: probe 5-caminhos do `controlfolder`
  (DYSMANTLE.sh:40-50), `control.txt`, `get_controls`, `$GPTOKEYB`, `pm_finish`,
  `GAMEDIR=/$directory/ports/dysmantle`. NÃO é hardcoded NextOS.
- Dois binários: nativo (GLIBC_2.38) + `dysmantle.compat` (**GLIBC_2.27** — ABAIXO do piso
  PortMaster 2.30, cobre tudo). Escolha por `ldd --version`.
- Áudio: `opensles_shim.c` (OpenSL→SDL2, pump thread 4ms), **SDL_AUDIODRIVER/VIDEODRIVER
  nunca forçados** (regra #6) — SDL do CFW decide. Dysmantle é aarch64 ⇒ NÃO sofre o
  problema lib32/PipeWire do muOS.
- Input: gptokeyb → `DYSMANTLE_INPUT=gptk` → Paddleboat; SELECT+START no binário.

### Pipeline de textura HOJE (tudo estático, ZERO paginação)
- Offline: extração APK → `fixpak` (JPEGs vazios) → `texbake --sidetable` (cache ETC1
  por nome, escala = TEXSCALE) → opcional `--leanpak`.
- Runtime (`imports.c`): `my_glTexImage2D` (:1092) aplica TEXSCALE downscale (:1114-1162),
  ETC1-cache com verify anti-magenta (`try_upload_etc1` :989, `etc1cache_content_ok` :951),
  ISCALE do FBO de cena (:1192, clamp 0.4-1.0).
- **NÃO existe LRU/evict/paginação** (grep confirmado). `my_glBindTexture` JÁ hookado
  (:535/:1577); `my_glDeleteTextures` e `my_glTexSubImage2D` **NÃO hookados** (a engine
  IMPORTA glTexSubImage2D + glCopyTexSubImage2D — atlas em runtime, ver §2.3).

### Low-RAM hoje = qualidade sacrificada
- Launcher: MemTotal < 1100MB → **TEXSCALE 3.0** (feio) + ISCALE 0.65 + ES2.
- R36S ROCKNIX: MemTotal útil **~480MB** (CMA 98MB reservado GPU) + zram 524 do sistema.
  Funciona a 76fps **só com o ES parado**; com ES residente (+100MB) = swap-death.
- R36S/ArkOS-clone: ~639MB úteis + zram 512 (referência bully2).

### Muros de imagem já resolvidos/conhecidos (manter em TODO device)
| Problema | Status | Fix |
|---|---|---|
| Mundo branco (shaders `*Shadows` feature_level=2 → fmt 0) | RESOLVIDO | `hook_getshader` degrada nome do shader, default ON |
| Magenta (colisão de nome no cache ETC1) | RESOLVIDO | `etc1cache_content_ok` (verify MAD) |
| Dynamic shadows = crash no load (Utgard skinned→shadow FBO) | CONHECIDO | manter OFF (setting persiste! ver §5) |
| ETC2 nativo (ES3/KTX_REDIRECT) crasha ground-tiles no Mali-G31 | CONHECIDO | launcher já cai pra ES2 em low-RAM; ETC2 nativo SÓ ≥2GB |
| ETC1-direto-no-pak | **MURO do motor** (loader manifest-locked) | NÃO insistir |

### RAM: onde ela vai (medições .127, 832MB)
- RSS in-game ~470-530MB: `[anon]≈270-344MB` = **objetos vivos do mundo** (pools
  StageObjectAllocator — NÃO redutível por shim), texturas GL, `[stack]=131MB` anômalo
  (nunca investigado a fundo), heap 60MB.
- fps ~31-35 no Utgard é GPU/drawcall-bound; ISCALE é a alavanca de fps.

### O que o Bully já provou (código COMMITADO, `ports/bully/src/imports.c:306-570`)
Streaming assíncrono ~265 linhas + ~15 call-sites: registro no upload, swap em disco
write-once `<id>.tx` (**id-keyed** — imune a atlas/nome-stale, lição da Fase 3), LRU
clock, evict → 1×1 RGB (id continua válido), page-fault no bind, worker thread zero-GL +
ready-ring + drain na render thread, snapshot de metadados sob lock (anti-UAF), floor de
MemAvailable anti-OOM. Com paginação ativa o Bully **desliga ETC1/downscale = qualidade
nativa full-res, 60fps, RAM bounded**. Extraível com interface pequena (~8 accessors).

---

## 2. PLANO A — `DYS_PAGE`: portar o streaming do Bully (a peça central)

### 2.1 Mapeamento 1:1 (o que copia direto)
- Arrays `g_page_*[262144]` + lista compacta + LRU clock + `g_page_resident`.
- `dys_page_register` no fim do `my_glTexImage2D` (upload com pixels reais).
- `dys_page_write_swap` (`<swapdir>/<id>.tx`, header magic+dims+fmt, write-once,
  skip < 96KB, skip FBO px==NULL, skip texturas ISCALE/render-targets).
- `dys_page_on_bind` no `my_glBindTexture` (já hookado): toca LRU, fault/enqueue, evict.
- Worker + ring: `page_req_read` (só I/O) / `page_upload` (só GL) / `page_drain`.
- **ADICIONAR hook `glDeleteTextures`** (limpa estado, deleta .tx, id reusável) — não existe.
- Contabilidade `g_texbytes[id]` (novo no dysmantle; fault e evict usam o MESMO campo —
  lição do drift do bully).

### 2.2 Fontes de página (diferença-chave vs Bully)
- **kind=2 swap em disco = caminho principal** (id-keyed). O cache ETC1 por NOME serve
  como fonte kind=1 só pra UI/imagens diretas — no terreno a engine ATLASA e o nome não
  corresponde (fato provado no estudo do cache). Não depender dele pro streaming.
- Com `DYS_PAGE=1`: **TEXSCALE não se aplica** (upload nativo full-res) e o ETC1 lossy é
  pulado — exatamente a Fase 2 do Bully ("nativo, sem ETC1"). O bake offline continua
  existindo pros devices SEM paginação (fallback) e pro cold-boot rápido.

### 2.3 🔴 RISCO Nº1 — atlas em runtime via `glTexSubImage2D` (o Bully não tinha isso)
A engine importa `glTexSubImage2D` e `glCopyTexSubImage2D`. Se um atlas é evictado pra
1×1 e a engine faz SubImage nele depois → erro GL/corrupção; e o swap write-once não
refletiria os updates → re-fault traria atlas DESATUALIZADO.
Estratégia em camadas (implementar nesta ordem):
1. **MVP seguro**: hookar `glTexSubImage2D`/`glCopyTexSubImage2D` → textura que recebe
   SubImage/Copy vira **unpageable** (kind=0, sai do resident accounting). Medir com
   `DYS_PAGELOG` quanto sobra pageável. Se ≥60% dos bytes forem pageáveis, já resolve.
2. **Upgrade (se atlas dominar os bytes)**: no `glTexSubImage2D`, se a textura tem .tx,
   **aplicar o rect no arquivo de swap** (temos os pixels na chamada; memcpy por linha em
   mmap do .tx) → atlas continua pageável e o swap fica sempre atual. Se o mesmo id
   receber updates numa taxa alta (contador/seg), rebaixar pra unpageable (evitar I/O).
3. `glCopyTexSubImage2D` (fonte = framebuffer, sem pixels na mão): sempre unpageable.
4. Se SubImage chegar em textura EVICTADA: fault síncrono antes de aplicar (correção).

### 2.4 Cap/floor (lições do Bully, aplicar desde o dia 1)
- **Cap ACIMA do working-set visível** senão evicta textura na tela → preto preso em tela
  estática (rodada 4 do Bully). Começar generoso e apertar medindo.
- **Floor de MemAvailable** = rede anti-OOM real (evicta frias se avail < floor;
  < floor/2 = evicta quente). É isso que salva o R36S com ES residente.
- Evict: pular `!present` no filtro (bug dos "cadáveres 1×1" que custou 5M iterações).
- Mip/trilinear: dysmantle ES2 usa LINEAR nas ETC1 (sem mip) — no re-upload kind=2 RGBA
  respeitar o filtro que a textura tinha (guardar min-filter por id; regenerar mip SÓ se
  a textura usava mip — no Utgard cuidado com NPOT).
- Valores iniciais sugeridos: Mali-450 832MB: CAP=200 FLOOR=120; R36S ROCKNIX 480MB:
  CAP=100-120 FLOOR=80; R36S ArkOS 639MB: CAP=140 FLOOR=100. Tuning empírico.

### 2.5 Meta numérica (R36S ROCKNIX, o pior caso: 480MB + zram 524 + ES residente 100MB)
Hoje: RSS ~490MB c/ TEXSCALE 3.0 → swap-death com ES. Com DYS_PAGE: textura bounded
~100-120MB nativa + ISCALE 0.65 + preset Low (§3) cortando pool do mundo → alvo RSS
~350-400MB ⇒ cabe com ES residente usando zram do sistema como amortecedor de pico
(NUNCA criar/mexer em zram — regra #9; o do ROCKNIX/ArkOS já existe, só usar).

---

## 3. PLANO B — "celular de 1GB": fazer a ENGINE reduzir o mundo

Fato: RAM (~270-344MB de pool de objetos) e drawcalls são WORLD-CONTENT-bound; shim não
corta. Só a engine reduz. E a engine TEM o sistema completo (strings/símbolos verificados
no libNativeGame.so 2026-07-02):

- `Shadegrown::SetDetailLevelPreset(const char*)` + `ShadegrownDelegate::OnSetDetailLevelPreset`
  + **`DysmantleShadegrownDelegate::OnSetDetailLevelPreset`** (o jogo implementa o callback!)
- `ParticleEffectManager::SetDetailLevel(DetailLevel::Value)`,
  `ParticleEffect::RemoveDetailLevel`, `ParticleEffect::CreateDetailLevelFromDefaultDetailLevel[WithScaling]`
- `Shadegrown::SetShadowDetailLevel(int)`, `dynamic_shadow_detail_level`
- data.pak tem settings **`DetailLevel` / `ParticleDetailLevel` / `RemoveDetailLevel` /
  `ShadowDetailLevel`** + UI com botões Low/Medium/High (`<array id="LOW">/<array id="MEDIUM">`).

`hook_detail` (main.c:600, detour em lb+0xcca150) existe mas NUNCA dispara no boot — o
jogo não chama o setter sozinho no nosso caminho. Caminhos, na ordem do mais barato:

1. **Settings persistidos**: descobrir onde o jogo grava DetailLevel (gamedata/10tons/
   DYSMANTLE/ — mesmo formato `10tc`+zlib do profile.save, ou XML de settings). Se
   existir, **escrever Low direto no arquivo na instalação** (zero código no binário,
   mesmo espírito do BullySettings). Bônus: gravar `dynamic shadows=OFF` blindado (§5).
2. **Chamar nós mesmos**: xref de `SetDetailLevelPreset`/`OnSetDetailLevelPreset` pra
   achar o singleton Shadegrown → chamar `SetDetailLevelPreset("Low")` LAZY no render
   loop após "Renderer Initialization done" (lição FF9: hook/call no init trava boot).
   `hook_detail` já dá o detour; falta a CHAMADA ativa.
3. **`RemoveDetailLevel`** ("remove detail" = objetos decorativos): investigar se preset
   Low remove objetos do mundo → esse é o ÚNICO lever conhecido que corta o pool de
   ~300MB (o gargalo real de RAM). Prioridade alta de investigação.
4. **Spoof de device JNI**: este build consulta pouco (getBatteryLevel/Status,
   getDeviceId, GetDeviceModel — verificado). `GetDeviceModel` → devolver modelo 1GB
   (ex. "SM-J500H") custa 1 linha no jni_shim; verificar por xref se algo decide tier
   por modelo. Não há consulta de RAM via JNI neste build (igual bully2).
5. Knob no launcher: `DYSMANTLE_DETAIL=Low` default quando MemTotal < 1100MB (o knob já
   existe, só passa a ter efeito quando 1/2 funcionarem).

Ganho esperado: menos partículas/objetos = menos RAM de pool + menos drawcalls = mais
fps no Utgard (dois coelhos).

---

## 4. PLANO C — multi-CFW PortMaster (template = bully2, técnicas PROVADAS)

Copiar do bully2 (`ports/bully2/src/egl_shim.c`, `Bully.sh` v11):
1. **Escada de config GL** (egl_shim.c:62-117): `{ES2 a8 d24}→{ES2 a0 d24}→{ES2 a8 d16}→
   {ES2 a0 d16}→{ES3...}` com retry em EGL_BAD_CONFIG + **rejeição de contexto desktop-GL**
   (`ctx_is_gles`, lição sonic4/panfrost). Dysmantle hoje só pede uma config.
2. **Present por lista positiva** (egl_shim.c:123-133): driver `kmsdrm|wayland|x11`
   (**case-INsensitive** — bug "KMSDRM" maiúsculo do EmuELEC X5M) → SwapWindow; resto
   (mali/fbdev) → eglSwapBuffers cru.
3. **Preload versionado** de libs (`libGLESv2.so`→`.so.2`, `libEGL.so`→`.so.1`) pra CFW
   sem symlink -dev; + `/usr/local/lib/<triplet>` no LD_LIBRARY_PATH (libmali ArkOS).
4. **gptokeyb**: herdar o fix do check quebrado (`set -- $GPTOKEYB` testava o literal
   "sudo") — o launcher do dysmantle usa a mesma receita antiga, auditar.
5. `pm_platform_helper` + `MALLOC_ARENA_MAX=2` + trap cleanup EXIT/INT/TERM + `pm_finish`.
6. **Áudio**: manter SDL auto (regra #6). Se tester reportar mudo em muOS/Knulli/ALSA-cru,
   aplicar a varredura de drivers do sonic4 (tenta na ordem, NUNCA hardcode único).
7. Build: padronizar num binário compat único (GLIBC_2.27 já cobre o piso 2.30). O
   nativo 2.38 pode continuar como fast-path NextOS.
8. Matriz de CFW a validar (do STUDY bully2): ArkOS/dArkOS (glibc 2.30), ROCKNIX
   (mesa/panfrost — a escada+ctx_is_gles cobre), muOS/Knulli (PipeWire, 64-bit ok),
   TrimUI (PowerVR — quirks de shader, testar), RGCubeXX (720x720 1GB — testar ISCALE
   e layout de tela), X5M/NextOS (já validados).
- Lembrete de layout: X5M lista `.sh` em `ports/` direto; NextOS Elite usa
  `ports_scripts/`. O zip PortMaster padrão (`ports/DYSMANTLE.sh`) cobre os CFWs comuns.

---

## 5. Fixes de imagem consolidados (política única, todos os devices)
- `hook_getshader` (mundo branco): default ON sempre — já é. NÃO mexer.
- **Dynamic shadows**: crash no Utgard e o setting PERSISTE (usuário fica travado).
  Blindar: ao escrever settings Low (§3.1), forçar shadows OFF em device ES2/Utgard a
  cada boot (ou interceptar o setting). Em ES3 forte (X5M) pode ficar disponível.
- ETC2 nativo (passthrough ES3): SÓ RAM ≥ ~2GB (crash ground-tiles no Mali-G31 low-RAM).
  Launcher já faz — manter.
- Verify anti-magenta: ON sempre que cache ETC1 em uso.
- ISCALE: com DYS_PAGE liberando VRAM, tentar subir 0.65 → 0.8 no R36S (imagem mais
  nítida); medir.
- Vídeos intro 16:9 em painel 4:3 = letterbox cosmético, não mexer.

## 6. Ordem de execução (fases pequenas, commit a cada validação)
- **F1** — DYS_PAGE MVP síncrono (register + swap kind=2 + evict + fault no bind +
  hook glDeleteTextures + SubImage⇒unpageable) na bancada Mali-450 (tem swap = seguro).
  Validar: sem preto, resident ≤ cap, screenshot dys_shot.
- **F2** — async worker + floor MemAvailable + `DYS_PAGE⇒TEXSCALE off` (nativo full-res).
  Validar qualidade vs TEXSCALE 3.0 (a foto que importa).
- **F3** — R36S ROCKNIX + ArkOS com **ES residente**: tuning cap/floor; meta = carregar
  fase + andar rápido (streaming de zona) sem swap-death. Se SubImage-unpageable pesar,
  implementar o rect-no-swap (§2.3.2).
- **F4** — preset Low / "celular 1GB": settings file → chamada direta → RemoveDetailLevel.
  Medir RAM de pool antes/depois (smaps anon).
- **F5** — launcher multi-CFW (escada GL, present list, preload, gptokeyb fix, binário
  único) + zip PortMaster.
- **F6** — testers externos (muOS/Knulli/TrimUI/RGCube): áudio+vídeo+controle.

## 7. O que NÃO fazer (muros e regras já pagos)
- ❌ zram (regra #9) — usar o swap/zram que o CFW já tem.
- ❌ forçar SDL_VIDEODRIVER/SDL_AUDIODRIVER (regra #6).
- ❌ ETC1-direto-no-pak / KTX_REDIRECT em low-RAM (muros provados).
- ❌ cache/encode em RUNTIME (rejeitado no Bully; bake offline na instalação é ok).
- ❌ cap de paginação abaixo do working-set visível (= preto em tela estática).
- ❌ name-keyed swap (Fase 3 do Bully: nome stale em load em lote = textura errada;
  id-keyed é imune).
- ❌ 2 instâncias (comm="Main"! matar via `ps w | grep "\./dysmantle"`, kill por
  /proc/*/exe, confirmar 0 antes de relançar — regra #3).
- ❌ dynamic shadows ON em Utgard.

## 8. Resumo executivo
O Dysmantle já tem launcher PortMaster e binário compat abaixo do piso; o que falta pra
"todos os devices" é a robustez multi-CFW do bully2 (escada GL + present list + preload),
que é trabalho de template. A qualidade de textura em 1GB se resolve portando o streaming
assíncrono do Bully (id-keyed, ~265 linhas, commitado e validado) com UMA novidade
obrigatória: tratar `glTexSubImage2D` (atlas em runtime). E o corte REAL de RAM/fps em
1GB não está em shim nenhum — está em fazer a engine rodar no preset Low ("celular
fraco"), e ela tem a infraestrutura completa (`DysmantleShadegrownDelegate::
OnSetDetailLevelPreset`, ParticleDetailLevel, RemoveDetailLevel) esperando ser acionada.
Os três planos são independentes e somam: streaming (textura nativa bounded) + preset
Low (pool do mundo menor) + ISCALE = R36S 480MB com ES residente vira viável.
