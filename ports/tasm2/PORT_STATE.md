# TASM2 port state

Saved: 2026-07-04 11:40:02 -03
Device: 192.168.31.90
Port dir: /home/felipe/nextos_ports_android/ports/tasm2
Target dir on device: /storage/roms/ports/tasm2

## Update 2026-07-06 (sessao NextOS): loop GAIA quebrado

- Device migrou de .90 para 192.168.31.92 (EMUELEC, DHCP).
- Confirmado com JNI_DEBUG que NAO e bug de GL/black-screen: o jogo chega ao
  loop principal e roda a ~22fps chamando `sGetMilliseconds()` sem parar.
- RAIZ do "logo Marvel -> preto": loop infinito de auth GAIA. O engine repetia
  `isSharedValue`/`getSharedValue` p/ FIRST_LAUNCH/ANON_GLUID/ENC_KEY_GLUID e
  emitia `[HEI] 7000/7001/8001/20001` sem fim, spawnando threads (0xcbce34),
  nunca avancando p/ welcome/video/menu.
- O estado GAIA fica em `[r4, #28]` (0x1c): 0=pendente, 1=ok(log 20000),
  2=falha(log 20001). O engine ficava preso em 2.
- FIX aplicado no run.sh: `TASM2_FORCE_GAIA_STATUS_PATCH=1` (forca [r4+28]=1 no
  site 0x00c8e340, cai no ramo 20000) + `TASM2_BLOCK_GAIA_THREADS=1`.
  Resultado: HEI cai de centenas p/ ~12 e vira 20000 (sucesso), loop PARA,
  jogo estabiliza a 22fps, boot loop termina limpo (GL2JNI boot -> 0).
- PORE'M o frame visivel continua o clear roxo (127,127,255) de tela cheia
  (capturas t2 8s/14s/20s identicas, area visivel 100% roxa). Ou seja: o muro
  do "frame roxo" e SEPARADO do loop GAIA e continua de pe.
- Metodos welcome/video seguem registrados mas NUNCA chamados mesmo pos-fix:
  `sWelcomeScreenLaunch`, `sLaunchVideoPlayer`, `playVideo`, `stopVideo`.
  Proximo alvo: por que o engine nao dispara welcome/intro apos auth OK.
- Nota: o APK mod (`...GloftASHMmod_1.2.8d`) esta empacotado com Qihoo 360 jiagu
  (StubApp/libjiagu) — classes.dex real encriptado, decompile Java nao ajuda.

## Update 2026-07-06 (cont.): muro do frame roxo caracterizado a fundo

Trace GL (TASM2_GL_DEBUG=1) em steady-state prova o mecanismo do frame roxo:
- Todo frame o jogo faz `glClear(mask=0x100)` = SO' DEPTH (nao COLOR). O
  127,127,255 e um clear-color antigo que fica FIXO de fundo. So desenha 1 quad
  (glDrawElements count=6, program=3, tex2d[0]=22) que e INVISIVEL (nao muda o
  pixel central; area visivel 1280x720 = 100% roxa, 0 pixel diferente).
  => Cena vazia: o engine esta num estado pre-welcome sem UI construida.

Sequencia real de boot (JNI trace):
- EndSplash -> InitGameAPI(ZZZ) chamado 1x -> jogo ESPERA.
- `ConnectToService()V` e registrado (GetStaticMethodID) mas NUNCA chamado.
- `sWelcomeScreenLaunch(I)V`, `sLaunchVideoPlayer`, `playVideo` idem: so'
  registrados, nunca chamados. O engine trava ANTES de welcome/intro.

No loop idle o main thread so' chama repetidamente `sGetMilliseconds` (timing) e
`JGetFreeDiskSpace` (~38x/janela) — padrao de estado "download/instalacao de
conteudo online" esperando. O gate do welcome e ESTADO INTERNO NATIVO setado por
um worker que completa a conexao/sync online — que sem servidores Gameloft
(mortos ~2018) nunca liga. NAO ha lever visivel por JNI que destrave.

Levers testados que NAO destravam (todos deixam o frame roxo identico):
- bloquear vs deixar vivo o worker GAIA 0xcbce34: sem diferenca.
- Callbacks GameAPI RE-ENTRANTES (TASM2_GAMEAPI_NATIVE_CALLBACKS dentro de
  InitGameAPI): TRAVAM o jogo (deadlock de lock — nativeInit re-entrante).
