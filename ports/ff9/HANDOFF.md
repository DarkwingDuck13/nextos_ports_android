# FF9 (Final Fantasy IX) so-loader → Mali-450 — HANDOFF

> Unity **2022.3.62f3 IL2CPP arm64**, package `com.square_enix.FFIXww.android_googleplay`.
> Metadata **PLAINTEXT** (magic af1bb1fa), **sem pairip / sem DRM** (repack APKVISION).
> Scaffold reusa **Terraria** (`ports/terraria`). Device de teste: Mali-450 / EmuELEC (mesmo do chrono).
> APK: `FINAL-FANTASY-IX-v1.5.4-full-apkvision.apk`.

---

## ESTADO ATUAL (s1) — 🟡 boot completo, trava no handshake do job-system; tela PRETA

Boot vai longe **sem crash**:
- **F0** libunity carregada/relocada, imports resolvidos, init_array (426), **JNI_OnLoad=0x10006** OK.
- **F1** libil2cpp carregada/relocada, init_array (24) OK.
- **F2** `initJni` (lê `bin/Data/boot.config` via AssetManager) → `nativeRecreateGfxState` OK → loop `nativeRender`.
- **GL context REAL criado** (Mali fbdev): Unity loga extensões `GL_OES_depth24...`, carrega "unity default resources".
- **C# rodando**: `ApplicationInfo`, `Company Name: SquareEnix`, playerprefs, `runOnUiThread`, `isUaaLUseCase`.
- **Choreographer doFrame disparando** (driver-thread `TER_CHOREO`): "doFrame começou a disparar".
- ⚠️ `eglChooseConfig EGL_BAD_ATTRIBUTE` 8× nos logs do Unity, **mas o context é criado mesmo assim** (fallback) — não é o bloqueio.

🔴 **MURO**: tela **PRETA** (fb0 = preto sólido, estável em t=30/60/90s). **Deadlock de handshake do job-system no startup.**

### Diagnóstico gdb (decisivo)
- **Thread 1 "UnityMain"** bloqueada em `pthread_cond_wait` chamado de **libunity+0x61efe0**. O loop:
  ```
  61efe0: ldr x8,[x19,#88]   ; x8 = obj[88] (fila/queue head)
  61efe4: cbz x8, 61eff0     ; se 0 -> espera
  61efe8: ldr x8,[x8]
  61efec: cbnz x8, 61f000    ; se *obj[88]!=0 -> prossegue (tem item/pronto)
  61eff0: cond_wait(x21=cond, x20=mutex)
  61effc: b 61efe0           ; loop
  ```
  Função entra em 0x61ee80; faz mutex_lock + chamadas JNI/UnitySendMessage(0xc0a860/0xc0aba8/0xc0b700) ANTES do wait.
- **TODAS as threads de trabalho PARKADAS em futex** (syscall): `Loading.Preload`, `Loading.AsyncRe`, `UnityGfxDeviceW`, 16× `Background Job.`, `Job.Worker 0/1/2`, `GC Finalizer`, 3× `AssetGarbageCol`. → ninguém produz; nenhum job foi despachado. **Deadlock de startup**, não livelock.
- O `[SEM] post` storm visto em logs é o **doFrame** do choreo (cada frame posta um sem); no instante do dump tudo está parado.

### Interpretação
É o **mesmo muro do Terraria/Elderand**: o produtor nativo (job/preload/gfx) do startup não é alimentado pelo nosso ambiente fake → a main espera para sempre uma fila (`obj[88]`) que nunca é preenchida. O doFrame do Choreographer **já funciona** (não é o bloqueio do frame-2 puro); o bloqueio é a **fila de job/preload** do boot.

---

## FIXES APLICADOS (commitados — master, origin+private, commit 0991d99)

1. **`so_util.c` so_load reescrito LAYOUT-AGNÓSTICO.** Unity **2022** põe o **1º PT_LOAD NÃO-executável** (R--), com o segmento RX em vaddr≠0 (libunity RX@0x364890, libil2cpp RX@0xe7291c). O loader antigo assumia executável-primeiro: no `else` fazia `if(text_segno<0) goto err_free_so` com `res=0` → retornava "sucesso" com `text_base=nil` → `so_relocate` crashava em base nula. Além disso `text_base=load_base+text_vaddr` era errado p/ código fora do vaddr 0 (RVAs/relocs não batiam). **FIX**: module base = vaddr 0 = `load_base`; mapeia TODOS os PT_LOAD no `p_vaddr`; `text_base=load_base`; `text` cobre `[0,data_lo)` (RX), `data` = span de TODOS os RW `[data_lo,data_hi)`. (Terraria/2021 era RX-first, por isso nunca apareceu.)

