# Mega Man X (Capcom) — Port so-loader → Mali-450 (.79)

APK: `~/Downloads/mega-man-x-androeed.store-0-1730359376.apk` (135MB, `jp.co.capcom.rockmanx` v1.07.00 vc38).

## Ficha da engine
- **Unity 2021.3.39f1 IL2CPP**, metadata **v29** (`0x1d`).
- **arm64-v8a APENAS** (sem armv7). ABIs: `libil2cpp.so` (62MB), `libunity.so` (17MB), `libmain.so`.
- Pipeline **built-in** (SEM URP). Sprites 2D + TextMeshPro. InputSystem novo, Addressables 1.19.19, Localization, UniTask.
- **GLES 3.0** exigido (`uses-gl-es 0x30000`); Vulkan opcional.
- Conteúdo **100% local** (`resources.resource` 68MB + `data.unity3d` 37MB). Addressables só localização (0.35MB), **nada remoto**.
- DRM: **sem pairip**. LVL crackeado (o APK é "LVL Extreme crk"), Play Integrity só SDK (não enforça). Activity = `UnityPlayerActivity` padrão.
- **libil2cpp EXPORTA a API `il2cpp_*` inteira (236 símbolos)** → dá pra `dlsym`, não precisa hardcodar offsets (≠ Terraria).
- Localização: tem japonês; forçar inglês (regra #5) — pendente.

## Base reaproveitada: Terraria (Unity 2021.3.56f2 IL2CPP)
Mesma geração de engine. Reuso quase 1:1 de `ports/terraria/src/`:
`so_util.c` (loader ELF), `egl_shim.c` (EGL+GLES3→GLES2 Mali Utgard), `jni_shim.c`+`jni_idx_stubs.gen.c` (JNI fake estilo Bogodroid), `opensles_shim.c` (áudio), `bionic_shims.c`/`pthread_fake.c`/`sem_shim.c` (bionic→glibc), `native_pad.c`/`gamepad.c` (controle), `imports.gen.c` (tabela dynlib_functions).

- **`imports.gen.c`**: reusei a tabela do Terraria; MMX precisava de só 12 símbolos extras (todos libc/bionic triviais): `asin log10 log2f fstat ftello lseek mmap stat statfs` (passthrough dlsym) + stubs `ALooper_pollAll gethostbyaddr __FD_ISSET_chk`. Já adicionados.
- **`main.c`**: copiado do Terraria (é o so-loader do Cuphead adaptado). Os hooks específicos de jogo (com offsets hardcodados 0x73c860 etc.) são TODOS `getenv`-gated (TER_*/CUP_*) → auto-desligam se o launcher não os setar. Só troquei os paths (`/storage/roms/terraria`→`/storage/roms/megamanx`).

## boot.config (copiado do Terraria)
```
androidUseSwappy=0
gfx-disable-mt-rendering=1
gfx-enable-gfx-jobs=0
gfx-enable-native-gfx-jobs=0
```
(mais as 4 linhas padrão). NÃO resolveu o stall do choreographer (Unity 2021.3 usa o Choreographer Java mesmo com Swappy off), mas mudou hang→crash no ponto de stall.

## Build / deploy / run
- `build.sh` — toolchain NextOS Amlogic-old aarch64 (glibc 2.38, casa com o .79 EmuELEC 4.8). Compila OK. Binário `megamanx` (ELF pie aarch64, NEEDED libSDL2/libm/libc).
- (release futuro: `build_glibc230.sh` Docker buster p/ outros devices — ainda não usado.)
- Device `.79`: EmuELEC 4.8, kernel 3.14.79, fb0 1280x720, Mali `libGLESv2→libMali.so` (Utgard GLES2), SDL2 2.32.67.
- Layout no device: `/storage/roms/megamanx/{megamanx, libil2cpp.so, libunity.so, libmain.so, bin/Data/...}`.

## 🚨 HARNESS DE DEBUG SEGURA — `dbgrun.sh` (LIÇÃO CARA)
O device **asfixia** se um megamanx roda em busy-wait sem controle (o busy-wait do frame estrangula o sshd → KEX não completa → perde o device, precisa reboot físico). Regras da harness:
1. **`nice -n 19`** no jogo → sshd SEMPRE preempta → nunca mais perde o device. (validado: load 1.0, ssh responsivo durante o busy-wait.)
2. **`timeout DUR`** sempre → auto-kill. **NUNCA lançar sem timeout** (foi o erro que asfixiou).
3. **Watchdog** externo (DUR+10s) de reforço.
4. Matar instância antiga por **exe-symlink** (`/proc/*/exe`), NÃO por `pgrep -f path` — o binário roda como cmdline relativo e o match por path absoluto falha (bug que deixava instâncias velhas vivas).
5. **NUNCA `setsid`** (regra #8, atrapalha fbdev) — usar `nohup`.
6. Log persistente: `run.out` no SD (sobrevive a hang/power-cycle).
Uso: `ssh root@.79 'sh /storage/roms/megamanx/dbgrun.sh 45 [ENV=val...]'`

## 🎉 BOOT DESTRAVADO (2026-07-03 s1) — jogo RODANDO frame 2800+
✅ Boot completo: libunity+libil2cpp, JNI_OnLoad, initJni, RecreateGfxState, **render loop rodando milhares de frames**, framebuffer ATIVO (fb0 75% não-preto). **Identidade corrigida** (era `com.and.games505.TerrariaPaid`+path terraria hardcodado no jni_shim → agora `jp.co.capcom.rockmanx`).

**A RECEITA que destravou** (no `run.sh`):
```
MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005
```
- **`MMX_INLINETASK`**: trampolim em `libunity+0x350580` (per-object-task loop, `ldr x8,[x19,#88]`) chamando `mmx_inline_task(obj)` que finge a conclusão: `node=*(obj+88); *node=1` → satisfaz o `cbnz` em 0x35058c → sai da espera 0x35059c. (Análogo EXATO do TER_INLINETASK do Terraria em 0x2f37a4.)
- **`MMX_PATCH=0x34eafc=0x14000005`**: a `WaitForJobGroup` (`libunity+0x34eafc: b.ge` → `b` incondicional) sai imediato. É `while(*(counter@0x10e9d20) < target) cond_wait`; os jobs vão p/ workers ociosos (dispatch quebrado no so-loader) e nunca completam → sem o patch, deadlock. Análogo do TER_SKIPJOBWAIT (0x2f1d48). Os job-results são não-críticos pro boot (só um `NullReferenceException` tolerado).
- (`MMX_JOBCOUNTER=0x10e9d20` incrementa o counter no inline_task, mas 1 incremento < target; o patch do WaitForJobGroup é o que resolve. Manter como alternativa.)

## ⛔ MURO ATUAL: shaders GLES3→GLES2 (tela = cor uniforme)
O jogo renderiza mas o fb0 fica **uniforme (`ffff00ff`)**, log repete `Desired shader compiler platform 5 is not available in shader blob`.

**Dados coletados (via MMX_GLLOG=1, CUP_SHADERDUMP=1):**
- O Mali REAL reporta: VENDOR=`ARM`, RENDERER=`Mali-450 MP`, VERSION=`OpenGL ES 2.0`. Contexto = GLES2 de verdade.
- **0 shaders chegam ao `glShaderSource`** ([SHSRC]=0) → NÃO é erro de compilação, é **seleção**: o Unity nem acha variante pro platform que deseja.
- Unity DESEJA **platform 5** (= GLES3Plus no enum ShaderCompilerPlatform do 2021.3) apesar do contexto ser GLES2.

**Diagnóstico:** o GfxDevice do Unity foi criado como **GLES3** (o jogo exige GLES3 no manifest; o blob Mali-450 aceitou um `eglCreateContext` com CLIENT_VERSION=3 e devolveu "sucesso" mas o contexto real é 2.0 — por isso o game NÃO recusou com "OpenGL ES 3.0 is required" e roda). Como o device interno = GLES3, o Unity busca shaders platform-5 que o blob deployado não tem nesse formato → nada renderiza.

## 🚀🚀 PROGRESSO MASSIVO (2026-07-03 s1, modo foco) — jogo BOOTA+RODA 60fps
Sequência de muros VENCIDOS:
1. **Boot job-system** ✅ `MMX_INLINETASK=1 MMX_PATCH=0x34eafc=0x14000005` (trampolim per-object-task + skip WaitForJobGroup).
2. **Play Integrity (DRM)** ✅ `MMX_NOINTEGRITY=1` — o `BootScene.Start`→`IntegrityCheck`→`PlayCoreIntegrityManager..ctor` dava NullReferenceException (JNI fake) → boot morria. Achei a coroutine aninhada `IntegrityCheck/<Start>d__4::MoveNext` via API il2cpp exportada (`mmx_nuke_integrity` em main.c) e patchei → `mov w0,#0; ret` (IEnumerator "done" na hora). ⚠️ Timing: warmup 2 frames (patchar ANTES do BootScene.Start; frame 0 crasha pq il2cpp não pronto).
3. **Shaders GLES3→GLES2** ✅ **TRANSPILADOS OFFLINE** por `transpile_shaders.py` (UnityPy): traduz o GLSL HLSLcc (#version 300→100, in/out→attribute/varying, flatten UBO, layout removido, texture→texture2D, flat removido, SV_Target→gl_FragColor) + muda programType interno 2/3/4→**5 (GLES2)** + **atualiza o LENGTH do subprograma na entrada (offset,LENGTH,0)** + relabel platform 9→5. 96% dos subprogramas (491/511) viram GLES2 válido. **Os shaders do jogo agora CHEGAM ao glShaderSource e COMPILAM+LINKAM (0 falhas!)** via o hook `MMX_XLATE` (que descobri: Unity resolve GL por `dlsym(RTLD_DEFAULT)`, não eglGetProcAddress).

## ⛔ MURO ATUAL (o último): job-system quebra o load da cena
O jogo roda a **60fps mas `draws/f=0`** — NÃO desenha geometria (só limpa a tela). A tela fica **magenta** (é a cor de CLEAR, não error-shader — os shaders compilam!). **Causa:** o skip da WaitForJobGroup (`MMX_PATCH 0x34eafc`, necessário pro boot) **impede o carregamento assíncrono da cena** — os jobs que pulo são o load de assets. Conflito fundamental: boot precisa do skip, conteúdo precisa dos jobs. `TER_JOBINLINE` (força 1 CPU→jobs inline) sozinho NÃO destrava o boot (deadlock volta). **Fix real:** fazer o job-system do Unity PROCESSAR os jobs de verdade no so-loader (workers "Job.Worker" existem mas ficam ociosos em futex — dispatch/sem_post quebrado) OU rodar inline mantendo o boot destravado. Trabalho de threading profundo (o Terraria também penou nisso). Ferramentas: MMX_MAINWAIT, CUP_DRAWCOUNT (draws/f), CUP_SHADERDUMP.

## 🧱 (histórico) CAUSA-RAIZ DOS SHADERS — resolvido pelo transpilador acima
Mapeamento empírico do ShaderCompilerPlatform (decomprimindo os blobs): **5=GLES2 (`#version 100`), 9=GLES3 (`#version 300 es`), 18=Vulkan (SPIR-V)**.
- **Shaders do JOGO (data.unity3d, 53):** só platform **9 (GLES3) e 18 (Vulkan)** — **ZERO GLES2**. O jogo foi buildado GLES3-only (exige GLES3 no manifest).
- **Built-in (unity default resources):** têm 5, 9, 18.
- O Mali-450 do .79 é **GLES2 puro** (deu contexto ES 2.0 pro request v3 do jogo). Unity vira GfxDevice GLES2 → deseja platform 5 → os shaders do jogo (só 9/18) não existem → `platform 5 not available` → NullReferenceException quebra TODA a setup de render (nem os built-in GLES2 compilam) → tela uniforme.
- O GLSL dos shaders do jogo é **HLSLcc GLES3 com UNIFORM BUFFERS (UBOs)** + `layout(location/binding, std140)` — features SEM equivalente em GLES2.

**Tentativas feitas (não resolveram):** relabelar platform 9→5 no data (Unity ACHA, err5=0, mas o tipo INTERNO do blob ainda é GLES3 → device GLES2 pula → SHSRC=0); spoofar glGetString ES3.0 (MMX_FAKEGLES3 — Unity detecta pelo CONTEXTO real, não muda); forçar GL major=2; NULL nas funções GLES3 do eglGetProcAddress. Nada faz o Unity submeter os shaders. libunity resolve TODO GL via eglGetProcAddress (hook OK via MMX_XLATE).

**Os 2 caminhos possíveis (ambos GRANDES):**
1. **Transpilador GLES3→GLES2 + repack do blob:** decomprimir cada subprograma platform-9, traduzir o GLSL (UBO block→uniforms individuais, `in/out`→`attribute/varying`, remover `layout()`, `texture()`→`texture2D()`, `#version 300→100`), trocar o tipo interno GLES3→GLES2, relabelar platform→5, recomprimir/repack o data.unity3d. Ferramentas prontas: UnityPy + `my_glShaderSource` (hook via MMX_XLATE já wira). Esforço alto; alguns shaders podem não ter equivalente GLES2.
2. **Rodar num device GLES3** (ex: R36S/RK3326 Mali-G31, X5M Mali-G310) — aí os shaders platform-9 rodam nativos. Mas a regra é Mali-450 (.79). 

Caminhos antigos (descartados):
1. Forçar o GfxDevice p/ **GLES2** (arg `-force-gles20` do Unity / boot.config) → Unity deseja platform 4 (GLES20); ver se o blob tem GLES2. ⚠️ risco: o check "GLES3 required" recusar (mas hoje já roda em 2.0).
2. OU garantir que o blob tenha as variantes do platform desejado (checar se deployei TODO o shader data: `unity default resources`, `globalgamemanagers`, resources.resource).
3. Estudar o shim ES3→ES2 do Mina/GTA SA (memória [[feedback_referencias_shim_es3_validas]]) — como eles alinham platform+contexto.
- `my_glShaderSource` (main.c 1297) existe mas é ALPHAFIX do Cuphead, não tradutor GLSL — precisará virar tradutor #version 300→100 SE o caminho for contexto ES3 real.
- Ferramentas: `MMX_GLLOG=1` (versão GL), `CUP_SHADERDUMP=1` (fontes submetidos), `CUP_GLES_MAJOR` (força versão no egl_shim — só kmsdrm).

## Histórico do muro anterior (RESOLVIDO): Choreographer/job-system
(mantido p/ referência — resolvido pelo INLINETASK+SKIPJOBWAIT acima.) A main travava em `pthread_cond_wait_fake` dentro da libunity esperando o job-system/HandlerThread; nondeterminístico (crash rápido / hang no gdb).

### Pistas
- A main espera um cond em libunity (Terraria documenta o análogo em `libunity+0x2f3680`).
- `jni_shim` JÁ dirige `handleMessage` no `sendToTarget` (linha ~1024) e tem driver de `doFrame` (`TER_CHOREO`). Mas a main trava ANTES de chamar `sendToTarget` (só resolve o methodID), então esses drives não disparam.
- `TER_CHOREO` não ajuda: seu driver-thread só é criado no F2 (após RecreateGfxState), DEPOIS do ponto onde a main já travou.
- `CUP_CONDPOLL=100` (timedwait c/ wakeup espúrio) muda hang→crash rápido (a main acorda e prossegue com condição falsa → lê lixo).
- `TER_CONDTRACE=1` não logou `[CT]` (investigar — deveria pegar o call-site da espera da main).
- Como o **boot1 chegou a frames**, NÃO é muro arquitetural — é timing da setup do choreographer no so-loader (mesmo problema que o Terraria domou; ver `ports/terraria/HANDOFF.md`).

### Achados dos experimentos (2026-07-03, todos via harness segura)
- O stall em **544 é DETERMINÍSTICO** (3/3 e mais). O boot1 que chegou a frames (947) NÃO reproduziu — foi caso isolado.
- **NÃO** é o `nice` (sem nice também trava 544). **NÃO** é a race de scheduling.
- Reverter o boot.config (tirar gfx-disable-mt/swappy) **NÃO** ajuda (some com o crash, volta pro hang, mas ainda 544).
- `CUP_CONDPOLL=100` muda hang→crash (main acorda com condição falsa). `TER_CONDTRACE=1` NÃO loga `[CT]` (investigar por que — env propaga, provado pelo CUP_CONDPOLL).
- O log SEMPRE para em `GetMethodID(sendToTarget)`. **`obtainMessage(what=..)` NUNCA é chamado** (não loga) → a main bloqueia ANTES de postar a Message — provavelmente no **`HandlerThread.getLooper()`** (ou no cond nativo em libunity que espera o handler-thread sinalizar "looper pronto"). O `jni_handlemessage`-no-sendToTarget não dispara porque nunca chega lá.

## 🔬 RE PROFUNDA (2026-07-03 s1) — os DOIS loops de espera EXATOS

Ferramentas adicionadas (todas env-gated, `dbgrun.sh N ENV=val`):
- **`MMX_MAINWAIT=1`** — loga `[MAINWAIT] #N cslot=.. caller=libunity+0xOFF` de cada cond_wait da MAIN (vai pro `debug.log`, NÃO run.out — o main.c redireciona stderr; sempre ler `debug.log`).
- **`TER_CONDTRACE=1`** — loga `[CT] WAIT` de TODAS as threads (com comm+caller).
- **`MMX_SKIPWAIT="u:0xOFF,i:0xOFF"`** (+`MMX_SKIPWAIT_MS`, def 2000) — no cond_wait desses callers usa timedwait; se estourar (stuck) retorna 0 (prossegue). u=libunity, i=libil2cpp, qualquer thread.
- **`MMX_PATCH="0xOFF=0xWORD,..."`** — escreve words de 32b crus no .text da libunity (faz `so_make_text_writable` antes).

**As esperas (a main é a thread "UnityMain" tid==pid; há uma 2ª thread "UnityMain" tbm):**
1. **libunity+0x34d1d0** = `UnitySwappy` (func @0x34d110): cria uma thread (`pthread_create` @0x34d1a8) e faz `while(*(x20+32)==0) cond_wait` (flag "thread pronta" em x20+32). A thread spawada nunca seta o flag no so-loader → trava. Recorrente no boot; a maioria SUCEDE, a do frame 2 trava.
2. **libunity+0x35059c** = fila de mensagens do Handler (func perto de JNI_OnUnload): `while(*(x19+88)==0 || *(*(x19+88))==0) cond_wait` (fila em x19+88, espera uma Message ser enfileirada NA FILA NATIVA). O `sendToTarget`→`jni_handlemessage` do nosso shim NÃO popula essa fila nativa.
3. **libil2cpp+0xd1ac28** = espera de thread da folha (na 2ª "UnityMain").

**Testes feitos:**
- `MMX_SKIPWAIT` (timeout-return) nos 3 → NÃO crasha mas SPINNA (são `while(cond)cond_wait`; retornar não muda a condição → re-loop a cada MS). Sem progresso.
- `MMX_PATCH=0x34d1bc=0x14000007` (cbnz→b saída, pula o wait do Swappy) → loop sai MAS crasha em `unity+0x34d318` (fault=0x0): o código pós-loop usa o estado do Swappy que não ficou pronto.
- ⇒ **Pular NÃO serve** (estado inválido → crash). O fix certo é **FAKE-COMPLETION** (estilo Terraria INLINETASK/SKIPJOBWAIT): prover o estado que a thread spawada deveria ter setado.

**RAIZ ÚNICA:** as threads (Swappy tracer, workers, looper) ficam ociosas porque o **choreographer nunca entrega frame-callbacks** (Looper fake). Elas esperam frames; sem frames, não inicializam; sem init, não setam os flags que a main espera. Tudo encadeia no choreographer.

## Próximos passos (ordem de promessa)
1. **Entregar frame-callbacks de verdade** (fix da raiz, não fake): o jogo registrou o nativo `nOnChoreographer(JJ)` (Android Game SDK, via RegisterNatives — visto no log `ChoreographerCallback ... nOnChoreographer=0x...`). Capturar esse ponteiro + o **cookie** (1º arg long, passado quando o jogo cria o ChoreographerCallback) e uma driver-thread chamar `nOnChoreographer(cookie, frameTimeNanos)` ~60Hz. Isso deve acordar Swappy/workers → setam os flags → as esperas 0x34d1d0/0x35059c saem LIMPAS. (≠ do `TER_CHOREO` que dirige o `doFrame` JAVA; o MMX usa o nativo do Swappy.)
2. Se (1) não bastar: **fake-completion** de cada loop. Pro Swappy (0x34d110): fazer a thread spawada (ou um hook) setar `*(x20+32)=1` + signal, e prover o que 0x34d318 deref-a (desmontar 0x34d318 pra ver o campo null). Pro Handler-queue (0x35059c): enfileirar uma Message válida na fila nativa x19+88 (achar a struct do nó).
3. Comparar com `ports/terraria/HANDOFF.md` (o Terraria venceu o análogo; ver os offsets 0x2f37a4/0x2f1d1c/0xc10360 e adaptar a técnica).

**Como retomar:** device .79 tem o binário instrumentado. `sh /storage/roms/megamanx/dbgrun.sh 45 MMX_MAINWAIT=1 TER_CONDTRACE=1` e ler `/storage/roms/megamanx/debug.log`. Desmontar libunity: `$TC/bin/aarch64-libreelec-linux-gnu-objdump -d --start-address=0xOFF --stop-address=0xOFF payload/lib/libunity.so`.
2. Confirmar por que `TER_CONDTRACE` não loga (pegar o offset exato da espera em libunity e comparar com Terraria 0x2f3680).
3. Depois do render loop estável: validar IMAGEM no fb0 (risco GLES3→GLES2 / Texture2DArray), forçar inglês, mapear InputSystem novo.