- Callbacks GameAPI DIFERIDOS (novo TASM2_GAMEAPI_COMPLETE_FRAME=N no main.c:
  dispara nativeGameAPINotifyAuthChanges+nativeGameAPIComplete fora da chamada,
  no frame N): disparam LIMPO, sem crash, mas o jogo IGNORA — nao chama
  ConnectToService nem welcome. => nao e esse o gate.

Infra nova adicionada (main.c, env-gated, OFF por padrao, inofensiva):
- `TASM2_GAMEAPI_COMPLETE_FRAME=N` (+ opcionais `_COMPLETE_PLATFORM_INIT`,
  `_COMPLETE_INIT`, `TASM2_GAMEAPI_ACCOUNT`). Modelo async correto (nao
  re-entrante), pronto pra reuso se um dia acharmos o callback certo.

## Proximo passo REAL (nao e env-tweak — precisa RE nativo)

O muro e a transicao de estado nativa que gate-ia o welcome na flag interna de
"conexao/sync completa". Para quebrar: usar Ghidra/radare, achar a funcao de
state-machine do boot (perto do render 0x692f68 e dos leitores de estado GAIA
0xc8exxx), localizar o branch que testa a flag "services ready"/"connected" e
patchar pra sempre-tomado (como ja foi feito o sentinel/obf patch em main.c).
Referencia: o mesmo tipo de muro do PES2012 (dependencia Google Play/online).
Alternativa: descobrir por que o MOD `drandroid` (que bypassa online no Android)
nao ajuda aqui — mas o dex esta sob Qihoo jiagu.

## Update 2026-07-06 (cont.2): frente OFFLINE investigada (estudo COD BOZ)

Estudo do COD BOZ (Marmalade s3e): o download in-app e' contornado PRE-baixando os
`.dz` fora do jogo (codboz_setup) -> game acha cache local e pula a rede. Licao:
pre-encenar "nao preciso de rede". Diferenca crucial: COD BOZ trava por ARQUIVO
(pre-stageavel); TASM2 trava por HANDSHAKE DE SERVIDOR (nao pre-stageavel).

O binario TEM modo offline (strings `federation_offline`, `offline_store`,
`OfflineJson`, "Offline Store is already initialized"). Tentei forcar offline:

1. `TASM2_CONNECTION_TYPE=0` (initCheckConnectionType()->0 = sem rede): SEM efeito,
   frame roxo identico. A decisao online/offline NAO passa por esse retorno JNI.
2. Callers do idle-loop (via TASM2_JNI_CALLER_DEBUG):
   - JGetFreeDiskSpace <- 0xa71594 (thunk JNI off 0x220=CallStaticFloat)
   - sGetMilliseconds  <- 0xa7155c (thunk JNI off 0x214=CallStaticLong)
   - Ambos sao WRAPPERS; os callers reais (0xa715xx-0xa73xxx, ~20 sites) sao o
     coletor de DEVICE-SPECS/telemetria GLOT ([Game Options] Device Specs/FPS
     Report) — NAO e' monitor de download nem o gate. Poll de disco = telemetria.
3. `sGetMilliseconds` retorna CLOCK_MONOTONIC real (avanca certo) — nao e' timer
   congelado (descartada a hipotese tipo FF9 Time.time).

Conclusao da frente offline: o gate do welcome e' estado NATIVO interno, sem
footprint JNI hookavel. `initCheckConnectionType`, GAIA status, GameAPI complete,
connection-type, first-launch — nada disso destrava. O bypass offline real do MOD
`drandroid` vive na camada JAVA (classes GAIA/GameAPI modificadas) que nos
substituimos por jni_shim — mas o dex do MOD esta sob Qihoo jiagu (encriptado),
entao nao da pra ler qual stub especifico ele aplica.

## Caminhos REAIS restantes (ambos = trabalho pesado, nao env-tweak)

A) RE nativo com Ghidra/radare: decompilar a state-machine de boot (perto do
   render 0x692f68), achar o branch que gate-ia welcome na flag "services
   ready/connected" e patchar pra sempre-tomado (como sentinel/obf patch).
B) Unpack do Qihoo jiagu: rodar o APK MOD num Android/emulador e dumpar o dex
   descriptografado em memoria (StubApp decifra em runtime) -> ler a logica
   offline do MOD e replicar no jni_shim.

