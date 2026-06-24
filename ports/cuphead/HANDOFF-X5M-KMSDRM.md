# Cuphead — backend KMSDRM p/ o X5M (fbdev mantido) — 2026-06-13

## Missão
Rodar o port no **s905x5m** (`<device-ip>`). Hoje só abre em **mali fbdev**.
Queremos **kmsdrm também** no X5M, como fallback/alternativa, **mantendo o fbdev** do
Amlogic-old (Mali-450). "manteremos o fbdev mas vamos ter o kmsdrm também".

## Diagnóstico (por que não subia no X5M)
- O Cuphead (Unity) renderiza no **EGL REAL do Mali via fbdev** (`/dev/fb0`, `g_fbdev_win`,
  main.c ~532). Os `egl*` da libunity são imports resolvidos pelo `so_resolve` (main.c 2460)
  via `dlsym(RTLD_DEFAULT)` → **libEGL REAL** (dlopen GLOBAL em 2401-2402).
- O **X5M é Mali-G310 Valhall / KMSDRM — NÃO tem EGL fbdev** → o caminho default não inicializa.
- O `CUP_SHIMEGL=1` antigo criava a janela SDL **mas não re-roteava** os `egl*` da Unity (que
  seguiam no libEGL real) → "a Unity não usa o shim". **Esse era o elo que faltava.**
- O X5M (DEVICE=**Amlogic-no**) já tem **SDL3 STOCK upstream com KMSDRM+Wayland** (override
  device em `projects/Amlogic-ce/devices/Amlogic-no/packages/multimedia/SDL3/package.mk`,
  SDL_MALI=OFF SDL_KMSDRM=ON SDL_WAYLAND=ON; gbm via opengl-meson/Valhall). **Não precisa
  recompilar SDL** (a menos que a imagem instalada seja anterior a 2026-05-26 — VERIFICAR no device).

## Mudança feita (main.c) — device-aware, fbdev intacto
1. `cup_use_kmsdrm()` (novo): decide 1×. `CUP_VIDEO=kmsdrm|fbdev` força; `CUP_SHIMEGL` legado=kmsdrm;
   **auto: existe `/dev/dri/card0` → kmsdrm; senão fbdev**. (Amlogic-old não tem /dev/dri → fbdev.)
2. `my_aw_fromSurface`: kmsdrm → ANativeWindow fake (egl_shim ignora a window); fbdev → struct {w,h} real.
3. Bloco F2: em kmsdrm → `SDL_Init(VIDEO|AUDIO)`, default `SDL_VIDEODRIVER=kmsdrm`, `egl_shim_create_window()`,
   **`egl_patch_unity_got()`** (NOVO: `so_patch_got` re-rota TODOS os egl* da libunity → egl_shim) e
   `egl_shim_ensure_current()`. fbdev → inalterado (EGL real Mali, SDL só áudio).
4. `egl_patch_unity_got()` (novo, perto do `egl_route`): patcha 22 símbolos egl* reusando o mapa do egl_route.

Binário rebuildado (build.sh, toolchain Amlogic-old). O **mesmo binário roda nos dois devices**
(linka -lSDL2 dinâmico; auto-detecta). Strings novas confirmadas: `[F2] kmsdrm: %d slots egl* ...`.

## Launcher: `run.sh` (novo, device-aware)
- Mata instância anterior (loop pgrep). Env comum do jogo (config s10).
- `/dev/dri/card0` existe → **KMSDRM**: para frontend (emustation/weston) p/ liberar DRM master,
  `CUP_VIDEO=kmsdrm`, `SDL_VIDEODRIVER=kmsdrm`, `LD_LIBRARY_PATH=/usr/lib` (SDL3 stock do sistema).
- senão → **fbdev**: `CUP_VIDEO=fbdev`, `SDL_VIDEODRIVER=mali` (igual go.sh do Amlogic-old).
- `go.sh` (Amlogic-old) permanece como referência fbdev.

## ✅✅ VALIDADO NO X5M (.103, senha <senha>) 2026-06-13 — KMSDRM FUNCIONA
O X5M é **.103** (hostname Amlogic-no, /dev/dri/card0+renderD128, /dev/fb0). SDL2 NATIVO
2.32.10 do device JÁ tem **KMSDRM+Wayland** (sem driver mali) → não precisou recompilar SDL.
Stack Valhall: libmali/libgbm/libdrm.so.2/libEGL/libGLESv2. RAM 3.6G. Frontend=**essway.service**
(segura card0 → run.sh para). Deploy em /storage/roms/cuphead-recon (575M; assets do APK
~/Downloads/Cuphead-DLC-v1.0.2-full-apkvision.apk + libil2cpp/libunity do APK lib/arm64-v8a).
Helper: /tmp/x5m.sh (ssh) + /tmp/cupdeploy.sh (build+deploy+run stdbuf).

