# HANDOFF Elderand s7 (2026-06-23 tarde) — MURO PAIRIP CAIU DE VEZ; JOGO RODA E RENDERIZA

## 🏆 VIRADA TOTAL: a versão 1.3.22 (PDALIFE) NÃO TEM PAIRIP nos libs nativos
- `device_libs_1.3.22/libunity.so` (md5 8fe7856d) e `libil2cpp.so` (070ea7a4) **NÃO importam
  ExecuteProgram, NÃO têm libpairipcore no NEEDED, só INIT_ARRAY**. O mod PDALIFE removeu o pairip.
  → Virou port Unity IL2CPP NORMAL (igual Terraria/RE4). 6 sessões de muro pairip = OBSOLETO.
- DEPLOYADO no device .100 /storage/roms/ports/elderand: libunity.so+libil2cpp.so 1.3.22 + bin/Data
  completo (data.unity3d 33MB, datapack 16MB, sharedassets, **global-metadata.dat PLAINTEXT af1bb1fa**).
  Fonte: /tmp/base.apk (276MB, a 1.3.22). Re-extrair: unzip base.apk "assets/bin/Data/*" + "lib/arm64-v8a/*".

## FLAGS DE BOOT (atuais, funcionam): `ELD_DEVLIB=1 ELD_NOFATAL_OFF=1 CUP_NOSIGH=1 TER_CHOREO=1`
- ELD_DEVLIB=1: bind dos 12 lazy via eld_lazy_overrides_dev (eld_got_binds.h) + ATIVA SKIPOBB.
- ELD_NOFATAL_OFF=1: NÃO patchear 0x452484/0xf7c354 (offsets da libunity ANTIGA, errados p/ 1.3.22).
- CUP_NOSIGH=1: OBRIGATÓRIO — sem isso a Unity instala handler de SIGSEGV e nosso on_crash não pega.
- TER_CHOREO=1: dispara doFrame (game logic). Sem isso o jogo fica IDLE (espera Choreographer).

## ✅ FIXES APLICADOS s7 (todos compilam, sem regressão):
1. **on_crash RAW dump** (main.c topo): write(2) signal-safe ANTES de tudo (pc/lr/fault unity/il2cpp).
2. 🔑 **pthread TLS**: key_create era shim mas get/setspecific resolviam pro glibc (mismatch) → getter
   TLS @libunity 0x420344 retornava null → crash nativeRecreateGfxState. FIX: patch_got os 4
   (key_create/delete/get/setspecific → sh_*) na libunity E libil2cpp. **DESTRAVOU RecreateGfxState.**
3. **jni_new_object_impl**: jptr_ok() rejeita first_arg lixo (construtor sem-arg ex
   UnityCoreAssetPacksStatusCallbacks.<init>()V → vararg lixo 0x28<<48 → crash s[0]).
4. **playCoreApiMissing → true** (jni_CallBooleanMethodV): Play Core ausente, usa APK.
5. **ExceptionCheck(228) reflete g_jni_exc_pending** (era return 0 fixo); jni_Throw(13) seta pending;
   ExceptionOccurred retorna obj real. (necessário p/ semântica de exceção do OBB.)
6. 🔑 **SKIPOBB**: libunity 0x437c10 (discovery do OBB "main.0.<pkg>.obb", chamada de 0x436a60 com
   retorno IGNORADO) → patch `ret` imediato. O caminho "sem OBB" no so-loader corrompia uma std::local
   (crash 0xbfa610 lendo path como ptr). OBB não existe (igual device real) → pula → usa bin/Data.
   **DESTRAVOU o boot: IL2CPP init completo, runnables, Choreographer setup.**
7. 🔑 **choreo_driver_thread**: usava offsets il2cpp HARDCODED (0x73c860/0x73ccb4 da versão antiga →
   SIGILL). FIX: resolve il2cpp_domain_get/il2cpp_thread_attach por NOME (so_find_addr_safe no
   g_m_il2cpp). 1.3.22 reais: domain_get=0xcdcbd4 attach=0xcdd028.