Static xref de string e' INVIAVEL neste binario (strings referenciadas por indice
de tabela, nao endereco direto — testado movw/movt e literal-pool, 0 hits).

## Update 2026-07-06 (cont.3): RE nativo com rizin — CONCLUSAO DEFINITIVA

Instalado rizin 0.8.2 + rz-ghidra (decompiler) no host Arch (sudo ok). Analise
completa (aaa + projeto salvo em rz_proj.rzdb). Descobertas:

- Engine C++ stripped, orientado a objeto (chamadas por vtable). `step()`
  (0xa74f3c) chama update via vtable de um singleton global (0x90332c pc-rel).
- Os method-IDs (sWelcomeScreenLaunch, sLaunchVideoPlayer, etc.) sao registrados
  EM MASSA em fcn.0xa75f9c (loop de GetStaticMethodID via vtable off 0x1c4),
  cacheados em slots de um objeto. O LAUNCH real do welcome e' chamada INDIRETA
  por mid cacheado, guardada por condicao de estado — NAO ha call site com a
  string. Xref de string so' leva ao registro, nunca ao gate.
- onVideoFinished (0xea264) e' NO-OP (bx lr). setConnectionType grava em global
  0x134e774. Poll de disco = telemetria device-specs. Nenhum e' o gate.
- Xref de string por literal-pool (add rD,pc,rD) tem falsos-positivos demais
  (janela larga) e o rizin nao gera data-xref pra refs computados — so' resolve
  no decompile por-funcao. Achar o gate = tracar a state-machine C++ inteira por
  vtables+mids cacheados: multi-hora, payoff incerto.

### DESCOBERTA DECISIVA — por que nada disso destrava
- `libtasm2.so` deployado == `libtasm2.so` do APK MOD (md5 9ca414a7... IGUAL).
- O APK MOD e' empacotado com **Qihoo 360 jiagu** (assets/libjiagu.so,
  JIAGU_APP_NAME). => o `.so` NATIVO e' STOCK Gameloft (gate online intacto).
- O "crack offline" do MOD `drandroid` esta na camada JAVA (classes GAIA/GameAPI
  modificadas), dentro do dex CRIPTOGRAFADO pelo Qihoo. Essa camada Java e'
  exatamente a que nosso jni_shim substitui — logo viemos tentando reproduzir
  as cegas o comportamento do MOD sem poder ler o que ele faz.

### Rota realista p/ destravar (unica com payoff provavel)
PATH B — unpack do Qihoo: rodar o APK MOD num Android/emulador (frida +
dump-dex / FRIDA-DEXDump / objection) p/ dumpar o classes.dex DESCRIPTOGRAFADO
em runtime, ler o que as classes GAIA/GameAPI modificadas do MOD fazem (que
valor/flag/stub forcam offline) e REPLICAR isso no jni_shim. Precisa de ambiente
Android (device/emulador), que nao existe neste host de dev Linux.

PATH A (RE nativo do gate) fica como fallback caro: rz_proj.rzdb ja tem a analise
pronta; retomar de fcn.0xa75f9c (registro) -> achar slot do mid de welcome ->
xref de quem le esse slot (o launch) -> a condicao que o guarda -> patchar.

Ferramenta nova disponivel no host: rizin+rz-ghidra (util p/ todos os ports).

## Update 2026-07-06 (cont.4): QIHOO DESEMPACOTADO — dex do MOD extraido!

PATH B executado com sucesso. Ambiente: Waydroid (Android 13 x86_64, houdini) no
host Arch. Passos que funcionaram:
1. `waydroid session start` + `waydroid show-full-ui` (a UI ABERTA e' o que
   mantem o container UNFROZEN — sem ela congela e mata o frida-server; tambem
   destrava o adb, que fica "offline" ate a UI subir).
2. `waydroid app install <MOD.apk>`; adb connect 192.168.240.112:5555.
3. frida-server 16.7.19 x86_64 em /data/local/tmp; **rodar em FOREGROUND persistente**
   (`sudo waydroid shell -- .../frida-server16 -l 127.0.0.1:27042` como task de fundo
   que segura a conexao) — com `-D`/nohup/setsid ele MORRE. adb forward tcp:27042.
