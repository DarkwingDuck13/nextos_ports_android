# DuckTales: Remastered → Mali-450 (.164) — STATUS / HANDOFF

**Data:** 2026-06-23  •  **Device:** Mali-450 EmuELEC 192.168.31.164 (senha emuelec), fbdev 1280x720
**Pasta device:** `/roms/ports/ducktales` (dados) + `/roms/ports_scripts/DuckTales.sh` (launcher) — partição EEROMS (mmcblk1p3, 111G).
**⚠️ NUNCA gravar em `/storage` (mmcblk1p2, partição de sistema, 2.3G) — só em `/roms` (=`/storage/roms`, partição grande).**

## RESUMO EXECUTIVO
so-loader armv7 (NDK r17c, NativeActivity) **funciona e inicializa o engine WayForward por completo**:
carrega `libducktales.so` + `libfmodex.so` + `libfmodevent.so`, resolve 401 imports, roda 1013
construtores, `JNI_OnLoad`, cria contexto **GL Mali 1280x720**, sobe o job-system (10 threads) e
**carrega TODO o conteúdo do jogo** (todos os `.pak` + sound banks `.fsb`, incl. `music.fsb` 75MB).

🟡 **MARCO:** com `DUCK_SKIPFILE=music.fsb` (pula o 37º/último asset) **NÃO crasha e CHEGA AO
MAIN LOOP** (polling `hasJoystick` por frame). Mas não renderiza (frames=0): fica em retry-loop
porque o music bank é requisito antes do 1º frame.

🔴 **BLOQUEIO ATUAL:** crash por **corrupção de heap (unlink de free-list de alocador interno
custom, `node->prev=NULL`)** disparado ao carregar **com sucesso** o **37º (último) asset**
(`music.fsb`), **antes do 1º frame**. CHAVE: redirecionar music.fsb p/ um `.fsb` pequeno VÁLIDO
TAMBÉM crasha → **não é conteúdo/tamanho do music.fsb**; é o ato de completar o último load que
toca uma região de heap **já corrompida por um load anterior** (provável **buffer overflow
determinístico** em 1 dos 36 loads — crasha igual com 1, 2 e 10 threads efetivas, então não é
corrida pura). Não é bloqueio externo.

## O QUE FOI RESOLVIDO (cada um destravou um estágio)
1. **memset NULL** — tabela de imports gerada tinha todos os UND com func=0; `so_resolve` ligava 0.
   Fix: `so_util.c` trata func==0 como "não resolvido" → cai no resolvedor (dlsym/shims). 
2. **Helper de gamepad NVIDIA** (`NvGetGamepadAxes/Buttons`) faz reflexão JNI sobre JNI fake → lixo.
   Fix: hook → stub "0 eixos/0 botões" (input real vem por AInputQueue).
3. **Corrupção de heap por pthread bionic↔glibc** (mutex bionic=4B vs glibc=24B, sem bionic=4B).
   Fix: `recon_wire_pthread(dt_set_import)` wirado ANTES dos resolves (faltava ser chamado).
4. **`getDataDirectory()=""`** → `CopyFromAssetManager` copia asset→datadir, `fopen` falhava → `fwrite(NULL)`.
   Fix: jni_shim retorna userdata real p/ getDataDirectory + getDeviceLanguage="en".
5. **`fstat` layout bionic vs glibc** (st_size em offset diferente) → tamanho de arquivo lixo →
   buffer pequeno → parser FILELINK estoura. Fix: `stat_shim.c` traduz glibc→bionic (st_size@48).
6. **Deadlock do job-system** com SEMBREAK off + <10 threads. Causa: o coordenador (`wfMCP::Exec`)
   espera os ready-posts das 10 workers; reduzir threads quebra. **Precisa exatamente 10 threads.**
   Com 10 threads + `RE4_NO_SEMBREAK=1` (espera real, sem force-wake espúrio) carrega tudo.

## O BLOQUEIO (detalhe técnico)
- Backtrace converge em **`wfManagedPackage::RegisterFiles` / `wfFileSystemJob::Run` / um alocador
  em ~`0x939d84`** (file offset). Decodificado (ARM forçado): é um **unlink de free-list duplamente
  ligada** com bins por classe de tamanho — `prev->next=next; next->prev=prev` onde prev/next estão
  corrompidos (NULL ou valor pequeno ~0x3000). Clássico de heap/free-list corrompido.
- Esse alocador (malloc=`0x9392a4`, free=`0x939e04`, ambos chamam o unlink) é **bundle estático
  NÃO thread-safe** (≠ malloc glibc, que o `wfHeap::Alloc` usa). Sob 10 workers, corrompe.
- Crash é **antes de qualquer `eglSwapBuffers`** (frames=0 sempre) → tela preta, sem imagem.

### Tentativas que NÃO resolveram (registradas p/ não repetir)
- **Serializar `wfJob::Exec`** (lock global via trampolim): **deadlocka** — jobs têm dependência
  cross-thread (job A segura o lock e espera job B). Recursive lock idem. (env `DUCK_SERIAL_JOBS`)
