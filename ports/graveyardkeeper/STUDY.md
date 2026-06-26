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
