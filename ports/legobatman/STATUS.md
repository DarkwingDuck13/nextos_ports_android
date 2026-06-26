# 🦇 LEGO Batman 3: Beyond Gotham → Mali-450 (.79) — STATUS

Engine **Fusion** (`libLEGO_Black_Mobile.so`, arm64, ES2 native, no DRM).
Device `192.168.31.79` (sshpass -p '' root@.., EmuELEC, Mali-450 Amlogic).
Golden ref framework = LEGO Star Wars TCS (`~/lswtcs/`, src `docs/reference/lswtcs-src/`).

# 🦇 LEGO BATMAN 3 — RESUMO DO QUE FALTA (2026-06-26, fim s5)
**JÁ FUNCIONA:** boot, render LIMPO/estável (FBCOPY), título "Beyond Gotham", menu, **controle Xbox físico navega o front-end** (Touch to start → menu → New Game), New Game INICIA.
**FALTA (3 coisas):**
1. 🔴 **GAMEPLAY** — clicar New Game **trava NA HORA**. Causa achada (gdb): **DEADLOCK de 2 threads no GL do Mali** ao compilar o 1º shader COMPLEXO de gameplay (`light=dlit,refl=blinn,refl2=cubemapadd,dirlight=2,vctint=1`): MAIN thread em `futex_wait`, thread worker (compilando o shader) presa em `_mali_osu_lock_wait` (lock interno do driver Mali). Classe RE4 (single-context EGL não suporta 2 threads em GL ao mesmo tempo). Tentativa em curso: serializar GL com mutex global (ver egl_shim_ensure_current) OU forçar compile de shader síncrono numa thread só.
2. 🔊 **ÁUDIO COMPLETO** — sem som. opensles cria players (CreateAudioPlayer) mas a engine nunca dá Enqueue de PCM (thread de decode/áudio não alimenta). `fnaSound_Update` 0x3ca454.
3. 🎬 **FUNDO ANIMADO ATRÁS DOS MENUS** — falta o fundo animado do menu principal (NextOS: não sei se é VÍDEO ou fundo 3D animado). O título tem cena 3D (Terra/Gotham, é 3D real-time no original). Cutscenes de nível = 36 .mp4 (vídeo via GameVideoPlayer+MediaPlayer+"video layout" Android, que não temos). NÃO é crítico p/ jogabilidade.

---

## 🎮 s5 (2026-06-26) — CONTROLE NAVEGA O MENU + NEW GAME INICIA; trava no load do 1º nível (DEADLOCK GL Mali)

🔬 **CAUSA DO FREEZE CONFIRMADA (gdb no estado travado):** New Game → compila shaders de
gameplay → ao compilar um shader COMPLEXO (`albedo=textured,light=dlit,refl=blinn,
refl2=cubemapadd,dirlight=2,vctint=1`, prog=60) o log corta no meio do dump e **trava**:
- MAIN thread: `futex_wait` (bloqueada num lock).
- thread worker "legobatman": `_mali_osu_lock_wait` em libGLESv2 (lock interno do driver Mali).
- ⇒ **deadlock de 2 threads sobre o contexto/lock GL único do Mali** durante compilação de
  shader. (single-window EGL: 1 contexto, 1 thread por vez; a engine compila shader numa thread
  worker enquanto a main também mexe no GL → fight no lock do Mali → trava.)
- EGL_BAD_ACCESS (2x) é só no BOOT, não é o culpado do freeze.
🛠️ **FIX a tentar (próxima sessão):** serializar TODO acesso GL com um mutex global recursivo
(lock em volta de ensure_current + chamadas GL, uma thread por vez), OU achar flag de
shader-compile síncrono, OU dar contexto próprio às worker threads (mas EGL_BAD_ACCESS atrapalha).
Mesmo muro do RE4 (job-system + GL multi-thread).

✅ **Render limpo (FBCOPY) deixou a resposta do controle VISÍVEL** → com o flicker fora,
o **controle Xbox físico NAVEGA o front-end** (Touch to start → menu → New Game) — NextOS
confirmou "controle funcionou". O front-end **responde ao joypad** via `nativeControllerSetData`
(o flicker é que escondia a resposta antes; NÃO é touch-only como eu achava na s3). Commits
fb63579 (FBCOPY) + 059156e (controle/FMV).