**O QUE JÁ FUNCIONA (boot via kmsdrm, em ordem):**
1. cup_use_kmsdrm()=1 (auto /dev/dri). SDL_Init kmsdrm OK; janela 1280x720; **contexto ES3.0 no
   Valhall** (egl_shim pede ES3, cai p/ ES2). egl_patch_unity_got: 22 egl* da Unity → egl_shim.
2. Passa o **check de GLES3** ("hardware requirements"): (a) getBoolean("gles-api-check")→1 via flag
   do NewStringUTF (jni_shim) pula o AlertDialog; (b) **DELEGAÇÃO ao EGL real** — egl_shim captura
   o EGLDisplay real do SDL (eglGetCurrentDisplay) + escolhe EGLConfig ES3 real (eglChooseConfig)
   e delega eglGetConfigAttrib → Unity vê config ES3 verdadeira e ACEITA (sem isso: SIGTRAP).
3. **Multi-contexto threaded do Unity RESOLVIDO** (era EGL_BAD_ACCESS): MakeCurrent reescrito p/
   EGL real — a 1ª thread a pedir a window vira DONA (renderiza na g_win_surf real); as demais
   threads recebem um **contexto real próprio** (eglCreateContext compartilhando o share-root) num
   **pbuffer real** → recursos compartilhados, zero BAD_ACCESS. Ambas as threads: REAL OK.
4. Boot segue: il2cpp init_array, JNI_OnLoad, initJni, device-info, **serializa settings JSON**
   (screenWidth 1280 etc), Build.*, getPackageManager.

## ⛔ BLOQUEIO ATUAL (2026-06-13): HANG na APP-INIT (analytics/device-info), NÃO no GL
Trava em ~417-449 linhas do run.out: último JNI = loop getInstallerPackageName
("com.teamcherry.hollowknight") + getPackageInfo + Build.TAGS/MODEL/DEVICE (NULL). Main thread
em **hrtimer_nanosleep** (spin-wait), GfxDeviceWorker ocioso (futex_wait), demais workers ociosos.
NÃO é timeout de rede (45s sem progresso). É a init de telemetria/anti-pirataria do jogo.
- **Provável raiz**: getInstallerPackageName retorna "" (esperado "com.android.vending"?) e/ou
  getPackageInfo retorna objeto inválido → check de loja/anti-tamper trava. jni_shim NÃO tem
  handler de getPackageInfo/getInstallerPackageName (cai no fallback genérico).
- ⚠️ Em tese o **fbdev (s10) chegava ao título com OS MESMOS flags** → ou esse hang é
  kmsdrm/timing-específico OU a app-init mudou. Próximo passo decisivo abaixo.

## 🔄 PROGRESSO DO BOOT (sessão 18c, 2026-06-13) — 7 fixes empíricos, avançou ~417→443
Cada fix no jni_shim destravou uma etapa da init de app/analytics (o KMSDRM já estava OK):
1. `getBoolean("gles-api-check")→1` (flag NewStringUTF) — pula AlertDialog GLES.
2. `getInstallerPackageName→"com.android.vending"` — anti-pirataria (Play Store) parava de loopar.
3. `Build.*` (MODEL/DEVICE/RELEASE/ID/INCREMENTAL/TAGS/FINGERPRINT/...) → strings válidas
   (eram "") — crash-reporter/analytics coleta device fingerprint.
4. `GetFloatField` (idx 102) p/ DisplayMetrics density/xdpi/ydpi/scaledDensity (não existia).
5. log em CallVoidMethod (diagnóstico).
6. `diag_handler` SIGUSR1 (stack-scan) + blindagem no my_sigaction — MAS Unity MASCARA SIGUSR1
   em todas as threads → sinal NÃO é entregue. Backtrace por sinal IMPOSSÍVEL (idem SIGTRAP/SIGSYS:
   o crash-reporter do jogo hooka via syscall direta).
7. (binário md5 17a490bfea88cee582b26a0e4ce6360d)