4. O app MOD roda (SplashScreen->GameActivity, Qihoo decifra o dex) mas MORRE em
   segundos (crash nativo no houdini). Solucao p/ dumpar: script frida que
   **bloqueia exit/_exit/abort/pthread_kill + engole SIGSEGV/SIGABRT** (Interceptor.replace
   + signal handler no-op), spawna, resume, espera 8s (Qihoo decifra), e faz
   Memory.scanSync do magic dex "64 65 78 0a 30 33 ?? 00" em ranges r--/rw-,
   carveando por header size @+0x20. Script: /tmp/claude-1000/dexdump_hold.py.
   => 64 blobs dumpados. O dex REAL do jogo (6554 classes) =
   `dex_decrypted/game_classes.dex` (5.7MB). (os de 3.1MB = stub Qihoo).

### O que o dex REAL revelou (ground truth do fluxo online)
- `GameAPIAndroidGLSocialLib.InitGameAPI(ZZZ)` -> inner$6 na UI thread inicia
  sign-in Google Play. Offline, cai em `onSignInFailed()`: com sInitRequestIsCalling=1
  chama `nativeGameAPINotifyAuthChanges(1, erro)` + **`nativeGameAPIComplete()`**.
  => GameAPI FALHA GRACIOSAMENTE offline e NAO trava o menu. (Confirma por que
  meu TASM2_GAMEAPI_COMPLETE_FRAME nao destravou — nao era o gate.)
- `SUtils.isCheckVersionSDKs()` retorna 0 (sem gate de versao).
- `initCheckConnectionType()` = CheckConnectionType() real ou 0 (gracioso).
=> CONFIRMADO: o gate do menu e' 100% NATIVO (libtasm2.so). O jogo vai pro menu
offline no Android real; nosso port trava por alguma diferenca de AMBIENTE que o
native ve (nao por callback Java faltando). Agora temos AMBOS os lados p/ cruzar:
binario nativo (rizin) + Java completo (dex).

### Proximo passo (agora tratavel, cross-ref):
Comparar metodo-a-metodo o que o jni_shim devolve nas chamadas de boot vs o que
as classes Java REAIS fazem (DataSharing/SUtils/Device values, ordem de callbacks,
lifecycle). Alvo provavel: valores GAIA/DataSharing fake que colocam o native
num estado ruim, OU um sinal de lifecycle/surface que o so-loader nao entrega.
Ferramentas prontas: dex em dex_decrypted/, rizin rz_proj.rzdb, Waydroid up.

## Update 2026-07-06 (cont.5): jogo vai MUITO mais longe do que se pensava

Cross-ref com o dex real corrigiu varias hipoteses e revelou o fluxo verdadeiro.