✅ **NEW GAME INICIA** (shaders de nível light=prelit/outline compilam).
🔴 **TRAVA no load do 1º nível** (NextOS: "travou após clicar"). Diagnóstico (gdb):
- MAIN thread NÃO travada — está em `SDL_Delay`/nanosleep do nosso loop, roda `nativeRender`
  todo frame (polling). Workers idle em `pthread_cond_wait`. Tela fica no menu (mean 82).
- `eglCreateContext FAILED: EGL_BAD_ACCESS` aparece só 2x (no BOOT, não no load) → **NÃO é
  o culpado do load**. (single-window EGL não compartilha contexto enquanto a main segura a
  surface — investigar depois se virar gargalo.)
- Muitas `pthread_create_fake(entry=fnaThread_ThreadProc 0x3cfd0c, arg)` → cada uma chama
  `arg->func(arg->param)` (3cfd58) e "entry returned (nil)" (tarefas one-shot de load paralelo
  OU workers de job que morrem cedo). `shaderbin.fib` MISS (compila shaders em runtime, lento).
- ⇒ É a **classe deadlock job-system / load-do-nível** (igual RE4). O game-logic dentro do
  nativeRender espera um job/recurso que nunca completa.
- 🎬 **fnaFMV_SetMovieInfo(0,0) por frame (LBBG_NOSKIPFMV desliga)** força "movie finished"
  (byte estado 0xc3b660; fnaFMV_Finished=byte==0) p/ cutscene-vídeo não travar — **NÃO resolveu**
  o freeze (logo o hang é o load do nível, não a espera da cutscene). Hipótese do NextOS
  (vídeo) era boa mas o gargalo é outro.

### 🎯 PRÓXIMOS (freeze do load do nível)
- [ ] Caracterizar o job-system: identificar qual job o game-logic espera (instrumentar
      fnaThread/job-post-wait; achar o semáforo/cond que não é postado). Comparar contagem de
      threads/jobs vs RE4 (sh_sem_init).
- [ ] Testar: New Game vai pro Batcave/hub ou direto pra cutscene+nível? (logar qual .fib de
      nível/cena a engine pede no New Game).
- [ ] shaderbin.fib: servir o prebuiltshaders_android.fib como cache p/ não compilar em runtime
      (acelera load, pode destravar se for timeout).
- [ ] Empacotar launcher (LBBG_PADTAP p/ controle) + saves.

## ✅✅ s4 (2026-06-26) — FLICKER/LIXO RESOLVIDO: FBCOPY (double-buffer por software) + BANCADA NO CELULAR

🏆 **O FLICKER/LIXO (a "briga de buffers" que o NextOS via) ESTÁ RESOLVIDO.** Título agora
renderiza **limpo e estável** no device .79 ("LEGO BATMAN BEYOND GOTHAM / Touch to start").

🔑 **RAIZ do flicker = single-buffer:** o engine (GL single-buffer) redesenha a metade VISÍVEL
do fb 1280x1440 todo frame → o scanout pega frame meio-desenhado → tearing/lixo (disclaimer
sobre spinner). Meus dumps de /dev/fb0 pegavam frames COMPLETOS da metade de baixo, por isso
não mostravam o flicker que o NextOS via na tela real.

🔑 **FIX = `LBBG_FBCOPY` (AGORA PADRÃO):** double-buffer por software.
- engine renderiza na metade de BAIXO (back, escondida); display fica fixo na metade de CIMA
  (front, `yoffset=0`); no present faço `glFinish()` + **`memcpy` baixo→cima** de um frame
  COMPLETO via mmap de /dev/fb0. Display só mostra frame completo → ZERO tearing.
- `egl_shim.c`: `fbcopy_present()` (mmap fb, FBIOPAN yoffset=0, memcpy half). Default on
  (desliga com `LBBG_NOFBCOPY`). `main.c`: chama `egl_shim_present()` no loop após `nativeRender`
  (o front-end NÃO limpa FBO0 todo frame — conta com a camada de vídeo — então o hook do glClear
  não dispara sempre; dirijo a cópia do loop). pan_keeper só no modo legado.
- ⚠️ p/ VALIDAR remotamente: dumpar /dev/fb0 e olhar a **metade de CIMA [0:720]** (= o que o
  display mostra), não a de baixo.