- **Lockar o alocador interno** malloc/free (`0x9392a4`/`0x939e04`) com trampolim de 8 args
  (preserva args de pilha — as funções leem `[sp,#76]`): **não eliminou o crash** → há outro
  caminho que mexe na free-list (provável `realloc`/`malloc_consolidate`), OU a corrupção é um
  **buffer overflow** (escrita além de um buffer na metadata do chunk vizinho), não alloc concorrente.
  (env `DUCK_POOLLOCK` — opt-in, default off; usa spinlock recursivo async-signal-safe.)
- **Stack 8MB** nas threads (eram 128KB): não mudou. (env `DUCK_STACK_MB`, default 8)
- **Pular `.fsb`** (DUCK_NOFSB): ainda crasha → não é exclusivo do fmod.
- **Reduzir threads** (`DUCK_JOBTHREADS`): <10 deadlocka (coordenador espera 10).

### Dados que estreitam a causa (medidos)
- **NÃO é OOM:** peak VmRSS = 136MB, device com 558MB livres no crash. O jogo libera os buffers de
  asset após parsear (RSS baixo mesmo com music 75MB).
- **`node->prev == NULL`** no unlink é assinatura clássica de **double-free / use-after-free** no
  alocador custom (≠ glibc; glibc abortaria com "double free"). Provável: algum buffer/recurso é
  liberado 2× nesse pool durante o load (talvez no caminho wfFileReader/CopyFromAssetManager ou no
  parser de um asset). 🔎 PRÓXIMO: gdb watchpoint no campo prev do nó que corrompe, OU hookar o
  `free` do pool (`0x939e04`) p/ logar double-free (mesmo ptr liberado 2×).
- Crasha igual com 1, 2 e 10 threads efetivas (GetCpuCount forçado) → **não é corrida pura**.

## PRÓXIMOS PASSOS sugeridos (em ordem de promessa)
1. **Determinar race vs overflow determinístico:** rodar single-thread REAL até o fim do load.
   O obstáculo é o deadlock do coordenador (espera 10). → achar a contagem esperada em `wfMCP::Exec`
   (espera `sem[0]` em loop) e **patchar a contagem + o spawn juntos** p/ rodar com 1-2 threads.
   Se 1 thread carrega tudo SEM crashar → é race (seguir p/ #2). Se crasha igual → é overflow
   determinístico (achar o buffer; provável no parser FILELINK ou no manejo de evento fmod).
2. **Se race:** lockar TAMBÉM o `realloc`/`consolidate` do alocador interno (achar offsets),
   OU tornar o alocador thread-safe redirecionando suas chamadas internas. Alternativa: liberar o
   lock global de jobs durante os waits (sem_wait/wfEvent::Wait soltam o lock) → serializa o
   trabalho de CPU mas resolve dependências (padrão condvar).
3. **Se overflow:** gdb watchpoint no chunk que corrompe (achar quem escreve o ~0x3000 no campo
   prev/next), subir até a função do engine que estoura.
4. Depois da imagem: áudio (fmod já cai em `org.fmod.FMODAudioDevice`→pulse via jni_shim se
   `dlopen(libOpenSLES)` falhar), input (AInputQueue já mapeado p/ Xbox), empacotar.

## COMO RODAR (device)
```sh
ssh root@192.168.31.164   # senha emuelec
cd /roms/ports/ducktales
systemctl stop emustation
DUCK_LIBDIR=lib DUCK_ASSETS=assets DUCK_DATADIR=userdata \
RE4_GAMEDIR=$PWD RE4_USERDATA=$PWD/userdata RE4_NO_SEMBREAK=1 DUCK_MAXSECONDS=30 \
./ducktales > logs/run.log 2>&1
```
Ou pelo launcher: `bash /roms/ports_scripts/DuckTales.sh` (mata-antes, watchdog, logs persistentes).

## INFRA DE SEGURANÇA (entregue, conforme pedido)
- **Watchdog anti-travamento** no binário: `DUCK_MAXSECONDS=N` força saída; detector de "render
  travado" (force-exit se frames>0 param 15s); heartbeat a cada 5s no log.
- **`ulimit -c 0`** + `setrlimit(RLIMIT_CORE,0)`: SEM core dump (já travou o device 1× enchendo
  partição). core_pattern do device = `|/bin/true` (descarta), mas garantido em 2 camadas.
- **Mata+confirma** instâncias por `/proc/*/exe` antes de cada run (launcher e runner).
- **Logs persistentes com timestamp** em `logs/`: `game_test.log` + split `crash.log`/`audio.log`/`input.log`.
- `run_dev.sh` = runner de DEV (timeout externo + watchdog + screenshot fb0).

## ARQUIVOS (host: ~/nextos_ports_android/ports/ducktales)
- `build.sh` — build armhf (toolchain RE4). `src/` — loader + shims.
- `src/main_ducktales.c` — harness (load fmod+ducktales, JNI, EGL, watchdog, hooks).
- `src/asset_shim.c` (AAssetManager→disco), `src/stat_shim.c` (bionic stat), reusados do RE4:
  `so_util.c egl_shim.c android_shim.c jni_shim.c pthread_shim.c softfp_shim.c opensles_shim.c imports.gen.c`.
- `DuckTales.sh` — launcher PortMaster. `payload/` — APK extraído (libs+assets).
- Binário `ducktales` NÃO versionado (regra). Device IP pode mudar (DHCP).