2. **Removido `CUP_GCOFF` do run.sh.** Chamava `il2cpp_gc_disable` no OFFSET hardcoded do Terraria `il2cpp_base+0x73ca6c`, que no FF9 é uma **tabela de dados** (0xffffffff) → executar dados = **SIGILL**.

3. **il2cpp C API resolvida por NOME** (não por offset). `il2_domain_get`/`il2_thread_attach` via `so_find_addr_safe("il2cpp_domain_get"/"il2cpp_thread_attach")` no F1 (módulo ativo = il2cpp). libil2cpp **exporta os 241 símbolos `il2cpp_*`**. O choreo_driver_thread usava `0x73c860`/`0x73ccb4` (offsets Terraria) → SIGILL.

### ⚠️ DÍVIDA TÉCNICA (resolver por NOME quando tocar nessas features)
Ainda há MUITOS offsets Terraria/Cuphead hardcoded em `main.c`/`jni_shim.c` que SÓ disparam em features gated (não ligadas no boot mínimo): `0x73c860` dom_get, `0x73c86c` domain_get_assemblies, `0x73c22c` assembly_get_image, `0x73c264` class_from_name, `0x73c28c` class_get_method_from_name, `0x73cc7c` runtime_invoke, `0x73ca44/0x73ca48` field get/set, `0x73cc80` class_init, etc. E offsets de PATCH em libunity (storage-check `0x2d8fac`, job-system `0x2f37a4`/`0x2f1d48`/`0x2eaacc`/`0xc0da20`, NOGCWAIT `0x74f260`...) são **todos do Terraria 2021** → errados p/ FF9 2022. **Não ligar flags TER_*/CUP_* que usem offset sem reverter o offset do FF9 primeiro.**

---

## COMO RODAR

`run.sh` (env mínimo, fbdev Mali-450): `SDL_VIDEODRIVER=mali`, `TER_NOSTORAGEPATCH=1` (pula o NOP do storage-check 0x2d8fac do Terraria, offset errado p/ 2022), `CUP_NOLOGFILE=1`, `CUP_FRAMES=999999999`, `TER_CHOREO=1`. `-force-gfx-direct -force-gles20` já são injetados por default no cmdline (`cmdline_fd`).

Layout no device: binário `ff9` + `libunity.so`/`libil2cpp.so`/aux no **GAMEDIR root** `/storage/roms/ff9/`; assets em `/storage/roms/ff9/bin/Data/` (concatenar os 38 `sharedassets0.assets.split*` em ordem `ls -v` ANTES de deployar). Lançar foreground.

Scripts de debug (no port): `gdbcrash.sh` (catch SIGILL+regs+bt), `gdbstk.sh` (stack da main), `gdball.sh` (PC de todas as threads). Resolver endereços: `addr2line -e ff9 <off>`; bases libunity/il2cpp saem no log (`libunity: text=`, `libil2cpp: text=`).

---

## PRÓXIMOS PASSOS (próxima sessão)

1. **Atacar o deadlock do job-system de startup** (a main espera `obj[88]` em libunity+0x61efe0; workers todos parkados). Caminhos:
   - RE da função 0x61ee80: descobrir QUEM deveria preencher `obj[88]` (que thread/job) e por que não é despachado. É a fila de Preload/AsyncRead? (threads `Loading.Preload`/`Loading.AsyncRe` estão parkadas).
   - Comparar com a receita Terraria que PASSOU desse muro: `TER_INLINETASK`/`TER_FORCETHREADED`/`TER_SKIPJOBWAIT` patcham o job-system de libunity — mas nos OFFSETS do Terraria. **Achar os offsets equivalentes na libunity 2022 do FF9** (job-scheduler "threaded" flag, WaitForJobGroup, per-object task wait) e re-apontar os patches.
   - Hipótese alt: o flag "threaded" do job-system fica 0 no nosso env (boot.config/capability) → scheduler nunca despacha p/ os workers (que existem e estão parados). Era exatamente o caso do Terraria (`TER_FORCETHREADED`, flag em libunity+0xc0da20). Achar o equivalente FF9.
2. Confirmar que o `eglChooseConfig EGL_BAD_ATTRIBUTE` não vira problema depois (context já cria; revisitar só se o render aparecer torto).
3. Regra: **JAMAIS japonês** — FF9 pode defaultar JP; forçar inglês quando chegar no menu (locale/região).

## Refs
- `ports/terraria` (mesmo loader IL2CPP; HANDOFF tem a saga do job-system 2021).
- `ports/elderand` (mesmo muro de handshake de frame/job no Mali-450; muitas sessões).
- `STUDY.md` (recon original).