📱 **BANCADA NO CELULAR FUNCIONANDO (adb por REDE):** Moto G100 = **192.168.31.49:5555**
(`adb connect 192.168.31.49:5555`; USB cai em transfer grande, REDE é estável). Jogo ORIGINAL
**instalado e rodando** (`com.wb.goog.lbbg/.GameActivity`, root via `su -c`). NextOS instalou.
- **Fluxo correto (do celular):** barra de carregamento → **disclaimer** (texto branco/PRETO,
  limpo: "LEGO BATMAN BEYOND GOTHAM © 2022 TT Games... WB") → **logos** → **título com fundo
  3D animado** (cena espacial: Terra + watchtower + nebulosa roxa + sol; `frame diff 3.09` =
  anima; `MOVIEDEBUG surface=null` no título = é 3D real-time, NÃO vídeo) + "Toque para começar".
- 🎬 **SISTEMA DE VÍDEO da engine** (o "fundo é vídeo" do NextOS = cutscenes/logos, não o título):
  Java `GameVideoPlayer` + `mMediaPlayer` (android.media.MediaPlayer) + **"video layout"**
  (Surface Android atrás do GL, compositada pelo Android). Funções nativas: `JavaCallback_playMovie`
  (0x3c6bdc), `fnaFMV_Open/Update/Close` (0x3c7058...), `Java_..._nativeUpdateMovieInfo` (0x239568
  → `fnaFMV_SetMovieInfo`), `fnaPrimitive_SetVideoTextureRendering` (0x3d3c5c). Logcat: `video
  layout attached to ViewRootImpl`, `OH DEAR!!! No surface available for mMediaPlayer.setSurface()`.
  - **36 .mp4 em data01** = `cutscenes/lvl_NN_*_intro/outro.mp4` (cutscenes de NÍVEL). O nosso
    so-loader NÃO tem MediaPlayer/video-layout → cutscenes não tocam (fundo de cutscene vazio).

### 🎯 PRÓXIMOS (em ordem)
- [ ] **Fundo 3D do título/menu (cena espacial) NÃO renderiza no nosso port** — só aparece o
      UI/logo sobre escuro. A engine deveria desenhar Terra/watchtower 3D. Investigar: cena vai
      pra FBO1 (960x540, 75%) e o composite/shader falha? assets 3D não carregam? Comparar GL
      do celular vs nosso. **É o maior gap visual restante.**
- [ ] Navegação: tap não avança de "Touch to start" → menu (mesmo problema da s3; agora com
      render limpo fica mais fácil de debugar). Front-end é touch-only; shim controle→touch =
      `LBBG_PADTAP`.
- [ ] Vídeo das cutscenes: decodificar mp4 (ffmpeg/cross-compile glibc) → textura GL → desenhar
      como fundo no hook do glClear (camada de vídeo própria). OU pelo menos preto (sem lixo).
- [ ] Empacotar launcher + commit master.

## ⚠️ s3 ADENDO (2026-06-26) — NEXTOS VIU NA TELA REAL: FLICKER + FLUXO NÃO COMPLETA

🚨 **MEUS DUMPS DE /dev/fb0 ESTAVAM ENGANANDO.** Eles leem a metade de baixo do fb e
pegam frames COMPLETOS, então pareciam menu/título limpos. **Na tela real (NextOS olhando)
o que acontece é:**
- abre um **disclaimer** + um **botão gigante atrás** (o spinner "X") **pegando a tela toda,
  os dois brigam pelo fb e fica PISCANDO** (flicker).
- depois o disclaimer some e o **botão gigante fica como LIXO atrás**.
- **NÃO aparece "New Game"** de verdade; **faltam logos / imagens de fundo**.
- NextOS: *"o fluxo original não está sendo completado, faltam ponteiros e estamos pulando
  o fluxo original."* → **essa é a direção certa do próximo trabalho.**

🔎 **Diagnóstico do present/buffers (fb = 1280x1440 = 2 metades de 720):**
- DEFAULT = single-buffer (`DOUBLEBUFFER=0`) + render na metade de baixo + `pan_keeper`
  força `yoffset=720` + present no hook `glClear(FBO0)→egl_shim_present` (SwapWindow+pan).
  → single-buffer = engine **redesenha a metade VISÍVEL todo frame** → display escaneia
  no meio do redraw → **tearing/flicker** (disclaimer sobre spinner).