## ⛔ BLOQUEIO ATUAL: HANG na init da UNITY ANALYTICS (poll-wait), ~431-443 linhas
É **UnityEngine.Analytics / Unity Services**: log mostra `unity.player_session_count`,
`unity.player_sessionid`, `Settings.Secure.ANDROID_ID`, `unity.cloud_userid`, checkPermission
INTERNET, **advertising-id GMS** (bindService com.google.android.gms.ads.identifier), e termina
em `getMetrics` x3 + main thread em hrtimer_nanosleep (poll). GfxDeviceWorker ocioso.
- **Hipótese principal**: o `runOnUiThread(runnable)` da analytics (linha ~398) — o Unity posta um
  Runnable (provável fetch do advertising-id / session-start) e faz poll esperando ele rodar, mas
  o jni_shim **NÃO executa Runnables** (CallVoidMethod(runOnUiThread)=no-op) → espera infinita.
- **Diagnóstico travado**: sinais mascarados → sem backtrace. Não dá p/ confirmar 100% sem RE.

## ✅ SESSÃO 18c — Runnables RODANDO (jnibridge invoke) + descoberta das variantes V
- **il2cpp usa as variantes V** (Call*MethodV: args via va_list). As funções roteavam os índices V
  (38/62/115) pras versões varargs → handle/args lidos como LIXO. FIX: refatorei CallVoidMethod/
  CallBooleanMethod/CallStaticObjectMethod em V+wrapper e roteei os índices V. +strdup do nome no
  RegisterNatives. Agora o **Runnable do runOnUiThread/post RODA E COMPLETA** (invoke do jnibridge,
  handle correto). Mas o boot AINDA trava no **getMetrics** (init C#, antes do nativeRender) — não
  é os Runnables nem JNI-completeness. CUP_NORUNUI desliga a exec. Binário md5 a7fa4e3b...
- ▶️ Precisa de backtrace SEM sinal (mascarados) p/ achar o wait do getMetrics, ou RE do bootstrap.

## ▶️ PRÓXIMO PASSO (2 caminhos — históricos)
A) **Executar os Runnables postados** (provável fix): implementar o invoke do jnibridge —
   (1) capturar o handle no `newInterfaceProxy` (1º arg jlong) → registrar proxy→handle;
   (2) em `runOnUiThread`/`post`/`postDelayed`, achar o handle do Runnable e chamar o native
   `invoke` (jni_find_native("invoke"), sig `(JLClass;LMethod;[LObject;)LObject;`) SÍNCRONO na
   própria thread, com um Method fake cujo getName()→"run" e args[]=vazio. Risco: re-entra il2cpp.
B) **Desligar a Unity Analytics**: fazer checkCallingOrSelfPermission(INTERNET)→DENIED(-1) (flag
   NewStringUTF), p/ o Unity pular o fluxo de rede/advertising. Mais simples, pode não bastar.
+ Comparar com fbdev (.164) p/ confirmar se é geral (provável: mesmo jni_shim).

## ▶️ PRÓXIMO PASSO ORIGINAL (continuar o boot)
1. **Backtrace do hang**: rodar e mandar SIGTRAP no PID (on_crash imprime pc=libunity+0x...) p/
   achar a função que faz o spin-wait. (on_crash cobre SIGTRAP; vê o dump no run.out.)
2. Tentar handlers no jni_shim: getInstallerPackageName→"com.android.vending"; getPackageInfo→
   objeto fake com versionCode/versionName; ver se destrava.
3. Comparar com fbdev: deployar o MESMO binário no Amlogic-old (.164, CUP_FORCE_FBDEV=1) e ver se
   trava no mesmo ponto — isola se é kmsdrm-específico ou boot geral.
4. Se passar: deve chegar a nativeRender → SwapWindow (1º frame na TV via kmsdrm).

## Mudanças no código (resumo)
- main.c: cup_use_kmsdrm(), egl_patch_unity_got(), bloco F2 kmsdrm.
- jni_shim.c: g_gles_warn_skip (flag NewStringUTF→getBoolean) pula o aviso GLES.
- egl_shim.c: contexto ES3+fallback; captura EGLDisplay/Config/Surface reais; pbuffer worker;
  GetConfigAttrib delega ao EGL real; MakeCurrent EGL-real com dono-da-window por-thread.
- run.sh, go.sh (fbdev). Binário md5 atual: 2185aaae54e8f4fc9633b0833ddda513.

## (histórico) X5M parecia OFFLINE no começo da sessão
- .165 sem rota (ARP FAILED, ping 100% perdido). Varredura da /24 não achou NextOS aarch64 com
  /dev/dri. Único NextOS online = Amlogic-old (.164/.89/.103/.190, MAC da:da:21:51:75:a9, SÓ fbdev).
- **Não dá p/ validar kmsdrm sem o X5M ligado.** Falta SÓ a tarefa 5 (deploy+validar).

