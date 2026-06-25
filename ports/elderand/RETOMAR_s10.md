# RETOMAR ELDERAND — estado salvo (sessão s10, 2026-06-24)

> Pausado a pedido do NextOS. Rumo decidido por ele: **INSISTIR no Mali-450** (binary-patch
> single-threaded gfx), NÃO trocar pro X5M. Logs permanentes em `logs/`. Watchdog sempre ativo.

## TL;DR do que rolou nesta sessão
1. **✅ FEITO E MEDIDO: corte de RAM −89MB.** O so_loader segurava a cópia CRUA de cada `.so`
   (`so_base` = `malloc(so_size)`: libil2cpp 70MB + libunity 21MB) o run INTEIRO sem uso — só
   liberava no `so_unload()` (teardown). Agora libero logo após carregar os módulos.
   **Medido (rss_probe): VmRSS 181372kB → 91896kB (-89MB), VmSwap=0.** As threads passaram de
   **estado D (swap-stall) → S (sleep limpo)** = boot determinístico. (Era o pré-requisito s9/s10.)
2. **🔬 O MURO NÃO é o cond-handshake do s8.** Com gdb fresco (device estável pós-RAM-fix): boot
   chega 100% no render loop (`[render 0]`, roda C# real: prefs/permissions/analytics) e trava num
   **RACE NÃO-DETERMINÍSTICO** no sync de gfx/job — uma run a main trava em `pthread_mutex_lock`
   (libunity+0x2d7950, função de frame-pacing/present), OUTRA run no mesmo cenário trava em `futex`
   cru. GfxDeviceWorker + todos os job-workers no mesmo wait de semáforo (libunity+0xad9d00).
3. **🔴 `-force-gfx-direct` é CÓDIGO MORTO.** A injeção (`cmdline_fd`) só está ligada no `my_open()`
   p/ path "cmdline", MAS o Unity 2021.3.42 **não lê /proc/self/cmdline** (`[CMDLINE]` nunca loga;
   libunity nem importa `openat`). Logo o single-threaded-gfx NUNCA foi entregue → worker MT sempre
   nasce. ⟹ a rota real é **BINARY-PATCH na libunity**, não cmdline.

## Working tree (NÃO commitado — perguntei ao NextOS se commito o RAM fix)
- ✅ `src/so_util.c` + `src/so_util.h`: `so_free_module_temp(so_module*)` — testado, funciona.
- ✅ `src/main.c` (~linha 6280): bloco que libera so_base de unity+il2cpp + `malloc_trim(0)` +
  `rss_probe("antes/apos-free-sobase")`. Testado.
- ✅ `src/main.c` (~3753): `rss_probe()` (lê /proc/self/status). Testado.
- ⏳ `src/main.c` (~5345, antes do `int main`): `eld_thr_census()` + spawn gated `TER_THRCENSUS`.
  **ESCRITO mas NÃO buildado/testado.** Varre /proc/self/task/<tid>/comm+state após N segundos.
- Último binário no device/PC já SEM o census: md5 **59d75f388e6a881b8442e2845d6ba188** (tem RAM fix + rss_probe).
  Pra usar o census precisa **rebuildar** (`./build.sh`).

## PRÓXIMO PASSO EXATO (quando retomar)
1. `./build.sh` (inclui o census novo).
2. Manter ambiente estável sem zram (kernel 3.14 trava com zram): validar swapfile já ativo (`cat /proc/swaps`).
   Com esse caminho, `./run_test.sh` não faz manipulação de swap; objetivo é manter ~512MB de swap do device.
   Se houver troca de configuração no device, confirmar depois do reboot: swap ativa + memória livre razoável.
3. `./run_test.sh 30 "ELD_DEVLIB=1 ELD_NOFATAL_OFF=1 CUP_NOSIGH=1 TER_CHOREO=1 ELD_GCOFF=1 CUP_1CORE=1 TER_JOBLOG=1 TER_THRCENSUS=22"`
4. Cruzar no log: `[JOBTHR] tid=N ... start=libunity+0xXXXX` (nascimento) com
   `[CENSUS] tid=N comm=UnityGfxDeviceW` (nome final) → achar QUAL start-routine é o gfxworker.
   Candidatos atuais: **0x368988** (jobs genéricos), **0xb9ab18** (4×, criadas pré-render — suspeito),
   **0xbbb9a0** (rede, pós-render).
5. Com o start-routine do gfxworker: disassemblar o CRIADOR (o `bl pthread_create` que carrega esse
   start em x2) e achar o ramo **threaded-vs-direct** acima → **binary-patch p/ forçar gfx-direct**
   (main faz GL inline, sem worker, sem handshake). objdump:
   `~/NextOS-Elite-Edition/build.*Amlogic-old.aarch64-4/toolchain/bin/aarch64-libreelec-linux-gnu-objdump`
6. Se gfx-direct destravar → vem o 2º muro: **shader ES3→ES2** (blob URP só tem GLES3x; platform 5
   = GLES20 não existe). Aí: reportar ES3 + shim GLSL ES3→1.00 estilo Mina/Dusklight.

## Infra / lembretes
- Device: `192.168.31.100` (emuelec, fb0 1280x1440). `ssh sshpass -p archr root@192.168.31.100`.
- libs reais: `device_libs_1.3.22/` (libunity md5 8fe7856d). NÃO tem pairip (ELD_PAIRIP=off).
- `run_test.sh [SECS] "ENV..."` faz build+kill(/proc/*/exe)+deploy+run+watchdog+screenshot. Logs em
  `logs/`. Screenshot `logs/frame_*.png` ("uniforme"=preto).
- `run_test.sh` ganhou limpeza forçada no `trap` (INT/TERM/EXIT): termina instâncias remanescentes de
  `/storage/roms/ports/elderand/elderand` mesmo se o host abortar, para reduzir risco de travar o device.
- ⚠️ device REBOOTA sob pressão/wedge → revalidar `swapfile` ativo e espaço; no kernel 3.14, **não usar zram**.
- ⚠️ regra NextOS: matar elderand por /proc/*/exe + confirmar 0 antes de scp/run (run_test já faz).
- Offsets novos (libunity rel. base): 0x2d7950 (main mutex/present), 0x409874 (Mutex::Lock wrapper),
  0xad9d00 (wait semáforo job/gfx), 0x9d5ffc/0x9d6004 (spin Filter1). Bases mudam por run.