- `LBBG_DBLBUF=1` (double-buffer nativo, SDL dá o flip): **pior** — `pan` vai pra `0,0`
  (metade de CIMA = preta) enquanto o conteúdo cai na de baixo → tela PRETA. SwapWindow
  pana pra metade errada nessa stack Mali.
- `LBBG_NOSWAP=1` (glFinish + pan, sem SwapWindow): **ainda piscou** (NextOS confirmou).
- ⚠️ Hipótese forte ainda NÃO testada a fundo: o meu **hook que força `glClear` full-screen
  no FBO 0** (imports.c, adicionado na s2 p/ tirar "lixo") pode estar **apagando a camada de
  FUNDO** que a engine compôs → fundos somem + camadas brigam. **CHECAR glClear_wrap
  (imports.c ~linha 1026): só limpar 1x por frame / não limpar por cima do fundo.**
- 🎯 Próx. ideia de present: render p/ FBO próprio (offscreen) e **blit atômico** p/ a metade
  visível 1x por frame (sem tearing), OU descobrir por que o conteúdo cai na metade de baixo
  e fazer cair na de cima (yoffset=0 sempre visível, sem pan).

📱 **PLANO QUE O NEXTOS PEDIU: rodar o APK ORIGINAL no Moto G100 e comparar o fluxo.**
- Moto G100 (`adb` id `0074124494`), **arm64-v8a, Android 12, ROOT via Magisk** (`su -c` → uid 0;
  `adb root` NÃO, é production build). É a mesma bancada de outros ports.
- APK = `~/Downloads/LEGO-Batman-Beyond-Gotham-v2.2.1.05-full-apkvision.apk` (1.6GB, **APK único
  instalável**, sem OBB/splits; ZIP store).
- 🔴 **USB INSTÁVEL DEMAIS PRO TRANSFER (1.6GB):** `adb push` dá "failed to read copy response"
  + velocidade falsa (1672 MB/s) e derruba a conexão (device→offline); chunks tб caem; **MTP
  tб falha** ("não conseguiu abrir 002,126", re-enumera). Comandos pequenos de `adb shell`
  funcionam; só o transfer grande quebra.
- ✅ **WORKAROUND que estava montando:** celular baixa o APK pelo **WiFi** (não passa pelo USB):
  HTTP server no PC (`cd srv && python3 -m http.server 8000`, host IP **192.168.31.218**,
  link `http://192.168.31.218:8000/lbbg.apk`) → abrir no navegador do celular OU
  `adb shell` wget/curl backgrounded → depois `adb shell su -c 'pm install /sdcard/Download/lbbg.apk'`
  (comando pequeno, funciona). **NextOS ia mandar o arquivo ele mesmo / pausou aqui.**
- 🎯 **OBJETIVO no celular:** rodar o original, capturar **logcat** + screenshots do fluxo
  (disclaimer→logos→fundos→menu→New Game) e ver QUAIS chamadas/ponteiros a engine faz que
  nós pulamos no so-loader (qual setup de render/EGL/FBO/asset o nosso boot não replica).

## ✅ s3 (2026-06-26) — TÍTULO + MENU "NEW GAME" RENDERIZAM EM INGLÊS, TOUCH FUNCIONA

Boot completo → splash dev (engrenagem+raio) → **"LEGO BATMAN — BEYOND GOTHAM" + "Touch to start"**
→ **menu front-end com play-button + "New Game"** (inglês), tudo renderizando, 0 crash.
Input de **toque FUNCIONA** (taps avançam as telas do título). Imagens em `scratchpad/d1.png`
(título), `fb5/f2/g3.png` (menu New Game claro/escuro), `h2.png` ("Touch to start").

### 🔑 Achados desta sessão (na ordem)
1. **Shader compile no 1º boot é LENTO** (cache frio): engine pede `shaderbin.fib` (MISS) →
   compila shaders em runtime e cacheia em `save/shaderbinaries/<hash>/*.glprog` (52 progs).
   2º boot (cache quente) chega no menu em ~70s. Spinner "X num círculo" = LOADING, não trava.
2. **Não há tap-gate de verdade** no boot quente — vai direto pro título sozinho.
   A sequência de atração faz LOOP: splash(engrenagem) ↔ título("Touch to start").
   O menu New Game é alcançado por taps (rápidos, estilo AUTOTAP), não está no loop de atração.