### Fixes/verificacoes aplicados
- **jni_shim getSharedValue: retornar "" (string vazia) em vez de jstring NULA**
  p/ chave ausente. CONFIRMADO pelo dex: `DataSharing.getSharedValue` REAL sempre
  devolve "" (nunca null) e `isSharedValue` = (valor != ""). Com jstring nula o
  GAIA nativo estourava SIGSEGV no ctor de std::string (libc+0x7c720). Fix no
  tree; melhora correta (nao muda o caminho estavel FORCE_GAIA, so' o caso null).
- GameAPI (Google Play) FALHA GRACIOSAMENTE offline (onSignInFailed ->
  nativeGameAPIComplete) — NAO e' o gate (confirmado no dex).
- GAIA nativo gera/valida o GLUID (nao ha classe Java GAIA). Com valores AUSENTES
  (NO_DEFAULTS) o nativo tenta gerar fresco (HEI 7000->8002->8002->8007) e CRASHA
  na geracao (usa ponteiro interno null, nao um retorno JNI nosso). => NO_DEFAULTS
  nao serve; o caminho estavel continua FORCE_GAIA_STATUS_PATCH.

### O fluxo REAL de boot (com FORCE_GAIA, via IO+JNI trace dedup)
O jogo NAO trava cedo. Com GAIA forcado ele avanca ate o **CARREGAMENTO DE ASSETS**:
- le saves ud_*.sav (Control/Sound/Language/FriendList/OObjects OK; Item/Economy/
  GFX/InitPos/Connectivity ausentes = jogo novo, ok).
- PROBES de arquivos de RENDER que FALTAM (errno=2): `file.map` (indice de assets!),
  `GameOptions_saved.json`, `lut_fpp.and`/`lut_mid.and` (color LUTs de pos-proc),
  `lod_visualize_check_map.{and,tga,png}`. O jogo CONTINUA apos eles (le _ckt.dat,
  base.apk=31MB MOD, dyanmicSpecs.t, sliderUpdates.t) e SO ENTAO idle roxo.
- 4 chamadas `mid=NULL` (callers 0xe8120/0xe8190/0xe81c8 = helpers JNI genericos):
  metodos que o nativo chama e nao mapeamos; PORT_STATE original ja marcou isso.

### Suspeita atual do gate do frame roxo (pos-GAIA)
Setup de RENDER nativo falhando por assets ausentes — provavel `file.map` (o
indice que localiza texturas/shaders) e/ou os `lut_*.and`. Estao DENTRO do OBB
(formato CUSTOM Gameloft, magic 0D F0 AD 8B — NAO e' zip, unzip nao le). O jogo
tem seu proprio resource loader p/ o OBB; ou ele deveria extrair file.map/luts
p/ files/ e nao esta, ou o loader do OBB no nosso ambiente nao entrega esses
recursos. Confirmar: interceptar no nativo/hook a leitura do OBB e ver se
file.map/lut sao servidos de dentro do OBB.

### Estado: NAO jogavel ainda. Muro = multi-camada (servidor morto + Qihoo +
### OBB custom + setup de render nativo). Cada camada resolvida revela a proxima.
### Proximo alvo concreto: file.map/luts dentro do OBB custom (resource loader).

## Update 2026-07-06 (cont.6): tiro decisivo (trace real) BLOQUEADO

Tentativa de capturar o trace JNI/Java real via frida no Waydroid falhou por DOIS
muros somados:
1. **Qihoo tem ANTI-FRIDA.** Sob frida spawn, hooks em Application.onCreate /
   Activity.onCreate NUNCA disparam — o app sai antes do onCreate. Sem frida
   (monkey) o app roda (logcat: SplashScreen->GameActivity). => Qihoo detecta a
   instrumentacao e aborta. (O dump do dex funcionou pq foi memory-scan RAPIDO,
   antes do anti-frida; instrumentacao Java AO VIVO nao passa.)
2. **O jogo CRASHA no nativo no Waydroid** (houdini ARM) mesmo sem frida — so'
   chega ate GameActivity e morre. Entao o Waydroid nunca executa o fluxo nativo
   "que funciona" (menu). Nem com trace daria pra ver o gate nativo.

CONCLUSAO: nao existe ambiente acessivel onde o jogo NATIVO rode ate o menu
(Waydroid=x86+houdini crasha; alvo=Linux/Mali). Logo o gate nativo so' e'
observavel no proprio port (Mali-450) e/ou por RE estatico. Trace real de app
funcionando exigiria um Android ARM real (device rooted) onde o MOD chegue ao
menu — nao disponivel neste setup.

Estado: Spider-Man e' muro multi-camada de ROI baixo (servidor morto + Qihoo
anti-frida + OBB custom + gate de render nativo). Ganhos reais da sessao
(reutilizaveis): dump de dex Qihoo, rizin, Waydroid, fix getSharedValue "".
Recomendacao: aplicar as ferramentas no PES 2012 (mesmo Qihoo) ou pausar.

## Current state

- The game is native C++/JNI Gameloft, not Unity.
- ARM32 is viable on the current NextOS setup because the user confirmed multilib in `/home/felipe/NextOS-Elite-Edition`.
- The loader boots, audio is active, and controls/autokey are wired.
- The renderer is not fully broken anymore: the real Marvel logo renders first.
- After the Marvel logo the screen becomes a uniform purple/blue frame: RGBA `(127,127,255,255)`.
- The final blocker is now progression after splash/loading, not initial GL bring-up.
- No `tasm2` process was left running on the device when this state was saved.

## Current binary

- Built locally with `./build.sh`.
- Deployed to:
  `/storage/roms/ports/tasm2/tasm2`
- Last deployed binary size seen on device:
  `263460` bytes.

## Important code changes already in tree

- `src/imports.c`
  - Android data/OBB paths under `/storage/emulated/0/...` and relative variants map to local `files/` and `obb/`.
  - This fixed asset path leaks; OBB fallback is working.

- `src/main.c`
  - GAIA status patch is off by default unless `TASM2_FORCE_GAIA_STATUS_PATCH` is set.
  - Added `TASM2_END_SPLASH_FRAME=N` to delay `GL2JNILib.EndSplashScreen` until a specific frame.
  - Split `TASM2_SKIP_UTILS` into narrower gates:
    - `TASM2_SKIP_SUTILS`
    - `TASM2_SKIP_DEVICE`
    - `TASM2_SKIP_DATASHARING`
  - Added optional `GameOptions_onResumeGame` / `GameOptions_onPauseGame` calls:
    - `TASM2_CALL_GAMEOPTIONS_RESUME`
    - `TASM2_CALL_GAMEOPTIONS_PAUSE`
  - Added hidden profile globals:
    - `TASM2_PROFILE_COUNTER`
    - `TASM2_PROFILE_FLAG`

- `src/jni_shim.c`
  - JNI float-return callbacks use softfp ABI (`pcs("aapcs")`) because `libtasm2.so` is ARM softfp and the loader is hardfp.
  - `_GAIA_FIRST_LAUNCH` can be forced absent with `TASM2_GAIA_FIRST_LAUNCH_ABSENT`.
  - GAIA shared defaults are still needed; removing them causes HEI 8002/8007 and crash.
  - GameAPI native callback path exists but is env-gated:
    - `TASM2_GAMEAPI_NATIVE_CALLBACKS`
    - `TASM2_GAMEAPI_PLATFORM_INIT`
    - `TASM2_GAMEAPI_NATIVE_COMPLETE`

- `src/softfp_shim.c`
  - Added dense GL logging (`TASM2_GL_DENSE`).
  - Tracks current GL program, active texture, and bound 2D textures.

- `src/pthread_bridge.c`
  - Added optional `TASM2_BLOCK_GAIA_THREADS`.
  - This can block the noisy GAIA/GLOT worker at `libtasm2.so+0xcbce34`, but it does not by itself fix the purple frame.

- `_userpreferedprofile.dat`
  - Added locally and copied to the device.
  - Content:
    `standard_profile`

## Latest tests and results

### Baseline/current useful env

```sh
LD_LIBRARY_PATH=/usr/lib32:. \
TASM2_INIT_VIEWSETTINGS=1 \
TASM2_END_SPLASH=1 \
TASM2_CALL_SETPATHS=1 \
TASM2_CALL_SPLASH_GLOT=1 \
TASM2_CALL_SPLASH_FUNC=1 \
TASM2_GAMEAPI_LOGGED_IN=1 \
TASM2_CONNECTION_TYPE=1 \
TASM2_AUTOKEY=1 \
TASM2_FRAMES=1600 \
./tasm2
```

Result:
- Marvel logo appears.
- Then screen becomes uniform purple/blue.
- Audio stays active.

### Delayed splash test

```sh
TASM2_END_SPLASH_FRAME=240
```

Result:
- Log confirmed: `GL2JNILib.EndSplashScreen OK frame=240`.
- Final raw capture was still fully uniform `(127,127,255,255)`.
- Conclusion: calling `EndSplashScreen` immediately is not the main cause.

### JNI debug test

```sh
TASM2_JNI_DEBUG=1
```

Result:
- Captured through roughly 520 frames.
- Early preference call seen: `getPreferenceString("SDFolder")`.
- `nativeGetPreference` is registered but was not called in that capture.
- Welcome/video Java methods were registered but not called:
  - `sWelcomeScreenLaunch`
  - `sWelcomeScreenSetIsPau`
  - `sLaunchVideoPlayer`
  - `playVideo`
- `InitGameAPI(ZZZ)` is called once.
- Some `CallStatic*Method` calls still have `methodID == NULL`; likely worth tracing class names in `GetStaticMethodID` next.

### IO/profile test

Result:
- OBBs open and read correctly.
- Most missing loose files are expected OBB fallback, not a hard failure.
- Real missing file found earlier:
  `_userpreferedprofile.dat`
- After adding `_userpreferedprofile.dat`, it opens successfully.
- Final framebuffer still becomes uniform `(127,127,255,255)`.

### GAIA worker block test

```sh
TASM2_BLOCK_GAIA_THREADS=1
```

Result:
- Blocks 2 noisy workers at `libtasm2.so+0xcbce34`.
- Removes the `gv3/Priority.bin` and `Stream.bin` spam.
- Does not crash.
- Audio stays active.
- Final framebuffer still becomes uniform `(127,127,255,255)`.

Conclusion:
- GAIA/GLOT telemetry is heavy noise and should probably stay neutralized during tests, but it is not the only visual blocker.

### GameOptions resume test

```sh
TASM2_CALL_GAMEOPTIONS_RESUME=1
```

Result:
- `GameOptions.onResumeGame OK`.
- No crash.
- Did not unlock rendering/progression.

### Profile globals test

```sh
TASM2_PROFILE_FLAG=1
TASM2_PROFILE_COUNTER=1
```

Result:
- Hidden profile globals log as `1`.
- Final visual result is still expected to be purple; raw capture was saved locally as:
  `/tmp/tasm2-profileflags.raw`

### GDRM/installer test

```sh
TASM2_CALL_INSTALLER_INIT=1
TASM2_CALL_INSTALLER_START=1
TASM2_CALL_GDRM_INIT=1
TASM2_CALL_GDRM_ALLOW=1
```

Result:
- Bad path for now. Do not use as normal run.
- `GDRMPolicy.nativeAllow(...) -> 0`.
- Then crashes with SIGSEGV.

### No EndSplashScreen test

Result:
- Did not show state progress in logs.
- Continued the same GAIA/GLOT worker loop.
- Capture did not get copied in that run, so do not treat it as a visual proof.

### Skip only DataSharing test

```sh
TASM2_SKIP_DATASHARING=1
TASM2_END_SPLASH_FRAME=240
```

Result:
- Bad path. Do not use as solution.
- `SUtils.nativeInit` and `Device.nativeInit` ran, but `DataSharing.nativeInit` was skipped.
- Game hit GAIA errors:
  - `[HEI] 8002`
  - `[HEI] 8007`
- Then crashed with SIGSEGV.
- Conclusion: `DataSharing.nativeInit` is needed for GAIA shared/default values, or an equivalent replacement must be provided.

### Skip all utils test from earlier

```sh
TASM2_SKIP_UTILS=1
```

Result:
- Bad path. Do not use as solution.
- Causes HEI 8002/8007 and heavy GLOTv3 remove/path spam.

## GL findings

- Purple is a real rendered/cleared frame, not a blank EGL failure.
- Earlier dense trace showed:
  - Logo draw uses program `9` with texture `22`.
  - After logo disappears, the frame is cleared to purple.
  - Later frames mostly use program `3`; program `9` no longer appears.
- No clear shader compile/link failure was found.
- ETC1 compressed texture upload works.
- FBO completeness was OK in the previous trace.

## OBB/assets

OBBs present and opening:

- `obb/main.12032.com.gameloft.android.ANMP.GloftASHM.obb`
  - size: `1236510228`
- `obb/patch.12723.com.gameloft.android.ANMP.GloftASHM.obb`
  - size: `98576965`

Missing loose files such as `textures_vehicle.pak` are expected if they are inside OBB.

## Strong suspicion

The game is stuck after splash in an online/profile/lifecycle state, probably around GAIA/GLOTv3/DataSharing or a missing Java callback/state, not because ARM or GLES cannot render.

Important recurring thread/function area:

- `libtasm2.so+0xcbce34` appears repeatedly and is likely GAIA/GLOT worker related.
- Other recurring worker/callback addresses seen:
  - `libtasm2.so+0x10db4e5`
  - `libtasm2.so+0xa83300`
  - `libtasm2.so+0xb8e1a0`

## Do not repeat

- Do not disable all utils as a final approach.
- Do not skip `DataSharing.nativeInit` alone unless also replacing the GAIA shared/default behavior.
- Do not force `TASM2_GAIA_NO_DEFAULTS=1`; it caused GAIA parse errors/crash earlier.
- Do not assume this is still a pure GL black-screen issue; Marvel logo proves the draw path works.

## Recommended next resume steps

1. Investigate profile/save state:
   - Search for `_userpreferedprofile.dat`, profile, welcome, first-launch, and save defaults.
   - Check whether a missing profile marker keeps the game in splash/post-splash state.

2. Add targeted JNI logging/returns for lifecycle callbacks:
   - `sWelcomeScreenLaunch`
   - `sWelcomeScreenSetIsPau`
   - video/player callbacks if they are called later.

3. Investigate GAIA/GLOT worker loop without breaking `DataSharing.nativeInit`:
   - Prefer neutralizing telemetry/network work after shared defaults exist.
   - Avoid skipping initialization that creates required shared values.

4. If needed, disassemble around:
   - `0xcbce34`
   - `0xc8e2fc`
   - `0xc8cfe0`
   - `0xc8e340`
   - `0xc8e370`