## 🔴 PAREDE ATUAL: GC do il2cpp crasha durante doFrame (game logic rodando)
Com TER_CHOREO a thread anexa ao il2cpp e dispara doFrame → **a lógica C# RODA** (jogo limpa fb0 p/
preto = assumiu o framebuffer!) → crash **il2cpp+0xcea55c, sig=11 fault=0x1e8**, stack
0xcece40→0xcec1e4→0xce7c04→0xce79c0 (região 0xce7xxx = **Boehm GC / il2cpp_gc**). Em 0xcea534 faz
ubfx x21 + ldr [x10,x8,lsl#3] = lookup em PAGE-TABLE de metadata do GC; entrada = lixo (x8=0x7f00000065)
→ [x8+48] / ldrh → fault. = o GC tenta ler metadata de um endereço que não é objeto gerenciado válido.
HIPÓTESE: doFrame roda Update na thread CHOREO (não-main) → objeto/estado num estado que o GC não
rastreia, OU GC scan da pilha da thread choreo. rc=124 (watchdog) = travou após crash.

## PRÓXIMO (s8) — em ordem de aposta:
1. **Driver de doFrame na thread MAIN** (não thread separada): chamar jni_choreo_doframe no loop de
   render (após nativeRender) — game logic na main (já il2cpp-attached + thread do GL). Evita GC
   cross-thread. (testar: remover create do choreo thread, chamar doframe inline no loop ~6596.)
2. Se persistir: investigar o GC. Pode precisar do GC desabilitado no início (CUP_GCOFF mas com
   offset CERTO p/ 1.3.22: il2cpp_gc_disable real via so_find_addr "il2cpp_gc_disable") OU
   TER_NOGCWAIT com offset certo de WaitForThreadsToSuspend 1.3.22.
3. ⚠️ TODOS os offsets hardcoded no loop de render (0x1b62ad4, 0x73ca5c, 0x8733a8, 0x74f260, 0x2f37b0...)
   são da Cuphead/Terraria = ERRADOS p/ 1.3.22. Trocar por so_find_addr por nome OU recalcular se usar.

## INFRA (NextOS pediu): run_test.sh = harness com LOGS PERSISTENTES + WATCHDOG
- `bash run_test.sh [SECS] "ENV1=v ..."` → build (logs/build.log) + mata+confirma + deploy + run com
  timeout/watchdog + heartbeat 5s + screenshot fb0→PNG (logs/frame_*.png) + classifica crash.
- logs/: game_test.log (tudo, timestamp), crash.log, audio.log, input.log, build.log.
- Watchdog: timeout duro no run + WD externo; mata processo travado; nunca pendura o device.
- Verificação visual: logs/frame_*.png (Read tool). ⚠️ tela preta="uniforme (0,0)"; ES menu=conteúdo.

## DEVICES
- Mali-450 alvo: 192.168.31.100 (emuelec, fb0 1280x1440). ssh sshpass -p archr root@192.168.31.100.
- Bancada Moto G100 (adb 0074124494, root frida): app real com.pid.elderand roda; OBB dir VAZIO
  (confirma: sem OBB no device real). libunity bench == device_libs (8fe7856d).
- Toolchain objdump: ~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/bin/aarch64-libreelec-linux-gnu-objdump

## 🔄 ATUALIZAÇÃO s7 (cont.) — boot COMPLETO até render loop; trava no Choreographer
Fixes ADICIONAIS aplicados (todos no binário atual):
8. **sysconf p/ il2cpp**: patch_got("sysconf",my_sysconf) no bloco il2cpp. Era o `ConcurrentDictionary
   concurrencyLevel must be positive` (Environment.ProcessorCount=0 pq glibc sysconf interpretava a
   constante BIONIC 96/97 errado). DESTRAVOU a serialização (NodeCanvas/ParadoxNotion).
9. **Addressables aa/**: o jogo carrega `assets/aa/` (catálogo Addressables, 7447 arq/138MB) — NÃO
   estava deployado → "RemoteProviderException settings.json / Addressables unable to load runtime data".
   DEPLOYADO em /storage/roms/ports/elderand/aa/ (du=347M por overhead de cluster FAT; conteúdo 138M;
   Android=220 bundles + catalog.json + settings.json). asset_open + asset_redirect agora normalizam
   URIs "jar:file://<apk>!/assets/aa/..." → ASSET_BASE/aa/... **DESTRAVOU Addressables** (erro sumiu).
10. **ELD_GCOFF**: desabilita GC do il2cpp no boot via il2cpp_gc_disable por NOME (0xcdcde0). A thread
    choreo dirige doFrame→Update→aloca→GC stop-the-world escaneava threads→crash il2cpp+0xcea55c.
    Com GCOFF: **SEM crash**, "[CHOREO] doFrame começou a disparar".

## FLAGS ATUAIS (boot mais longe): `ELD_DEVLIB=1 ELD_NOFATAL_OFF=1 CUP_NOSIGH=1 TER_CHOREO=1 ELD_GCOFF=1`
(ELD_CHOREO_MAIN NÃO usar junto — a main trava na render, não dá p/ dirigir doFrame inline.)

## 🔴 PAREDE ATUAL: nativeRender frame 2 TRAVA no Choreographer (tela preta, sem crash, rc=124)
- Boot 100% até o render loop. Roda lógica REAL (Elderand.UI.AudioSettings, Odin Serializer, VoxelBusters).
- FMOD NÃO-fatal (jogo segue) mas erra: `FMOD5_Memory_GetStats not found in 'fmod'` → my_dlopen("libfmod.so")
  cai no glibc dlopen (falha, .so bionic)→g_dl_self→dlsym não acha. FIX futuro: carregar libfmod via
  so_loader (NEEDED: liblog/libstdc++/libm/libdl/libc) + my_dlsym resolver FMOD5_* nele. (áudio)
- ⚠️ `Desired shader compiler platform 5 is not available in shader blob` (repetido) = problema de
  GL/shader (provável render preto). Investigar DEPOIS de destravar o frame.
- 🔑 **TRAVA**: a main, dentro de nativeRender frame 2, monta o Choreographer (HandlerThread
  "UnityChoreographer"+Looper+Handler), posta Message (obtainMessage/sendToTarget→jni_handlemessage
  dirige handleMessage→postFrameCallback) e BLOQUEIA num cond NATIVO (libunity+0x2f3680 — offset da
  versão ANTIGA, confirmar p/ 1.3.22) esperando o doFrame sinalizar. A thread choreo (TER_CHOREO)
  captura o FrameCallback e dispara doFrame, MAS a main não desbloqueia. HIPÓTESES p/ s8:
  (a) o doFrame da thread choreo não chega no delegate C# real / não sinaliza o cond nativo;
  (b) o cond que a main espera está noutro offset na 1.3.22 (o 0x2f3680 é Cuphead/Terraria);
  (c) precisa rodar doFrame na thread DONA do Looper (HandlerThread), não numa thread arbitrária;
  (d) rendering tem que ser na MAIN (contexto GL), doFrame/Update na choreo — sincronizar.
  PRÓX: SIGUSR1 no processo vivo → diag_handler dumpa backtrace da main → achar o cond exato onde
  trava na 1.3.22; achar quem sinaliza; OU achar como o Unity 2021.3 Android entrega o 1º doFrame.

## 🎯 DIAGNÓSTICO PRECISO DA TRAVA (fim s7) — main em FUTEX raw esperando o doFrame
- Backtrace da main (SIGUSR1 + /proc/PID/stack): bloqueada em **`my_syscall` chamando syscall #98 =
  FUTEX_WAIT** (libunity faz futex RAW, não pthread_cond). base binário/run via /proc/PID/maps;
  lr cai em my_syscall @elderand+0x32aec (bl syscall@plt logo após `mov x0,#0x62`).
- A thread choreo (TER_CHOREO) dispara doFrame (`jni_choreo_doframe`→`invoke` do JNIBridge), MAS
  **o `handleMessage` NÃO é re-dirigido depois** (grep handleMessage=1) → o delegate C# do
  FrameCallback.doFrame NÃO está rodando de fato → ninguém faz FUTEX_WAKE no addr que a main espera.
- handleMessage roda 1× (síncrono via sendToTarget) e depois para; o jogo postou o FrameCallback de
  vsync e espera o doFrame. **Fix s8 = fazer o `invoke(FrameCallback_proxy, doFrame_method, args)`
  EXECUTAR o delegate C# real** (que faz o wake). Investigar o dispatch do JNIBridge.invoke nativo
  (jni_find_native("invoke")) com nosso Method/args sentinel de doFrame: confirmar getName→"doFrame",
  o handle do proxy, e se os args (frameTimeNanos como Long boxed) estão certos. Comparar com o
  handleMessage que FUNCIONA (mesmo invoke, método diferente). Logar dentro do invoke se o delegate roda.
- Alternativa: achar o uaddr do futex que a main espera (instrumentar my_syscall p/ logar uaddr quando
  op=FUTEX_WAIT vindo da libunity) e ver quem deveria acordá-lo; pode ser o render-thread sync do
  GfxDevice (multithreaded rendering) e não o Choreographer — nesse caso destravar é desligar MT
  rendering (1 core via my_sysconf já dá 4; testar CUP_1CORE=1 → sysconf 1 core → Unity roda inline).

## ⚡ TESTE RÁPIDO P/ s8: CUP_1CORE=1 (talvez destrave sem mexer no Choreographer)
Se a trava for o render-thread/job sync (MT), `CUP_1CORE=1` faz my_sysconf reportar 1 core → Unity
desliga multithreaded rendering/jobs → roda tudo na main → sem espera de worker. TESTAR PRIMEIRO no s8.