3. 🔑 **ABI do TOUCH (confirmado por disasm)**: `Java_..._nativeTouchEventDown` é um thunk de 12B:
   `w1=phase(1=down,-1=up,0=move)`, `w0=w2(id)`, floats x/y/p passam direto em **s0,s1,s2**.
   → `fnaController_AndroidNativeTouch_SetData(int id, float x, float y, float p, int phase)`.
   Nossa chamada `nativeTouchDown(env,obj, 0, x, y, p)` mapeia certo (id=0, x=s0, y=s1).
   - `nativeTouchEventGestureStart(id,x,y)` = **ResetData() + SetData(down)** (começa o gesto).
   - `nativeTouchEventGestureEnd(id,x,y)` = **ReleaseAllTouches** (levanta o dedo).
   - O "clique" no botão é detectado pelo **bit "released" (w10=4)** que só `SetData(phase<0)`
     (TouchUp) seta — `ReleaseAll` pode não setar. Tap atual = GestureStart→TouchUp→GestureEnd.
4. 🔑 **Coords de toque** = espaço da tela 1280x720 (passamos isso no nativeResize).
   Play-button centro ≈ **(390,360)**; "New Game" texto à direita.
5. 🔴 **CONTROLE FÍSICO NÃO FUNCIONA NO MENU (= bloqueador de jogabilidade no handheld)**:
   `fnaController_ProcessJoypadController(dev)` lê nosso mask (global 0xc3b000+152) e só faz
   early-out se `ctrlId==-1` (passamos 0, OK). MAS ele só roda se a engine o chamar com um
   `fnINPUTDEVICE*` real, e **`fnaController_Init` NÃO cria device de joypad** (só cria critical
   section + ResetData de touch). **Não existe JNI native de "controller added/connected"**.
   → Nenhum device de joypad é registrado → ProcessJoypadController nunca roda → **bits do
   controle não fazem nada no front-end** (bit-sweep 0..13 ×2 não disparou nada). O front-end
   é **touch-only** como está. NextOS precisa de controle → PRÓXIMO PASSO PRINCIPAL.

### 🎯 PRÓXIMOS PASSOS (em ordem de valor)
- [ ] **REGISTRAR UM DEVICE DE JOYPAD** p/ o controle físico dirigir o menu (e o jogo):
      investigar `fnaController_CreateDevice(fnINPUTDEVICE*)` (0x3c5344) e o loop de update de
      input da engine (quem chama ProcessJoypadController via vtable do device). Criar/registrar
      um device no init e rotear `nativeControllerSetData` por ele. SEM isso o port não é jogável
      no handheld (sem touchscreen). Mapa de bits→slot já extraído (offsets na função, ver disasm).
- [ ] Confirmar START de New Game: com touch, um tap LIMPO e ISOLADO no play-button (390,360)
      sem re-tap (a oscilação dos taps periódicos cancelava o load; shaders de nível
      `light=prelit/postfx/outline` JÁ chegaram a compilar = nível começava a carregar).
- [ ] Som (opensles cria players mas nunca dá Enqueue — investigar thread de áudio).
- [ ] Empacotar launcher PortMaster (`LEGO Batman 3.sh` existe) + commit master.

### Flags de env (main.c)
- `LBBG_DATADIR` / `LBBG_SAVEDIR` — dirs de assets/save.
- `LBBG_AUTOTAP=1` — taps de gesto periódicos no play-button (oscila). `LBBG_TAPX/Y` alvo,
  `LBBG_TAPEVERY=N` cadência em frames (default 90; 300 = mais lento).
- `LBBG_FILETAP=1` — tap ONE-SHOT: `echo "x y" > /tmp/lbbg_tap` no device dispara 1 tap (limpo).
- `LBBG_BTNSWEEP=1` — varre bits 0..13 do controle 8s cada (marca em /tmp/lbbg_sweep). (Provou
  que controle não chega no front-end — ver achado #5.)

### Como rodar (sem brigar com ES)
`systemctl stop emustation` ANTES de rodar por ssh. Kill: `for p in /proc/[0-9]*; do ... *legobatman*) kill -9`.
Captura: `dd if=/dev/fb0 of=/tmp/fb.raw bs=1M` (fb 1280x**1440**, mostrar metade de baixo [720:1440], BGRA→RGB).
`systemctl start emustation` ao terminar.

(Histórico s1/s2 = IMAGEM/logos, menu parcial, fixes de canary/OnceInit/som-loop/present-pan/shaders —
ver `project_legobatman_mali450.md`.)