## ▶️ PRÓXIMO PASSO (quando o X5M ligar)
1. SSH no X5M (IP atual; DHCP pode ter mudado). Conferir: `ls /dev/dri`, `cat /etc/os-release`,
   e **qual SDL3 está instalado** — `SDL_VIDEODRIVER=kmsdrm` num teste mínimo, ou ver se a libSDL3
   tem o driver kmsdrm (a imagem pode ser < 2026-05-26 = SDL3 antigo mali-fbdev → aí SIM recompilar
   o SDL3 stock do override Amlogic-no e deployar).
2. Deploy: `cuphead` (novo) + `run.sh` em /storage/roms/cuphead-recon/ + assets (receita HANDOFF-SESSAO17:
   libil2cpp.so, bin/Data/, global-metadata.dat; saves /storage/cuphead-sa).
3. `./run.sh` → ler run.out: `[F2] kmsdrm: SDL video driver = kmsdrm`, `N slots egl* -> egl_shim`,
   `janela GLES2 criada (egl_shim/SDL3 kmsdrm)`. Conferir presença na TV.
4. Se kmsdrm não pegar DRM master: ajustar o stop do frontend (nomes reais de serviço). Se SDL não
   achar driver kmsdrm: a libSDL3 do device é antiga → recompilar (ver passo 1).
5. Riscos: contexto GL compartilhado entre libGLESv2 real (Valhall) e o EGL do SDL3 — modelo
   proven re4, mas validar SwapBuffers. Input via CUP_GAMEPAD (js0) já funciona.

## Arquivos
- main.c: `cup_use_kmsdrm`, `egl_patch_unity_got`, bloco F2 (busque "kmsdrm" / "ELO").
- run.sh (novo), go.sh (fbdev, inalterado). Binário: ports/cuphead/cuphead.

## 🔬 SESSÃO 18d — DIAGNÓSTICO via ptrace: livelock no threaded-render do Unity
**Ferramenta nova: `tools-bt.c`** (backtrace por ptrace/frame-pointer; X5M tem ptrace livre + strace,
mas SEM gdb). Cross-compila dinâmico (toolchain Amlogic-old), deploy /tmp/bt, `bt <tid>`.
Mapear: addr - load_base (libunity/il2cpp do log "so_load: load base").

**ACHADO (definitivo):** o jogo CHEGA ao **`nativeRender`** (render loop começou!) e trava num
**livelock do threaded-render do Unity**:
- MAIN thread: em `nativeRender` (libunity+0x35C40C) → 0x35948C → 0x738C60 → 0x738334 →
  **spin-wait 0x6E9558** (contador até 999 + deltas de tempo fdiv/fadd; "espera a render thread
  terminar o frame"). strace: gettid + futex_WAKE em loop (sinaliza o worker).
- GfxDeviceWorker: em libunity+0x5A55D8 ← 0x5ACEF4 ← 0x5A57B4 ← 0x8F61A8 ← 0x26219C →
  **pthread_cond_wait REAL do glibc** (FUTEX_WAIT_BITSET|CLOCK_REALTIME) "espera trabalho".
- **Nenhum GL acontece** (0 shaders/draws/swap). Backtraces IDÊNTICOS entre amostras = deadlock fixo.
- = main espera o worker terminar o frame; worker espera um comando/sinal da main; circular. NÃO é
  bug de primitiva (glibc cond correto) — é a LÓGICA do handshake threaded.
- **Fix tentado (correto mas não destravou): dono da window = GfxDeviceWorker** (thread não-main),
  main→pbuffer (egl_shim, gateado por tid==pid; CUP_MAINGL força main na window). Antes a main pegava
  a window e o worker o pbuffer (worker não apresentava). Agora worker tem a window, mas o livelock
  é ANTES do GL, então não resolveu sozinho.

## ▶️ PRÓXIMO PASSO (caminhos concretos)
A) **Forçar single-threaded rendering** (sem GfxDeviceWorker) → nativeRender roda na main (que tem
   contexto) e apresenta direto, sem handshake. Achar como o Unity 2017.4 decide criar a render thread
   (player setting/runtime check) e desligar.
B) **RE do handshake**: por que a submissão de comando da main não acorda/satisfaz o worker. Desmontar
   0x738334 (submit+wait) e 0x5A55D8 (worker "tem trabalho?") — achar o flag/queue não-satisfeito.
C) Comparar com fbdev (.164): se o fbdev (real Mali EGL) passa o MESMO threaded-render, o problema é
   a interação egl_shim×worker; se trava igual, é geral. (Memória: CUP_GCOFF/sem_shim/"GC cooperativo".)
