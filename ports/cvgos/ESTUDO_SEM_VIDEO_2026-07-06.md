# Por que ainda não temos vídeo no CVGoS — estudo 2026-07-06 (Claude)

## TL;DR
O caminho de vídeo (EGL/Mali/fb0) está OK. Não há imagem porque o boot managed (C#)
nunca completa. São DOIS muros empilhados, e o estado "bom" documentado no STATUS
**não é reproduzível com o binário atual** — o binário foi buildado e enviado na pausa
SEM validação; os runs bons eram do build anterior (working tree tem ~2.7k linhas
não-commitadas desde d3a4ac1, não testadas em conjunto).

## Evidência (runs de diagnóstico desta sessão, device .90, foreground via ssh)
Script: `/storage/roms/ports/cvgos/diag_claude.sh` (env base do STATUS + THROWLOG/I2SYM/IL2CPPSTACK).
Logs mantidos no device: `diag_nre.try2.log`, `diag_nre.try3.log`,
`diag_try1_trapspin_tail.log`, `diag_try4_trapspin_tail.log`, `diag_try5_trapON_pc0.log`.

1. **Path natural (CVGOS_NOTRAPPATCH=1, como no run.sh)**: boot chega a `[render 0]`/`[r1>`
   mas `main thread is trapped; signum = 1` desde ANTES do RecreateGfxState (530k+ ocorrências)
   e o loop gira vazio: **580k frames com draws/f=0**, nenhum asset carrega. O fix do SIGHUP
   ([SIGHUP] ignorado+bloqueado roda na linha 3 do log) **já não zera o trap** neste binário —
   regressão ou o "1" nunca foi SIGHUP de verdade neste fluxo.
2. **Trap-patch ON (sem NOTRAPPATCH, com/sem CRASHSKIP)**: morre SEMPRE no mesmo ponto,
   logo após `MakeCurrent #1 PBUF(worker)` + `[SEM] wait ... count=0`:
   **chamada de ponteiro de função NULL (pc=0x0)** numa thread worker; o SDL3 do device
   pega o SIGSEGV no handler fatal dele (libSDL3+0x16e3xx = restaura console + `raise()`)
   e re-lança. try2/try3/try5 idênticos → determinístico, não flaky.
3. Nenhuma combinação chegou ao fluxo `[SPARKGEAR]` do "último ponto bom" do STATUS.

## O muro "oficial" (NRE do SPARKGEAR) — raiz mapeada offline
- SPARKGEAR = **SPFX, sistema de VFX interno da Konami** (plugin nativo `libSPFXUnityPlugin.so`
  + shaders compilados `shader.gles2.vfxsb`), NÃO é analytics. O loader hoje não trata
  essa lib (P/Invoke `SPFX_PLUGIN_*` vai falhar depois — muro futuro).
- Dump IL2CPP gerado em `~/tmp/cvgos-il2cpp-dump/` (dump.cs/script.json/DummyDll, via
  Il2CppDumper + dotnet 9 local `~/.dotnet/dotnet --roll-forward LatestMajor`).
- O NRE nasce em **`ResourcesPath::get_persistentDataPath()` (RVA 0x3970340)**
  (classe TypeDefIndex 6515; irmã `get_temporaryCachePath` RVA 0x3970850, `.cctor` 0x3970D60).
  Disasm: faz `currentActivity → getFilesDir → getCanonicalPath` via AndroidJavaObject e
  **lança NullReferenceException (bl 0x3cf1290) se QUALQUER resultado managed vier null** —
  inclusive a string do getCanonicalPath. O throw visto no STATUS
  (`throw_helper #2 ... lr=libil2cpp+0x7a8b70`) é exatamente esse null-check.
- No shim, `getFilesDir` retorna `make_jstring(path)` (um "File" que na verdade é string
  fake). A conversão managed dessa cadeia (libunity AndroidJNI → il2cpp) produz null em
  algum passo — instrumentar com o diag acima QUANDO o estado bom voltar a rodar.

## Como resolver (recomendação, em ordem)
1. **Recuperar o estado bom primeiro**: commitar/stashar o delta atual, rebuildar do
   d3a4ac1 (binário não é versionado — o build antigo se perdeu!), confirmar que o run
   bom volta (render 0 → assets → SPARKGEAR), e só então reaplicar os guards novos um a um.
   O delta não-commitado inclui coisas invasivas (fake managed Thread em
   `cvgos_patch_thread_current`, wrap de FILE* bionic, patches de cctor) — candidatos
   prováveis do pc=0 e/ou do trap=1 permanente.
2. **Matar o muro do NRE na raiz, sem depender do JNI**: il2cpp-patch (estilo MMX_ILPATCH)
   em `ResourcesPath::get_persistentDataPath` (0x3970340) e `get_temporaryCachePath`
   (0x3970850) para retornar direto `il2cpp_string_new("/storage/roms/ports/cvgos/userdata")`
   (e `/userdata/cache`). Hook de 8 bytes no prólogo (arm_hook8 já existe) → função C que
   devolve a string managed cacheada. Elimina toda a cadeia
   currentActivity/getFilesDir/getCanonicalPath de uma vez, determinístico.
3. Depois do NRE: preparar o **SPFX nativo** — ou so_load da `libSPFXUnityPlugin.so`
   (armv7, mesmo mecanismo dos outros .so) resolvendo os `SPFX_PLUGIN_*` no il2cpp, ou
   stub de todos os `SPFX_PLUGIN_*` retornando "sem VFX" (jogo deve tolerar — o próprio
   código loga "Load SPFXConfig is Faild..." e segue). Stub primeiro = mais barato.

## ATUALIZAÇÃO (mesma sessão) — fix do NRE implementado + muro real reposicionado
- ✅ **Fix do NRE IMPLEMENTADO e no binário** (`CVGOS_RESPATHFIX`, default ON; desliga com
  `CVGOS_NORESPATHFIX`): `patch_i2_respath_fix()` em main.c hooka (arm_hook8) os getters
  `ResourcesPath::get_persistentDataPath` (il2cpp+0x3970340) e `get_temporaryCachePath`
  (0x3970850) → retornam string il2cpp real via `il2cpp_string_new` (= il2cpp+**0x79e4b4**,
  NÃO o 0x1b62c38 do Cuphead que estava no código de streamingAssets). Builda OK, deploya,
  hook instala (`[RESPATHFIX] ... hookados`). **Binário atual no device já tem o fix**
  (md5 `101a4b32...`). Está no working tree, NÃO commitado.
- 🧱 **PORÉM o NRE NÃO é o muro atual — está DOWNSTREAM de um muro upstream não passado.**
  Rodei 3 configs com o fix:
  - Natural (`CVGOS_NOTRAPPATCH=1`): il2cpp_init COMPLETA (global-metadata carrega, domain
    criado) mas o **trap-gate** (libunity+0x31fe38) faz `nativeRender` bailar todo frame →
    PlayerLoop nunca roda → **draws/f=0 pra sempre**, cena managed nunca executa → getter
    nunca chamado. Tela preta = engine viva, boot managed parado no gate.
  - Gate forçado (patch ON) + fakes de thread desligados
    (`CVGOS_NO_THREADCURRENT_PATCH/NO_RUNTIMETYPE_PATCH/NOIL2CPPATTACHMAIN`): morre
    **determinístico** logo após `MakeCurrent #1 PBUF(worker)`, precedido de
    `[I2-RGCTX] slot idx=4 veio NULL -> fallback[4]=0x17a3e0` → SIGSEGV numa worker thread.
- 🎯 **MURO REAL ATUAL = resolução de rgctx / metadata genérica do il2cpp** (o guard
  default-ON `patch_i2_rgctx_slot_guard`, hook em **il2cpp+0x79f51c**, devolve fallback
  CHUTADO quando um slot de contexto genérico vem NULL → corrompe a worker → crash). É o
  **muro "classe FF9" de il2cpp-init/metadata** já citado na memória. Enquanto os slots de
  rgctx vierem NULL (metadata genérica não totalmente wired), NENHUMA config passa da init
  de gfx da worker → o boot managed (cenas, SPARKGEAR, e o meu NRE) fica inalcançável.

### Ordem REAL dos muros até o vídeo (revisada)
1. **[ATUAL] rgctx/metadata genérica NULL** (il2cpp+0x79f51c) — worker crasha na gfx-init.
   Raiz: registration/metadata genérica do il2cpp incompleta. Precisa RE do `il2cpp_init`/
   `Il2CppMetadataRegistration` (por que `Il2CppRGCTXData` slots vêm NULL). NÃO resolver com
   fallback chutado (o atual 0x17a3e0 corrompe). Muro multi-sessão.
2. trap-gate (libunity+0x31fe38) — se rodar sem crash, ainda gateia o PlayerLoop; achar o
   setter de `curthread+0x104` (o gate COPIA curthread->trap p/ player->trap; disasm em
   `ESTUDO`) e satisfazer o registro da thread em vez de forçar.
3. **[JÁ RESOLVIDO no código] NRE ResourcesPath** — `CVGOS_RESPATHFIX` cobre.
4. SPFX nativo (`SPFX_PLUGIN_*` / libSPFXUnityPlugin.so).

## SESSÃO 2 (2026-07-06 madrugada) — 4 muros DERRUBADOS, boot avança MUITO
Descoberta-chave: **o runtime il2cpp está SAUDÁVEL** (probe `CVGOS_I2HEALTH`: corlib=mscorlib,
System.String/Thread/Object resolvem, 43 assemblies OK) — o rgctx NULL e o managed=NULL eram
sintomas, não a raiz. Os muros reais eram bugs de shim/config, corrigidos em sequência:

1. ✅ **CUP_GFXARGS sobrescrevia -force-gfx-direct** — o run.sh/diag setavam `CUP_GFXARGS="-force-gles20"`,
   o que DROPA o `-force-gfx-direct` do default → Unity multi-threaded → GfxDeviceWorker crashava não-
   deterministicamente. Corrigido p/ `"-force-gfx-direct -force-gles20"`. (⚠️ mas ver muro #5: a injeção
   via /proc/self/cmdline NÃO está sendo lida pela Unity 2018.4 — provavelmente args vêm do Intent Java.)
2. ✅ **JNI UTF-16 strings faltando** (RAIZ do stack-overflow no GfxDevice init) — `GetStringLength`(164),
   `GetStringChars`(165), `ReleaseStringChars`(166), `NewString`(163) eram `jni_stub`(=0). Unity lia o
   persistentDataPath como UTF-16 via GetStringChars→NULL/0 → path corrompido. Implementados corretamente
   (jni_shim.c, zero-extend ASCII↔UTF-16). Passou o GfxDevice init → **GL extensions logam, entra em código managed**.
3. ✅ **readdir: layout bionic vs glibc** (RAIZ da recursão infinita de path) — libunity é bionic e lê
   `struct dirent` com `d_name` no offset **19**; glibc 32-bit `readdir` põe em **11** → Unity lê lixo no
   lugar de "." → NÃO pula a entrada "." em DeleteDirectoryRecursive → **recursa no próprio dir → stack overflow**.
   Fix: `my_readdir`/`my_readdir_r` repackam via `readdir64` p/ layout bionic (main.c, set_import+patch_got).
4. ✅ **Thread.get_CurrentThread null** — `il2cpp_thread_attach` volta managed=(nil); `Thread.CurrentThread`
   crashava em `UnitySynchronizationContext.InitializeSynchronizationContext`. FIX = patch `cvgos_patch_thread_current`
   (default-ON) que já existia — só precisava NÃO estar desligado por env.

**Progresso: de "spin vazio / crash de worker aleatório" → boot passa GfxDevice init, GL, código managed,
processamento de path, sync-context.** Todas as correções no working tree (não commitado).

### 🧱 MURO ATUAL (fim s2) — call a ponteiro-lixo 0xffffbfd8 num worker de gfx (DETERMINÍSTICO)
Após MakeCurrent#1 PBUF(worker) + `[SEM] wait count=0`, uma **worker thread** (tid ≠ main) faz `blx` p/
**0xffffbfd8** (ponteiro de função não-inicializado) → SIGSEGV. Determinístico (2 runs idênticos, lr muda só
o base ASLR: 0xf6e52e30/0xf7012e30). CRASHSKIP pula 1× mas morre logo após. Handler só imprime o header
(fault=pc=0xffffbfd8 não-mapeado → o próprio handler re-faulta ao ler pc_target).
- **Causa provável**: gfx multi-threaded (o `-force-gfx-direct` NÃO pega — muro #5 abaixo). O GfxDeviceWorker
  ou uma job-thread chama um fn-ptr per-thread que não foi setado no so-loader.
- **PRÓXIMO (ordem)**: (a) fazer o `-force-gfx-direct` REALMENTE pegar — a Unity 2018.4 Android NÃO lê
  /proc/self/cmdline (o `[CMDLINE] injetado` nunca loga; `grep cmdline`=0). Args vêm do Java (Intent
  `getStringExtra("unity")` / `nativeInjectEvent`) — achar/forjar esse canal no jni_shim, OU patchar direto
  a decisão de MT-rendering / o spawn do GfxDeviceWorker no libunity. (b) OU dar full-dump do 0xffffbfd8:
  guardar o handler contra pc não-mapeado (ler pc_target só se addr_readable) p/ ver o caller e o fn-ptr.

### Ordem REAL dos muros (revisada s2)
1..4 ✅ RESOLVIDOS (acima). 5. **[ATUAL] -force-gfx-direct não pega → gfx worker chama fn-ptr lixo 0xffffbfd8**.
6. [já resolvido no código] NRE ResourcesPath (`CVGOS_RESPATHFIX`). 7. SPFX nativo (`SPFX_PLUGIN_*`).

### Config de teste que chega mais longe (diag_gate.sh no device)
Gate FORÇADO (sem NOTRAPPATCH) + `-force-gfx-direct` + SDL_NO_SIGNAL_HANDLERS=1 + todos guards default-ON
+ CVGOS_I2HEALTH. Env: `CUP_GFXARGS="-force-gfx-direct -force-gles20" CUP_1CORE=1 CUP_NOSIGH=1
SDL_VIDEODRIVER=mali CUP_VIDEO=kmsdrm CUP_GLES_MAJOR=2 CVGOS_ARRAYSKIP/TMPFONTGUARD/VECGUARD/ENUMNULLGUARD`.
Binário atual no .90 = md5 467a4d58 (tem TODOS os fixes s2). Logs: `/storage/roms/ports/cvgos/diag_gate*.tail.log`.

## SESSÃO 3 (2026-07-06 tarde/Claude) — DUAS RAÍZES DERRUBADAS: unity-args via Bundle + sigaction LP32

### Muro #5 (gfx-args) RESOLVIDO — canal real da command line
A Unity 2018.4 Android NÃO lê /proc/self/cmdline. Ela lê `activity.getIntent().getExtras()`
e chama **`Bundle.containsKey("unity")`** (GATE: se false, nunca lê) + **`Bundle.getString("unity")`**.
O jni_shim devolvia containsKey=0 no fallback → engine nunca via os args. FIX (jni_shim.c):
- `containsKey("unity")` → 1 (log `[UNITYARGS]`; desliga com `CVGOS_NOUNITYARGS`)
- `getString("unity")` → valor de `CUP_GFXARGS` (default `-force-gfx-direct -force-gles20`);
  retornar ANTES do va_arg do default (Bundle.getString tem 1 arg; ler 2º = lixo).
Confirmado: `GetStringUTFChars -> " -force-gfx-direct -force-gles20"`.
⚠️ Com gfx-direct a MAIN é a render thread → precisa **`CUP_MAINGL=1`** (senão o egl_shim
dá pbuffer pra main e a window fica preta).

### 🩸 RAIZ DO 0xffffbfd8 + exit 158 + deadlock GC: my_sigaction usava layout bionic ARM64
`struct sigaction` bionic **LP32 (armv7)** = `{ handler; mask(4B); flags; restorer }` —
**HANDLER PRIMEIRO**. O shim usava o layout LP64 `{ flags; handler; ... }` → quando o
**Boehm GC do il2cpp** instalava o handler de SIGPWR(30), o campo lido como handler era a
**sa_mask** (sigfillset−{alguns}) = `0xffffbfd8`!! Na 1ª coleta, SIGPWR → jump p/ 0xffffbfd8
= o "crash de fn-ptr lixo no worker". Variantes: exit 158 (SIGPWR ação default matava o
processo), dump do on_crash truncado (processo morria no meio), deadlock `[SEM] wait`
libil2cpp+0x7dfd88 (= GC_stop_world esperando acks que nunca vinham). O mesmo bug já tinha
sido visto como "handler corrompido 0x7f10000004" (comentário no patch_got do sigaction).
FIXES (bionic_shims.c): layout LP32 correto em `my_sigaction` + **`my_sigsuspend`** real
(mask bionic 4B→glibc; o stub fazia a thread "suspensa" do GC continuar rodando = coleta racy).
**CUP_GCSIG/CUP_GCSUSP ficaram OBSOLETOS** (não usar; Boehm instala os handlers reais dele).

### Estado após os fixes (diag_gate5, 420s)
- `[render 0]` → `[r1>` → engine viva, MediaRouter/input setup, GC stop-world completa.
- I2-RGCTX guard instalado mas **NUNCA disparou** → o "muro rgctx/metadata genérica" ERA
  sintoma da corrupção de sinais. MURO MORTO.
- Novo ponto de morte (7 min in): `ArrayTypeMismatchException` em
  `RuntimeType::CreateInstanceForAnotherGenericParameter+0x9c` (ARRAYSKIP engoliu com
  fallback chutado) → NRE em **`LogResources$$OnAfterDeserialize`** (RVA 0x31F078C, campo
  keys/values null na desserialização) → Il2CppExceptionWrapper não pego → `terminate` →
  abort. Suspeita: os guards da era corrompida (ARRAYSKIP/VECGUARD/ENUMNULLGUARD/
  TMPFONTGUARD) agora fazem mais mal que bem — diag_gate6 roda SEM eles.

### SESSÃO 3 (continuação) — SPARKGEAR ULTRAPASSADO; cadeia de guards nova
Sequência validada (diag_gate18/19): render 0 → `[LOGRES] SKIP (keys/values null)` →
`[VECGUARD] count=float 1.0 → 0` ×3 → `[ENUM-UT] ICanvasElement parent null → devolve Type`
→ **`[SPARKGEAR] Load SPFXConfig ==> SPFXConfig.asset` → "is Faild..." (jogo tolera)** →
muro: `MonoCustomAttrs.IsDefined` lança NRE e o RAISE crasha no invoker
(il2cpp+0x9251bc/0x9256a4 = thunks de Runtime::Invoke) com `method->methodPointer=NULL`
p/ `NullReferenceException..ctor` (existe @0x117c7c8 no binário — o MethodInfo runtime
vem sem ponteiro = lazy metadata-init de método não completou).

RE novo (correções ao entendimento antigo):
- il2cpp+**0x79f51c** = `Thread::GetThreadStaticData(slot)` (era rotulado "rgctx"!).
  [0x4be5410]=lista de threads; [thr+8]=InternalThread; [internal+0x3c]=tabela; tabela[slot]
  = ponteiro do BLOCO de statics-de-thread. il2cpp+0x79f3f4 = registrador (aloca p/ todas
  as threads da lista). Fallback correto = bloco 4KB ZERADO por slot (zero = estado inicial
  legítimo de thread-static). Attach-retry não resolve (main já attachada; slot falta mesmo).
- il2cpp+**0x7753xx** (I2-METAFLAG) = builder de GC-descriptor: seta bits por field-offset;
  com offset lixo (às vezes ponteiro absoluto) o bit-OR cai em endereço arbitrário —
  quando a página é GRAVÁVEL, CORROMPE SILENCIOSAMENTE (só o caso não-gravável era pego).
  Provável fonte do bit `enumtype` ligado em `UnityEngine.UI.ICanvasElement` (interface!).
- `EqualityComparer<T>.CreateComparer` → branch enum com tipo não-enum → 
  `Enum.GetUnderlyingType` → vm hash(type=NULL) crash. Guard: parent≠"Enum" → retorna o Type.
- `Exception::FromNameMsg` (0x7a96f0) lê [info+4] sem null-check; thunks chamam com
  info=NULL. Guard: buffer zerado.
- Invokers il2cpp+0x9251xx-0x9256xx: `mov r3,r0; mov r0,r2; blx r3` etc. — pc=0 = 
  methodPointer NULL vindo do MethodInfo.

PRÓXIMOS (ordem sugerida): (1) testar caminho natural CVGOS_NOTRAPPATCH=1 (gate20) — com
sigaction correto o SIGHUP-trap pode ter sumido e os patches invasivos do libunity
(TRAPGATE/SKIPERRFUNC) podem ser a causa do metadata-init incompleto; (2) se persistir,
hookar o lazy-init de método (flag+bl init no prólogo il2cpp) p/ logar falhas; (3) guard
no invoker: methodPointer NULL → resolver via RVA do dump (temos script.json completo!);
(4) raiz do GC-descriptor spray (guard preventivo: offset > instance_size → skip).

## SESSÃO 3 (continuação Fable) — cena do Título deserializa; muro = metadata-usage init

Progresso: passou SPARKGEAR e deserializa a cena do Título INTEIRA (TitleSerializedResources,
TMPro.TextMeshProUGUI, UnityEngine.UI.Image/Text/RawImage, mvTweenAlpha). Morre no
scan de atributos/Awake das MonoBehaviours.

### Diagnóstico decisivo (SIZEPROBE) — o layout de runtime está CORRETO
`CVGOS_SIZEPROBE` chama `il2cpp_class_instance_size` (0x79de40) em builtins:
`UnityEngine.UI.Text=124 (0x7c)` — CASA o dump (campos até 0x78 +4). Image=0x8c,
RawImage=0x78, MonoBehaviour=0xc. Logo **NÃO há bug de Class::SetupFields/pointer-size**
no nosso runtime. Os avisos `[ALOG:6] "A scripted object ... different serialization
layout (Read 68 bytes but expected 128)"` são **RED HERRING TOLERADO** (asset skew do
repack do mod; Unity lê o que tem e zera o resto). OBB não tem o title (level0/
sharedassets vêm do apk).

### O crash REAL: MethodInfos de slots de metadata-usage não-inicializados
Reflection-invoke (Runtime::Invoke 0x78adfc; blx em 0x78ae6c, retorno 0x78ae70) recebe
MethodInfo CORROMPIDO: `methodPointer`(off 0) aponta pro heap MANAGED, `invoker`(off 4)
= throw-stub (il2cpp+0x7a8b58, o "raise NRE" que o il2cpp usa p/ método sem corpo).
`blx` pula pra dado → SIGILL/SIGSEGV com pc na região rwx do heap de metadata
(sempre os mesmos slots il2cpp+0x4df8d8 / 0x4e3858 nos frames = `s_Il2CppMetadataUsages`).
Ou seja: o método é resolvido via um slot de USAGE que nunca foi preenchido.

### Guards/firewall desta sessão (todos default-ON salvo nota)
- **INVOKE FIREWALL** (on_crash; `CVGOS_NOINVOKEFW` off): se `lr==il2cpp+0x78ae70` (retorno
  do blx do Runtime::Invoke) e `pc` fora do .text REAL (il2cpp [base,base+0x4706b80) ou
  libunity; o heap rwx de metadata NÃO conta como código) → `pc=lr; r0=0` = invoke devolve
  null. FUNCIONA: pula ~6 invokes quebrados. PORÉM pular o Awake deixa objeto meio-init →
  crash secundário (lr=0x9, estado managed destruído). Firewall sozinho não basta.
- **RT-INVOKE** (0x78adfc): valida methodPointer/invoker ∈ .text; rejeita heap+throw-stub.
- **METH-INIT** (0x78af68): pós-init, invoker LIXO/throw-stub → no-op stub; reescreveu o
  thunk invoker 0x9256a4 com null-check tail-call (`cmp r0,#0; bxeq lr; mov r3,r0; mov r0,r2; bx r3`).
- **INV0/ISDEF/ENUM-UT/EXC-FROMNAME/LOGRES**: cadeia menor pós-SPARKGEAR.

### PRÓXIMO (real) — atacar a raiz, não os sintomas
1. **RE de `MetadataCache::InitializeMethodMetadata` / `il2cpp_codegen_initialize_method`**:
   por que os slots de `s_Il2CppMetadataUsages` não são preenchidos. Achar a tabela de
   usages (metadataUsagePairs/tokens) na registration e conferir se está wired. Provável:
   o codegen-init de método (que popula usages no 1º uso) não está rodando, ou a metadata
   de usages não foi registrada no `il2cpp_init` (relacionado ao S_MetadataRegistration
   já mapeado offline: file 0x4707eb8).
2. Alternativa pragmática: FORÇAR `il2cpp_runtime_class_init` + resolução de todos os
   métodos das classes da cena do Título ANTES do Awake (pré-warm), OU forçar
   `il2cpp_codegen_initialize_method` de cada método usado.
3. Se metadata-usage for irreparável: tentar APK STOCK (não-mod) — pode ter metadata+assets
   consistentes (memória: stock de OUTRO jogo crashava, mas cvgos stock é não-testado).

## Notas de ambiente
- O env "base recomendado" do STATUS está incompleto: as sessões boas rodavam num shell
  interativo que ainda tinha exports acumulados (CRASHSKIP/NOTRAPPATCH do run.sh etc.).
  Documentar SEMPRE o env completo (`tr "\0" "\n" < /proc/PID/environ`) no próximo run bom.
- emustation.service no .90 está MASCARADO (symlink /dev/null, 3/jul 02:41) — pré-existente.
- Launch de diagnóstico: ssh FOREGROUND (sessão viva), `nice -n 19 timeout N ./cvgos`.
