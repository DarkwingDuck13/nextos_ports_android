# Graveyard Keeper — STUDY + HANDOFF (so-loader Mali-450)

> Atualizado 2026-06-26. Port builda, roda no device `.89`, passa do boot Unity/Choreographer e mostra a tela de loading do Graveyard Keeper no framebuffer. Referência prática: **Terraria/Unity funcional**. Pixel Cup não deve ser usado como referência de runtime porque não foi concluído.

## TL;DR
Graveyard Keeper = **Unity 2018.2.2f1 IL2CPP**, sem DRM, texturas ETC1. O port arm64 já renderiza a tela de loading no Mali-450 usando o caminho Terraria: JNI fake mínimo, pthread/cond bridge, Choreographer dirigido e execução direta dos binários no device.

## O JOGO (verificado na APK)
- APK: `/home/nextos/Downloads/graveyard-keeper-v1.129.1-apkvision.org.apk` (210MB). Extraído em `/home/nextos/Downloads/gk_study/apk/`.
- Engine: **Unity 2018.2.2f1 IL2CPP** (confirmado em libunity.so e globalgamemanagers).
- ABIs: arm64-v8a (usado), armeabi-v7a, x86.
- Libs nativas arm64: `libil2cpp.so` (73MB, lógica C#), `libunity.so` (17MB, engine, exporta JNI_OnLoad@0x259f30), `libmain.so` (só JNI_OnLoad), `libgpg.so` (Google Play Games — provável stub).
- `global-metadata.dat` **PLAINTEXT** (magic `af1bb1fa`, version 24=Unity2018/19). Sem cripto.
- **Sem pairip, sem VM de DRM.** Só `HasProLicense`/libgpg (Google licensing benigno).
- Texturas: **ETC1** predominante (`UI/DefaultETC1`) = nativo Mali-450 ES2 ✅. ⚠️ ~12 texturas **DXT5** soltas nos bundles (provável UI/RGBA) → se forem carregadas de fato, precisam transcode p/ ETC1/RGBA (técnica do FF9 asset_redirect).
- libil2cpp NEEDED: liblog, **libstdc++** (bionic, NÃO libc++_shared), libm, libdl, libc.
- Assets: `assets/bin/Data/` = 494MB, 15330 arquivos locais; 15276 são bundles hasheados sem extensão, mais globalgamemanagers/levelN.splitN/global-metadata.dat.

## DEVICE
- `.89` = **EmuELEC NextOS-Retro-Elite-Edition**, kernel 3.14.79 aarch64, **Mali-450 Amlogic S905L**, 832MB RAM, fb0 virtual_size=1280x1440 (= double-buffer/**meio-buffer com panning**, igual LEGO Batman/FF9). 41GB livres em /storage/roms.
- SSH: `sshpass -p '' ssh root@192.168.31.89` (root, senha vazia).
- ⚠️ Regra #8: lançar com `nohup bash ./run.sh`, **NUNCA setsid/sh** (Amlogic old Mali-450).
- ⚠️ Regra #6: **NÃO forçar SDL_VIDEODRIVER/SDL_AUDIODRIVER** (já respeitado no run.sh).
- ⚠️ Regra #3: matar+confirmar 0 instâncias antes de lançar (run.sh já faz via /proc/PID/exe).

## O QUE JÁ FOI FEITO (estado atual)
1. Port dir: `/home/nextos/nextos_ports_android/ports/graveyardkeeper/`.
2. `payload/lib/` = libil2cpp.so, libunity.so, libmain.so, libgpg.so (arm64 do GK).
3. `payload/assets/bin/Data/` = assets do GK.
4. Imports regenerados do GK: `bash gen-unity-imports.sh payload/lib/libunity.so payload/lib/libil2cpp.so` → `src/imports_unity.gen.c` (366 entradas, 90 stubs, 276 passthrough). **Removido o `src/imports.gen.c` stale do pixelcup** (conflito de `dynlib_functions`).
5. `build.sh` ajustado (nome graveyardkeeper, toolchain NextOS Amlogic-old). **BUILD OK** → binário `graveyardkeeper` arm64 (warnings de SDL2 ld são benignos).
6. `run.sh` adaptado: GAMEDIR=/storage/roms/ports/graveyardkeeper, prefixo GK_, sem forçar SDL driver, resolução automática de fb0.
7. **Deploy feito** via rsync p/ `/storage/roms/ports/graveyardkeeper/` no .89 (lib/ + assets/ + binário + run.sh). Warnings de chown são benignos (vfat).
8. Crash inicial `libil2cpp+0x3084f88` corrigido: imports de `pthread_attr_*`/`pthread_getattr_np` entram no shim e devolvem stack fake válida para o GC/IL2CPP.
9. `pthread_create_fake` virou default para GK, mantendo `TER_NOPTCREATE=1` como escape.
10. JNI de Android/Unity ajustado para GK:
    - package/path: `com.tinybuild.graveyardkeeper`, `base.apk`, `userdata`, `lib/*.so`;
    - `HandlerThread`, `Handler`, `Looper` e `Message` agora têm sentinelas não nulas;
    - `obtainMessage` guarda `what`, `sendToTarget` chama `handleMessage` sincronamente;
    - `native invoke` agora recebe a interface correta: `java/lang/Runnable`, `android/os/Handler$Callback`, `android/view/Choreographer$FrameCallback`.
11. Choreographer destravado:
    - `TER_CHOREO=1` dispara `doFrame` em thread própria;
    - `GK_SKIP_CHOREO_WAIT=1` libera apenas o cond-wait específico `libunity+0x271404` (`pred = cslot - 0x30`), sem mexer nos outros waits.
12. `run.sh` já exporta por padrão:
    ```sh
    TER_CHOREO=1
    GK_SKIP_CHOREO_WAIT=1
    ```
13. Redirect de assets do GK corrigido:
    - `.res/.resS/.resG` de `sharedassets*`, `globalgamemanagers` e hashes redirecionam para o arquivo real sem sufixo ou `.split0`;
    - bug de `snprintf(buf, "%s", buf)` removido no caminho `.split0`;
    - `.resource` continua fallback para nomes não-hash.
14. Diagnóstico async/vtable adicionado:
    - `GK_NATIVELOADSPY`, `GK_ASYNCPOLL`, `GK_ASYNCDUMP`, `GK_ASYNCVTSPY`, `GK_VT11SPY`;
    - `vt11 = libunity+0x619438` entra no worker/background do async;
    - `vt12 = libunity+0x619c20` retorna `1` por frame;
    - `vt13 = libunity+0x619cf0` não foi observado.
15. `GK_VT11SPY` mapeou helpers internos, mas **não usar hook C normal em `libunity+0x631250`**: a função usa `x8` como hidden/result argument e wrapper C corrompe chamada. Se precisar, só com hook em assembly preservando `x8`.
16. `GK_STATSPY` adicionado para mapear `stat/lstat/stat64/lstat64` sem ligar `GK_DLLOG`.
17. Fast paths de filesystem adicionados para acelerar a varredura de bundles:
    - `stat-fastdir`: `libunity+0x6c916c` é `DirectoryExists` (`stat` + `S_IFDIR`). Para `assets/bin/Data/<hash>.res*`, retorna falso imediato. Opt-out: `GK_NOFASTDIRSTAT=1`.
    - hashes de 32 hex com `.res/.resS/.resG` redirecionam direto para o arquivo hash sem `access(.resource)` e sem `access(hash)` prévio; o `stat/open` final ainda decide existência.
    - `stat-fasthash` existe como experimento opt-out (`GK_NOFASTHASHSTAT=1`), mas a varredura dominante observada até agora é `DirectoryExists`, não esse caminho.

## TESTES CONFIRMADOS
- `./build.sh` local: **BUILD OK** (`graveyardkeeper` ELF arm64). Warnings de debug do `libSDL2.so` no linker são benignos.
- Deploy manual de binário e `run.sh` no `.89`: OK.
- Teste manual `GK_FRAMES=1200 TER_CHOREO=1 GK_SKIP_CHOREO_WAIT=1`: `rc=0`, passou de `render 1140`.
- Teste final pelo `run.sh` normal, só com `GK_FRAMES=900`: `rc=0`, passou de `render 840`.
- Framebuffer capturado de `/dev/fb0` (`1280x1440`, 32bpp, stride 5120). As duas metades mostram a tela de loading do Graveyard Keeper corretamente. PNG local de prova: `/tmp/gk_fb_top.png` e `/tmp/gk_fb_bottom.png`.
- Log-chave esperado:
  ```text
  [CHOREO] driver-thread de doFrame criada (~60Hz)
  jni_shim: [CHOREO] FrameCallback capturado
  [CHOREO_WAIT] libunity+0x271404 ... -> ready
  [CHOREO] doFrame começou a disparar
  [render 600]
  [F2] === render loop terminou ===
  ```
- Teste com `GK_STATSPY=1` depois dos fast paths: `stat #15000` chega por volta do frame 960; antes a mesma faixa só chegava depois de vários milhares de frames. Sem crash.
- Teste longo em 2026-06-26:
  ```sh
  env GK_RESTART_ES=0 GK_FRAMES=10000 GK_LOADSPY=1 GK_NATIVELOADSPY=1 GK_ASYNCPOLL=1 bash ./run.sh
  ```
  Resultado: `rc=0`, render loop até `f=9960`, sem crash, sem `Internal_SceneLoaded` da `scene_main_mobile`, `done=0 progress=0.000` o tempo todo.

## PONTO EXATO ONDE PAROU
- O boot Unity/IL2CPP está vivo e renderizando. Não é mais crash inicial, EGL, assets base ou Choreographer.
- A primeira cena (`level0` / `preloader`) carrega e dispara `SceneManager.Internal_SceneLoaded`.
- O preloader chama `SceneManager.LoadSceneAsync("scene_main_mobile", LoadSceneMode.Additive)`.
- O `AsyncOperation` managed é criado e aponta para um objeto nativo válido.
- `AsyncOperation.get_isDone` fica `0`; `AsyncOperation.get_progress` fica `0.000`.
- `set_allowSceneActivation(op, true)` via `GK_FORCE_ALLOW=1` foi testado e **não muda** o progresso.
- Com `GK_STATSPY=1`, o `UnityPreload` varre milhares de paths `assets/bin/Data/<hash>.res`; o caller dominante é `libunity+0x6c916c`, confirmado por disassembly como `DirectoryExists`.
- GDB durante o preload mostrou três fases:
  - antes dos fast paths: `UnityPreload -> my_stat -> asset_redirect -> access`;
  - depois dos fast paths: `UnityPreload -> my_stat -> fstatat64`, depois `UnityPreload -> my_open -> open64`;
  - fase tardia: volta para libc/Unity sem unwind útil, mas o processo segue vivo.
- Conclusão atual: a requisição async existe e o pipeline nativo trabalha/abre bundles, mas o método background `vt11` não chega ao ponto que publica progresso (`progress=0.9` em `libunity+0x619970`) nem dispara conclusão até 10000 frames.

## PRÓXIMO PASSO
1. Continuar no `vt11` do GK (`libunity+0x619438`), não em RVAs herdados de outro port.
2. Instrumentar o trecho pós-open do `UnityPreload`: pegar PC/LR em fase tardia com gdb e/ou hooks leves em helpers já mapeados (`0x61a35c`, `0x61bd70`, `0x61c254`, `0x6310f8`, `0x631c2c`, `0x631e98`, `0x6336f4`, `0x633b24`, `0x63402c`, `0x634d20`, `0x619a70`).
3. Evitar `GK_VT11SPY` muito verboso em runs longos: ele muda timing e deixa a varredura lenta. Usar hooks pontuais ou contadores.
4. Não usar `CUP_PSPY`, `CUP_PRELOAD_BG`, `CUP_DRAINPRELOAD` nem RVAs `0x873xxx`: são de outro Unity e ficam bloqueados por padrão salvo `GK_ALLOW_OLD_PSPY=1`.
5. Quando `progress` sair de `0.000` ou `scene_main_mobile` disparar `Internal_SceneLoaded`, validar menu, input e áudio. Controle provável: reaproveitar a camada do Terraria (`TER_GAMEPAD`/`TER_CTRL`) quando o menu existir.

## HIPÓTESE PRINCIPAL
GK é Unity 2018.2, classe próxima dos ports Unity antigos que já funcionam. O gargalo inicial foi o `UnityChoreographer` sem Looper Java real; isso já foi resolvido. O gargalo atual está dentro do trabalho background do `AsyncOperation` de `scene_main_mobile`: ele já passa pelo resolvedor de assets e chega a abrir bundles, mas não retorna/publica progresso. Precisa ser derivado pelos ponteiros/vtable do próprio GK.

## FLAGS/MUROS PROVÁVEIS (do histórico Unity IL2CPP)
- `-force-gfx-direct` via cmdline (main.c já tem cmdline_fd / fopen handling do scaffold) → GL single-thread.
- `libgpg`/Google Play Games pode crashar no init → stubar (não é necessário pro jogo).
- EGL ES2 forçado no egl_shim (já no scaffold; ver FF9 que força ES2).
- Choreographer/doFrame: **confirmado necessário** (`TER_CHOREO=1` + `GK_SKIP_CHOREO_WAIT=1`).
- Job-system: usar a lógica do Terraria como referência conceitual, mas offsets precisam ser recalculados para GK. O spy real do GK já tem `GK_JOBSPY`/`GK_SKIPJOBWAIT` nos RVAs `0x26d1cc/0x26d20c`.
- DXT5 stragglers → asset_redirect transcode SE aparecerem texturas pretas.
- fb meio-buffer panning (igual LEGO Batman): egl_shim do scaffold já lida; validar olhando metade de CIMA do dump.

## REFERÊNCIAS NO DISCO
- **PRINCIPAL**: `ports/terraria` (Unity IL2CPP jogável: controle+áudio+mundo). Usar como referência de runtime.
- `ports/pixelcup`: não usar como base de decisão de runtime; ficou incompleto/travado. Qualquer bloco `CUP_*` herdado no código do GK é legado/diagnóstico e não deve virar próximo passo sem derivação nova.
- `ports/ff9` (Unity 2022.3 IL2CPP: EGL ES2 forçado, asset_redirect, shaders ES3→ES2, input hook).
- `ports/chrono` (Cocos2d-x, no device .89 também).
- Toolchain: `~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain`.

## OBJETIVO (NextOS)
Jogo com **gameplay + controles + áudio OK + imagem na tela OK**.

---

## HANDOFF 2026-06-26 - PAUSA

Pedido do NextOS: parar agora, salvar tudo e deixar claro onde continuar.

### Commit base antes desta sessão
- Commit limpo anterior do scaffold: `570f776d3285a2b2e1d26f1418440cdb68368b7b Add Graveyard Keeper port scaffold`.
- Nesta pausa, salvar somente arquivos de `ports/graveyardkeeper/`. Existem mudanças sujas em outros ports no workspace; não mexer nelas.

### O que mudou nesta sessão
1. `src/main.c`
   - Adicionado patch opt-in `GK_NOSOUNDASSERT=1`:
     - RVA: `libunity+0x17bc04`.
     - NOP no `tbz` que entra no assert de `SoundHandle::Instance::~Instance()`.
     - Objetivo: manter o áudio nativo, mas impedir o `brk #1` quando o Unity libera `SoundHandle` fora da main thread.
   - Adicionado hook GK-specific de `FMOD::System::createSound`:
     - RVA real do GK: `libunity+0x985058`.
     - Flags:
       - `GK_AUDIOSPY=1`: loga `createSound`/falhas.
       - `GK_STREAMFALLBACK=1`: se `createSound` falhar sem `FMOD_CREATESTREAM`, tenta repetir com bit `0x80` ligado.
     - Importante: offsets antigos `TER_AUDIOSPY`/`TER_STREAMFALLBACK` de Terraria (`0x806cb4`, `0x805a94`) NÃO servem para GK.
   - `GK_NOAUDIOLOAD=1` ficou apenas como diagnóstico:
     - RVA: `libunity+0x182224`.
     - Não usar como caminho final, porque pula `SoundManager::IntegrateFMODSound` e inicia o jogo sem handles reais de áudio.
2. `src/jni_shim.c`
   - `Class.forName("com.google.games.bridge.TokenFragment")` agora devolve classe rastreável via `class_for`.
   - `NewObject` para classes `com.google.games.bridge.*` devolve objeto fake não nulo.
   - Motivo: após `Preloader.OnSceneLoaded: scene_main_mobile`, o plugin Google Play Games entrava nesse caminho e não podia receber classe/objeto nulo.

### Testes importantes desta sessão
1. Build local:
   ```sh
   cd /home/nextos/nextos_ports_android/ports/graveyardkeeper
   ./build.sh
   ```
   Resultado: `BUILD OK`. Warnings `_GNU_SOURCE` e warnings de debug do `libSDL2.so` continuam benignos.

2. Teste nativo de áudio com `GK_NOSOUNDASSERT=1` (sem `GK_NOAUDIOLOAD`):
   - Comando usado no device:
     ```sh
     cd /storage/roms/ports/graveyardkeeper
     env GK_RESTART_ES=0 GK_FRAMES=16000 GK_LOADSPY=1 GK_NATIVELOADSPY=1 GK_ASYNCPOLL=1 GK_RESSPY=1 GK_NOSOUNDASSERT=1 bash ./run.sh
     ```
   - Resultado salvo localmente:
     - `/tmp/gk-nosoundassert-done1-run.out`
   - Resultado técnico:
     - `progress` saiu de `0.000` perto de `f=13113`.
     - Em `f=13410`: `Cannot create FMOD::Sound instance for resource sharedassets1.resource, (Not enough memory or resources. )`.
     - Sem o patch, isso caía depois no assert `SoundHandle::Instance::~Instance() may only be called from main thread!`.
     - Com `GK_NOSOUNDASSERT=1`, chegou a:
       ```text
       [GK_ASYNCPOLL] f=13469 ... done=1 progress=1.000
       ```
     - Ou seja: `scene_main_mobile` completa o async e entra, mantendo o caminho de áudio nativo, mas ainda com erro FMOD em `sharedassets1.resource`.
   - Problema restante nesse run:
     - Depois de `Preloader.OnSceneLoaded: scene_main_mobile`, aparece spam:
       ```text
       [ALOG:6 CRASH] main thread is trapped; signum = 11
       [ALOG:6 CRASH] other thread is trapped; signum = 11
       ```
     - O processo continuou até `F2 === render loop terminou ===` por limite de frames, então não foi morte imediata. Precisa investigar no próximo retorno.

3. Teste com `CUP_NOSIGINST=1`:
   - Não usar como padrão.
   - Com `GK_NOSOUNDASSERT=1 CUP_NOSIGINST=1`, o processo chegou só perto de `progress=0.325/0.334` e morreu `RC=137`.
   - Hipótese: muda timing/memória e expõe OOM; não é caminho normal.

4. Teste `GK_NOAUDIOLOAD=1`:
   - Foi diagnóstico apenas.
   - Ficou mais lento e não deve ser usado como solução: em teste longo ficou perto de `progress=0.103`, sem completar no limite observado.
   - NextOS pediu explicitamente para não resolver pulando/desativando áudio. Próximo passo deve seguir áudio nativo.

5. Teste `GK_AUDIOSPY=1` em 2026-06-26:
   - Build e deploy do binário com hook OK.
   - Comando iniciado:
     ```sh
     cd /storage/roms/ports/graveyardkeeper
     env GK_RESTART_ES=0 GK_FRAMES=18000 GK_LOADSPY=1 GK_NATIVELOADSPY=1 GK_ASYNCPOLL=1 GK_NOSOUNDASSERT=1 GK_AUDIOSPY=1 bash ./run.sh > run-test-audio-spy.out 2>&1
     ```
   - O hook instalou:
     ```text
     [GK_AUDIO] hook createSound libunity+0x985058 instalado (spy=1 streamfallback=0)
     ```
   - Até `f~12540`, ainda estava `progress=0.000`; isso é esperado, porque no run bom a barra só acordou em `f~13113`.
   - Perto dessa janela o device saturou: ping continuou respondendo, mas novas conexões SSH passaram a falhar com:
     ```text
     Connection timed out during banner exchange
     ```
   - Estado do device ao pausar:
     - `192.168.31.89` responde ping.
     - SSH não abre sessão nova.
     - A sessão principal remota encerrou pelo lado do cliente, mas não foi possível confirmar se o processo morreu.
     - ADB TCP em `192.168.31.89:5555` recusou conexão.

### Estado exato para continuar
1. Primeiro recuperar o `.89`:
   ```sh
   ping -c 1 192.168.31.89
   ssh -F /dev/null -o StrictHostKeyChecking=no -o ConnectTimeout=20 root@192.168.31.89 'ps w | grep graveyardkeeper | grep -v grep'
   ```
   Se ainda houver processo:
   ```sh
   ssh -F /dev/null -o StrictHostKeyChecking=no root@192.168.31.89 'for p in /proc/[0-9]*; do e=$(readlink "$p/exe" 2>/dev/null); case "$e" in /storage/roms/ports/graveyardkeeper/graveyardkeeper*) kill -9 "${p##*/}";; esac; done'
   ```
   Se SSH continuar travado mas ping responder, provavelmente precisa reiniciar o device ou esperar o userspace liberar.

2. Assim que SSH voltar, copiar os logs antes de novo teste:
   ```sh
   scp -F /dev/null -o StrictHostKeyChecking=no root@192.168.31.89:/storage/roms/ports/graveyardkeeper/run.out /tmp/gk-audiospy-run.out
   scp -F /dev/null -o StrictHostKeyChecking=no root@192.168.31.89:/storage/roms/ports/graveyardkeeper/run-test-audio-spy.out /tmp/gk-audiospy-wrapper.out
   ```

3. Próximo teste recomendado, ainda com áudio nativo:
   ```sh
   cd /storage/roms/ports/graveyardkeeper
   env GK_RESTART_ES=0 GK_FRAMES=15000 GK_LOADSPY=1 GK_NATIVELOADSPY=1 GK_ASYNCPOLL=1 GK_NOSOUNDASSERT=1 GK_STREAMFALLBACK=1 bash ./run.sh
   ```
   Objetivo: ver se retry com `FMOD_CREATESTREAM` evita `FMOD_ERR_MEMORY` em `sharedassets1.resource`.

4. Se precisar logar argumentos do FMOD, usar `GK_AUDIOSPY=1`, mas evitar runs muito longos/verbosos até entender a saturação do `.89`.

### RE de áudio já confirmado
- `SoundManager::IntegrateFMODSound`: `libunity+0x182224`.
- `FMOD::System::createSound` wrapper real do GK: `libunity+0x985058`.
- Caller que monta `mode`/`resource`:
  - `libunity+0x156ca4`
  - `libunity+0x156db0`
  - `libunity+0x156eec`
  - `libunity+0x180228`
  - `libunity+0x182894`
- No caller `0x156eec`, o `mode` vem de `libunity+0x155790` e os tamanhos/canais parecem vir de campos do objeto de áudio:
  - `[x19+0x90]`, `[x19+0x88]`, `[x19+0x60]`, `[x19+0x78]`.

### Não esquecer
- Não usar `GK_NOAUDIOLOAD` como solução final.
- Não usar Pixel Cup como prova de runtime.
- Referência principal continua sendo `ports/terraria`, mas com offsets recalculados para GK.
- Próximo muro depois do áudio: spam/crash handler após `Preloader.OnSceneLoaded: scene_main_mobile`; a correção Google Play em `jni_shim.c` ainda precisa ser validada em run que chegue em `done=1` com o binário novo.

---

## HANDOFF 2026-07-03 - PAUSA NO DEVICE .90

Pedido do NextOS: parar agora e salvar estado. O processo remoto foi encerrado e confirmado:

```text
no-gk-process
MemAvailable: ~716MB
SwapFree: ~483MB
```

Device ativo desta rodada: `root@192.168.31.90`. Caminho remoto:

```sh
/storage/roms/ports/graveyardkeeper
```

### Arquivos alterados nesta rodada

- `run.sh`
  - log persistente em `/storage/roms/ports/graveyardkeeper/run.out`;
  - arquiva o log anterior em `logs/run-prev-<timestamp>.out`;
  - watchdog com dump de `/proc/meminfo`, `/proc/$pid/status`, threads, wchan e tail do log;
  - reset do OSD Amlogic para corrigir a regressao de zoom gigante (`free_scale=0`, axes 1280x720, `ppscaler=0`);
  - defaults novos: `CUP_GCSIG=1`, `TER_FAKEACK=1`, `CUP_MEMLOG=1`, `GK_NOSOUNDASSERT=1`, `GK_STREAMFALLBACK=1`.
- `src/main.c`
  - `TER_FAKEACK` corrigido para postar o semaforo real do GC suspend ACK em `libil2cpp+0x47004d0`;
  - `tgkill(SIGPWR/SIGXCPU)` interceptado para fake ACK;
  - classificador de worker ampliado (`Worker Thread`, `BackgroundWorke`, `UnityPreload`, `AsyncReadManage`);
  - `CUP_MEMLOG` ficou seguro por default: nao chama funcoes do GC sem `GK_MEMLOG_GC=1`;
  - `CUP_GCEVERY` ganhou `GK_GC_ONLY=1`, mas o caminho foi reprovado em teste;
  - `CUP_CTEXHALF` passou a dropar multiplos mips em vez de apenas o nivel 0.

Binario local e remoto sincronizados nesta pausa:

```text
graveyardkeeper md5: 93a6cd077ca26e3d43d2f1480e1cf480
run.sh md5: b134a41f253d08f83d4a5cd2c4ecde98
```

### Estado validado

- A tela com zoom gigante foi corrigida pelo reset de OSD no `run.sh`.
- O watchdog funciona e evita freeze do device. Quando memoria/swap passam do limite, o jogo e morto e o log fica salvo.
- O crash/trava antigo de GC suspend foi superado com `TER_FAKEACK=1` em `libil2cpp+0x47004d0`.
- O melhor caminho ainda usa:

```sh
PC_KICKWORKERS=2 PC_KICKWARM=0 PC_KICKMS=10 \
CUP_TEXHALF=512 CUP_CTEXHALF=512 \
GK_LOADSPY=1 GK_NATIVELOADSPY=1 GK_ASYNCPOLL=1
```

### Melhor run conhecido nesta rodada

Com `PC_KICKWORKERS=2` e `CUP_TEXHALF=512`, o async da cena principal avancou:

```text
LoadSceneAsync("scene_main_mobile")
progress=0.103
progress=0.333/0.334
```

Depois o watchdog matou por memoria antes do gameplay:

```text
rss ~= 699MB
swap ~= 184MB
total ~= 883MB
run saiu rc=137
```

Ou seja: o port ja passa do boot e entra na carga real da `scene_main_mobile`, mas ainda estoura o pico de memoria antes de completar.

### Testes reprovados nesta rodada

- `GK_NOAUDIOLOAD=1`
  - nao usar;
  - causa SIGSEGV em `fault addr 0x81` logo apos `render 0`;
  - o processo continua ate o limite de frames com spam de crash handler, mas nao e gameplay.
- `CUP_TEXHALF=256`
  - piorou a curva de memoria;
  - ainda ficou em `progress=0.000` com RSS maior que o perfil 512.
- `GK_VT11COUNT=1`
  - mudou timing/overhead e nao capturou o caminho real desta fase;
  - ate `f=8520`, `vt11 enter=0 exit=0`, `progress=0.000`;
  - run encerrado manualmente a pedido do NextOS, `rc=137`.
- `CUP_GCEVERY=900 GK_GC_ONLY=1`
  - crashou com SIGILL em `il2cpp_gc_collect`;
  - nao usar.
- `GK_MEMLOG_GC=1`
  - crashou com SIGABRT chamando `il2cpp_gc_get_*`;
  - nao usar.
- `CUP_SKIPRESWAIT=1`
  - piorou memoria cedo;
  - nao usar.
- `CUP_LOADYIELD=5000 CUP_LOADYIELD_F=18000`
  - nao ajudou a curva.
- `TER_JOBINLINE`, `TER_JOBWORKERS0`, `CUP_1CORE/TER_1CPU`
  - causaram SIGILL/travas cedo;
  - nao usar.

### Ultimo teste antes da pausa

Comando:

```sh
cd /storage/roms/ports/graveyardkeeper
env GK_RESTART_ES=0 GK_FRAMES=22000 GK_WATCH_SWAP_KB=220000 \
    PC_KICKWORKERS=2 PC_KICKWARM=0 PC_KICKMS=10 \
    CUP_TEXHALF=512 CUP_CTEXHALF=512 \
    GK_LOADSPY=1 GK_NATIVELOADSPY=1 GK_ASYNCPOLL=1 GK_VT11COUNT=1 \
    bash ./run.sh
```

Resultado no log atual:

```text
LoadSceneAsync("scene_main_mobile") em f=23
progress=0.000 ate f=8520
GK_VT11COUNT: vt11 enter=0 exit=0
MEM f=8400 avail=118MB swfree=410MB rss=588MB
run saiu rc=137
```

Conclusao: `GK_VT11COUNT` nao e uma boa proxima linha; ele nao pegou a fase atual e deixou o teste mais pesado/lento.

### Proximo passo recomendado

Retomar no melhor perfil sem `GK_VT11COUNT`, com watchdog ativo:

```sh
cd /storage/roms/ports/graveyardkeeper
env GK_RESTART_ES=0 GK_FRAMES=30000 GK_WATCH_SWAP_KB=220000 \
    PC_KICKWORKERS=2 PC_KICKWARM=0 PC_KICKMS=10 \
    CUP_TEXHALF=512 CUP_CTEXHALF=512 \
    GK_LOADSPY=1 GK_NATIVELOADSPY=1 GK_ASYNCPOLL=1 \
    bash ./run.sh
```

Se precisar cortar mais memoria, investigar antes de mexer:

1. Quem aloca o pico antes de `progress=0.333` usando hooks leves de alocacao ou logs de textura, nao `GK_VT11COUNT`.
2. Por que `CUP_CTEXHALF` nao aparece nos logs; talvez falte hook de `glCompressedTexSubImage2D`.
3. Se os grandes objetos sao texturas CPU-side antes do upload, `gl*` downscale sozinho nao vai bastar.
4. Evitar GC forcado; todos os caminhos de GC testados crasharam.
