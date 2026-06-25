# HANDOFF Elderand — LER PRIMEIRO (próxima sessão pós-/clear)

## 🏆🏆🏆 s6 (2026-06-23 tarde) — MURO DO PAIRIP CONTORNADO via FRIDA NO MOTO G100 ROOTEADO
**NextOS trouxe um Moto G100 (Android 12, KernelSU-Next) como bancada de testes PERMANENTE.** O app
Elderand (com.pid.elderand 1.3.22, versão PDALIFE-modded — tem `libPDALIFE.so`) **RODA no device real**.
Em vez de furar a VM do pairip, **li o app real rodando** e capturei tudo. **BANCADA REUSÁVEL P/ TODO PORT.**

**SETUP DA BANCADA (permanente, reusar sempre):**
- Root KernelSU-Next dava `CapEff=0` (sem ptrace/socket) → frida não subia. FIX: **módulo KernelSU**
  `/data/adb/modules/elddump/` com `service.sh` que sobe o **frida-server** no boot como init = **root COMPLETO
  (CAP_SYS_PTRACE)**. frida-server 17.15.3 arm64 (localhost). `module.prop`+`service.sh`+`frida-server` lá.
- PC: venv `/tmp/fridavenv` (frida-tools 17.15.3). `frida-ps -U` lista tudo = caps OK.
- ⚠️ o `su` interativo continua capless; o poder vem do frida-server do módulo (boot). Reboot ativa.

**CAPTURA (scripts em /tmp, logs em devharness/dev_out/):**
- `run_frida.py`+`hook.js`: spawn + hook dlsym/dlopen (API frida17: `Module.getGlobalExportByName`).
  Capturou 230 `il2cpp_*` (resolvidos via dlsym pela libunity→libil2cpp) — NÃO são os lazy.
- `got_syms.py`: **attach + lê a .got.plt da libunity (vaddr 0x1380f88, len 0x3070) do app rodando +
  DebugSymbol.fromAddress** → **365 slots externos, 364 com NOME**. `dev_out/got_resolved_symbols.log`.

**🔑 ACHADO DECISIVO: a libunity REAL do device (1.3.22, md5 8fe7856d) tem 354 rela.plt + só 12 LAZY.**
O "muro dos 200 lazy" era da libunity ERRADA (a play 44a2c0). Com a libunity do device o loader resolve
354 sozinho; restam **12 binds triviais (libc + __sF)**: calloc, realloc, free, strdup, fputc, malloc,
fread, fwrite, pthread_create, siglongjmp, __sF, +1 ponteiro interno (RELATIVE). TODOS no glibc do Mali-450
(s1 já tratava __sF/stdio). **Tabela gerada: `eld_got_binds.h` (365 slots, flag lazy). Libs reais do device:
`device_libs_1.3.22/` (libunity/libil2cpp/libmain/libfmod/.../libpairipcore).**

**INTEGRAÇÃO INICIADA (compila):** `src/main.c` ganhou `eld_lazy_overrides_dev()` (gated `ELD_DEVLIB=1`)
com os 12 binds da libunity 1.3.22 (offsets 0x1381b90.. GOT base 0x1380f88; calloc/realloc/free/strdup/
malloc/siglongjmp=host glibc, fputc/fread/fwrite=sf_ shims bionic-FILE, __sF=array shim, pthread_create=host).
Chamada após so_resolve. Build OK. ⚠️ FALTA p/ rodar: (a) deployar `device_libs_1.3.22/libunity.so`+`libil2cpp.so`
como os .so que o loader carrega; (b) extrair os ASSETS 1.3.22 (global-metadata.dat + bin/Data) do base.apk do
device (`/data/app/~~SZvA3.../base.apk`, puxável por root); (c) re-tunar offsets play-específicos no main.c que
quebram com a libunity nova (canary tpidr+0x28 é ABI-estável=OK; mas fixes de deadlock sem_shim/pthread em
offsets fixos vão precisar re-captura — usar a BANCADA pra achar). MULTI-SESSÃO mas o MURO (anti-tamper) caiu.

**🎉 ASSETS 1.3.22 EXTRAÍDOS + global-metadata.dat É PLAINTEXT** (magic 0xFAB11BAF/`af1bb1fa`)! A versão
modded PDALIFE NÃO encripta a metadata → SEM muro de decifração de il2cpp. `base.apk` (276MB) em `/tmp/base.apk`;
assets em `assets/bin/Data/` (global-metadata.dat 12MB, datapack.unity3d, sharedassets, boot.config, 7492 arquivos).
**Tudo pronto p/ o bring-up: libs reais + metadata legível + 12 binds + bancada pra capturar o que faltar.**

**➡️ PRÓX (port pode FINALMENTE andar):** (1) trocar o port p/ usar `device_libs_1.3.22/` (libunity 8fe7856d);
(2) bindar os 12 lazy via `eld_got_binds.h` (lazy=1) no eld_lazy_overrides (offset→símbolo, resolve no
glibc/framework e escreve em libunity.base+off); (3) ver se o pairip dessa versão é só wrapper-Play
(stubável tipo Terraria) ou VM-agressivo — testar com PLT já resolvido + VM stubada; (4) se libunity init
andar → render ES3→ES2 Mali-450 + áudio FMOD + controle. A BANCADA fura qualquer pairip futuro lendo o app real.

## 🚀 s6 (2026-06-23 manhã) — CELULAR VOLTOU: FUREI INTEGRIDADE+TELEMETRIA via FIXUP (mais longe que nunca)
Auto-captura funcionou (1 run dumpa tudo). Com o device achei o mecanismo EXATO e avancei MUITO além das 5 sessões:
- **Crash inicial = `pairip+0x3a7a4`** (resolver IL2CPP `XegSMCpJ1eNApDq7`, roda no _init do libil2cpp): `ldr x19,[x11,w2]`
  (x11=buffer do programa `\0IAP`, x19=slot de uma TABELA DE PONTEIROS indexada por `w2=(buffer[w3]^w10)%w8`) →
  `ldp [x19,#24]` = deref de DESCRITOR de região {start@+24, end@+32}, `sub; cmp #0x5000`. x19=LIXO (slot NÃO
  relocado: a app patcheia esse slot p/ um ponteiro real no init da VM; nosso harness não → bytes crus do asset).
- 🔑 **FNV é DETERMINÍSTICO sobre o ASSET CRU** (não depende do ambiente!): o loop `0x3a7b8` faz
  `h=h*0x100000001b3 ^ buffer[w28+i]` (0x1b71 bytes) vs baked-in `x24`. Confirmei: buffer[w28]==asset[0x20901] byte-a-byte.
  Então essa checagem PASSA igual na app e no harness — o problema é SÓ o ponteiro-descritor lixo.
- **FIXUP (ELD_FIXUP_DESC=1)**: no SIGSEGV decodifico a instrução, acho o reg-base Rn e aponto p/ `g_fake_page`
  (zerada, com end@+32=0x100000 → size>0x5000 → toma o ramo "hashear buffer" = caminho normal) e RETOMO. →
  passou o 0x3a7a4 e **rodou a VM até a TELEMETRIA Play Integrity** (getContext→Intent
  `com.google.android.gms.play.integrity.autoprotect.LOG_TELEMETRY`→setPackage→putExtra→**sendBroadcast**). MARCO.
- **2 gates da telemetria resolvidos:** (1) `mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, NONE)` mata o abort
  "Pointer tag truncated"; (2) **free-guard (ELD_FREEGUARD=1)**: a VM passa ponteiro LIXO misalinhado
  (0x2ae064a460b29e) p/ free()→scudo aborta; dropamos frees inválidos → sendBroadcast COMPLETA.
- **Parede atual = `pairip+0x50650`**: OUTRO loop FNV (mesmo primo) hasheando um buffer do HEAP `x1` com
  **count `x0`=0x23733 grande demais** → over-read além do fim (scudo:primary acaba em página `---p`) → SEGV_ACCERR.
  = mesmo padrão over-read das sessões antigas, causado pelo estado corrompido do descritor FALSO.
- 🎯 **DIAGNÓSTICO RAIZ:** o fixup PULA as checagens mas a VM opera com DADOS ERRADOS (descritor falso→
  lengths/ponteiros lixo→cascata: free lixo, over-read). O fix CORRETO = a TABELA DE PONTEIROS (buffer[w2]) precisa
  ter DESCRITORES REAIS. A app patcheia esses slots no init da VM; falta achar ONDE/COMO (não é dl_iterate_phdr —
  hookei e NUNCA é chamado antes do crash; não é leitura de maps-content — só `File.length()` é chamado, sem
  FileInputStream; pairip NÃO importa open/read, só dl_iterate_phdr/opendir/readdir/stat/dlsym/dlopen/free/malloc).
  PRÓX: achar o passo de relocação/patch do constant-pool da VM (provável usa o arg `Long(base da lib)` que o
  ExecuteProgram boxa). Tudo gated, sem regressão. Harness rebuild rápido; `run_on_device.sh` 1-shot; auto-dump+scan.
- ⚠️ **CONCLUSÃO da abordagem guard (testada à exaustão): NÃO CONVERGE.** FIXUP+FREEGUARD+READGUARD juntos
  passam integridade+telemetria mas batem em hash-sobre-dado-errado infinito; **NUNCA chega no dlsym (0 símbolos)**.
  Confirma: pular checagem não resolve pq a RAIZ (buffer do programa nunca é DECIFRADO + tabela de descritores
  nunca é CONSTRUÍDA) continua quebrada. `executeVM`(0x59a70)= GetArrayLength→malloc→memset→GetByteArrayRegion
  (cópia CRUA do asset p/ heap x24)→interpretador `0x4d2c4`(state={buf,len}, argsArray). A decifração/relocação
  é PASSO DO INTERPRETADOR que não roda/diverge no nosso env. **Furar de verdade = RE do interpretador 0x4d2c4**
  (achar onde decifra o buffer in-place — s3 viu strb@~0x41858 — e a origem da chave) OU achar por que a
  comparação de integridade que GATEIA a decifração falha aqui. Multi-sessão. Flags novas em harness_main.c
  (ELD_FIXUP_DESC/FREEGUARD/READGUARD), heap-tagging NONE, auto-dump+scan-strings+forge-path. Sem regressão (boot
  normal sem flags inalterado).

## ⏸️ s6 (2026-06-23 madrugada) — PREP COMPLETO, BLOQUEADO NO DEVICE (celular desconectado)
NextOS foi dormir e pediu p/ não parar. **O S24 NÃO está conectado** (adb vazio, lsusb sem Samsung).
TODO o caminho vencedor (s5/s6) precisa do device real (libs Play bionic + /proc/self/maps real).
Sem device = sem run. **Confirmei que o qemu/PC NÃO substitui**: a versão-NOSSA sob qemu crasha
ANTES (NPE em executeVM @pairip+0x5305c, NEM lê maps); só a versão-PLAY no device chega no +0x3913c.

**O QUE DEIXEI PRONTO p/ quando o celular voltar (1 run resolve):**
1. 🔑 **AUTO-DUMP de crash no harness** (`harness_main.c` novo `crash_handler` + `install_crash_handler`,
   já compilado em `eldh`). Em QUALQUER crash do pairip (SIGSEGV/BUS/ILL/ABRT) dumpa p/
   `/data/local/tmp/eldh/crash.log`: **PC relativo ao pairip_base**, x0..x30+sp, mem de x8(buffer VM)/
   x28(ptr lixo)/x1(VM state)/x0 via /proc/self/mem (não fauta), e o **/proc/self/maps INTEIRO +
   contagem de linhas/segs libpairipcore**. = tudo que o gdb daria do +0x3913c, numa run só, sem lldb.
2. **`devharness/run_on_device.sh`**: UMA run — push eldh+libs PLAY(`play/x/lib/arm64-v8a`)+assets
   (`play/x/assets`) p/ /data/local/tmp/eldh, roda ELD_PAIRIP=1 ELD_VMTRACE=1, puxa crash.log+dlsym.log
   p/ `devharness/dev_out/`. (libpairipcore play==devharness md5 3c41dbcc; resolver Play=`pvoBWX9seVNR5ehp`.)
3. **`/tmp/wait_phone.sh`** rodando em bg (me acorda quando adb enxergar o device).

**MODELO do +0x3913c (do objdump, p/ ler o crash.log rápido):** opcode do interpretador VM.
`x8=[x1]`=buffer do programa/VM; `w9=(w15 ^ decod) mod w16` (w16≈nº de range-objs); `x28=*(x8+w9)`
= 8 bytes do buffer tratados como PONTEIRO; crash `ldp x9,x29,[x28,#24]`+`sub;cmp #0x5000` = checa
**tamanho de uma região [start@+24, end@+32] ≤ 20KB** → x28 é um **"range descriptor" object**. Logo o
buffer tem uma TABELA de ponteiros p/ range-objs que a VM CONSTRÓI ao parsear /proc/self/maps; o
bytecode indexa nela esperando o layout de maps do app. No nosso processo o maps difere → program[w9]
= slot não-populado/garbage → deref de lixo. **DIAGNÓSTICO no crash.log:** se x28 for ~heap (range
válido mas errado) → w9/w16 off (forjar nº de regiões); se x28 for lixo puro → slot nunca populado.
**FIX provável:** forjar /proc/self/maps (servir maps sintético "de app real" via interceptação do
FileInputStream no jni_shim) p/ casar count/ordem de regiões → w9 cai num range-obj válido → VM segue
→ fase de resolução → hooks dlsym logam os 200 (lib,símbolo) em dlsym.log → bindar → PULAR a VM.
⚠️ a parte de parsear maps é BYTECODE (não dá objdump); só o crash.log runtime revela os valores.
---

## 🚀🚀🚀 s5 (2026-06-23) — BREAKTHROUGH: HARNESS NO ANDROID REAL FUROU O ANTI-TAMPER
NextOS conectou um **Samsung S24 Ultra (SM-S928B, Android 16, NÃO-rooted)** via adb e instalou o
Elderand da **Play Store** (user 0). Sem root → não dá p/ ler a memória do app. SOLUÇÃO QUE FUNCIONOU:
rodar o ExecuteProgram do pairip **no MEU processo** (shell uid) no device real, via `dlopen`
(libpairipcore **file-backed** → `/proc/self/maps` REAL satisfaz o anti-tamper que o emulador no PC
fingia errado). **Passou a parede da VM que travou 4 sessões.**

**HARNESS: `~/elderand-build/devharness/` (eldh, bionic arm64, NDK r27c em /tmp/ndk).**
- `harness_main.c` + reusa `jni_shim.c`+`jni_idx_stubs.gen.c`+`util.c` do port.
- Build: `aarch64-linux-android30-clang -O1 -fPIC -pie -o eldh *.c -ldl -llog`.
- Faz: `dlopen(libpairipcore.so)` (RWX por-segmento p/ auto-descript) → hooka **dlsym/dlopen no GOT**
  do pairip (loga os 200 órfãos) → `JNI_OnLoad` do pairip c/ jni_shim → **dlopen(libunity.so)** →
  o `_init` do libunity chama **ExecuteProgram(args REAIS)** → `VMRunner.invoke` → **bridge**
  (`pairip_vmrunner_invoke`) → `executeVM(bytes do asset, args reais)`.
- ⚠️ **PRECISA `ELD_PAIRIP=1`** (gate do bridge em jni_shim CallStaticObjectMethodV). Rodar:
  `cd /data/local/tmp/eldh && ELD_PAIRIP=1 ELD_VMTRACE=1 ./eldh`. Device em /data/local/tmp/eldh/
  (eldh + todas as .so do Play em x/lib/arm64-v8a + 22 assets IAP).
- **Play version DIFERE da nossa** (md5 libunity/libpairipcore ≠; Firebase 13_0_0 vs 12_2_0).
  Resolver do Play = asset **`pvoBWX9seVNR5ehp`** (referenciado pelo libunity); il2cpp ref
  `XegSMCpJ1eNApDq7`. Libs+assets puxados em `~/elderand-build/play/`.

**FIXES no jni_shim.c (s5, reusáveis):** NewObject de `Long(value)` agora guarda o valor cru em
`o->lval` e `longValue()` devolve ele (antes descartava → a VM recebia base-de-lib errada). O arg
real que ExecuteProgram boxa = **`Long(0x6ceb8f6000)` = base ELF de uma lib** (a VM usa p/
resolver/hashear).

**ATÉ ONDE CHEGOU (VMT log, executeVM rodando o resolver de verdade):**
FindClass(Float..Boolean,Intent,Uri,Context,VMRunner) → NewObjectArray(1)+NewObject(Long base) →
VMRunner.invoke → bridge → File('/proc/self/maps').length()=**193152** → VMRunner.getContext() →
NewObject(Intent "com.google.android.gms.play.integrity.autoprotect.LOG_TELEMETRY") → setPackage +
4×putExtra → **sendBroadcast** → **CRASH em libpairipcore+0x3913c**.

**A PAREDE NOVA = libpairipcore+0x3913c** (MUITO além do FNV/0x58eac antigo; anti-tamper PASSOU):
`ldr x8,[x1]` (x8=buffer do programa) ; índice `w9=(w15 ^ program[PC+0x22]) mod w16` ;
`ldr x28,[x8,w9]` (8 bytes do programa) ; **`ldp x9,x29,[x28,#24]`** → x28=`0xfa88594bc1ef2904`
(CONSTANTE, lixo-como-ponteiro) → SIGSEGV. As strings do programa decodificaram OK (telemetry
legível) → o programa ESTÁ decifrado; o índice `w9` é que dá num slot com lixo. Hipótese forte:
o índice depende do **hash/length de /proc/self/maps** (a VM leu length=193152 do NOSSO processo,
que difere do app real) → índice errado → deref de lixo. dlsym.log VAZIO (crashou ANTES de resolver).

**PRÓXIMO (s6) — atacar +0x3913c (agora é MEU processo → dá p/ gdb SEM root):**
0. **gdb no harness** (own-process, sem root): `gdbserver :5039 ./eldh` no device + `aarch64 gdb`
   no PC (NDK tem prebuilt). bp em pairip_base+0x3913c, ver x8/[x1], o slot program[w9] e w15/w16;
   subir o stack p/ achar de onde vem o índice e se é a maps-length/hash.
1. **Forjar /proc/self/maps**: fazer a VM ver um maps "de app real". O jni_shim lê via java.io.File
   (File.length/read) — servir um maps forjado (entradas /data/app/.../lib/arm64 file-backed +
   /apex), OU interceptar o open de /proc/self/maps no harness. Talvez o índice/length precise casar.
2. **dlopen TODAS as libs do app** no harness (libil2cpp, libfmod, etc.) p/ o /proc/self/maps ficar
   parecido com o do app (mais entradas file-backed) — pode estabilizar o hash.
3. Se passar: a VM entra na fase de resolução → os hooks dlsym/dlopen logam os 200 (lib,símbolo) em
   /data/local/tmp/eldh/dlsym.log → bindar em `eld_lazy_overrides` → PULAR a VM no port → boot.
⚠️ device por adb (RQCX204HZXX). App instalado em user 0 (Secure Folder=user 150 é INACESSÍVEL).
NDK r27c em /tmp/ndk. Harness rebuild rápido. NextOS: deixar o celular PLUGADO+desbloqueado.
---


## 🔬🔬 s4 (2026-06-23) — ROOT CAUSE do crash FNV ISOLADO A NÍVEL DE INSTRUÇÃO (sem regressão)
Usei o harness qemu DETERMINÍSTICO (`setarch -R`, base FIXA `0x7fffdc400000`) + `-d cpu`/`-d exec`
trace + gdb-python no gdbstub p/ rastrear EXATAMENTE de onde vem o `w26=0xfffffcef` que estoura o FNV.
**CHEGUEI NA INSTRUÇÃO QUE SETA O COUNT.** É o passo de RE mais fundo até agora.

**FERRAMENTAS NOVAS (reusáveis, em ~/elderand-build e /tmp):**
- `/tmp/qgdb.sh [gdb]` = qemu com **ASLR OFF** (`setarch -R`) → base pairip SEMPRE `0x7fffdc400000`,
  ExecuteProgram=`0x7fffdc468b18`. `gdb` → gdbstub :1234. HW-watchpoints NÃO funcionam no gdbstub
  do qemu-user ("Could not insert"); breakpoints `hbreak *<abs>` FUNCIONAM e disparam.
- `/tmp/qtrace.sh` = qemu `-d exec,nochain -D /tmp/qexec.log` (trace de TBs em ordem de execução).
  `-d cpu -dfilter <range>` = dump de regs por TB (filtrado no range do pairip = ~87k linhas com FNVCAP).
- Crash handler em main.c agora dumpa o estado FNV (`[FNV]` block): base, sp, w26(count), x20(hash),
  x8(buffer), x23(ctx), ptrA=[x23+104], buffer=[ptrA+8], offset=[sp+272], size=[ptrA+16], container.
  (gated: só quando pc dentro do pairip; NÃO afeta boot normal.)

**A CADEIA EXATA (provada com trace+gdb):**
1. O bloco **pairip+0x52b34** é o DECODER de uma instrução-VM. Lê o "VM-PC" `w10=[ptrA+20]=0x12351`
   e decodifica os operandos do bytecode em `buffer=[ptrA+8]`:
   `52b38 ldp w19,w10,[x8,#16]` (w19=[ptrA+16]=**size 0x19515**, w10=[ptrA+20]=**0x12351**);
   `52b3c ldr x9,[x8,#8]` (x9=buffer); vários `ldr/ldrsh [x9, w10+N]` lendo operandos.
   **`52b8c ldrsh w26,[x9, w13]`** com `w13=w10+0x18=0x12369` → **w26 = int16 SINAL em buffer[0x12369]**.
2. No runtime `buffer[0x12369]=0xfcef` → `ldrsh`→ `w26=0xfffffcef` (=-785). Carregado (callee-saved)
   intacto por ~352k TBs até o FNV em **0x58eac**, onde vira o COUNT do loop `cmp w26,w9` (unsigned
   ~4 bi) → over-read 1 página além do heap (fault `0x200000`).
3. **A constante 0xfffffcef é DETERMINÍSTICA** (idêntica entre ASLRs) pq é um byte DO PROGRAMA decodificado.
4. **O buffer NÃO é o asset cru**: `asset[0x12369]=0x08af` mas `buffer[0x12369]=0xfcef` → **o programa
   FOI transformado/decifrado** em runtime (≠ s2 que achava "não decifrado"). Mas o operando decodificado
   é LIXO (-785 não é count válido). Logo: **a decifração roda mas produz bytecode errado** (chave/fluxo
   divergente do anti-tamper) → operandos-lixo → over-read. NÃO é "esqueceu de decifrar".

**O QUE ISSO SIGNIFICA (veredito honesto):** é o MESMO muro (VM flattened auto-decifrante + anti-tamper),
agora localizado na instrução. Furar = achar o PASSO DE DECIFRAÇÃO EM MASSA do buffer (quem escreveu
`0xfcef` em buffer[0x12369]) e a ORIGEM DA CHAVE / o branch de opaque-predicate que diverge no nosso env.
HW-watchpoint no gdbstub do qemu não insere (limitação); precisa achar o decryptor por trace de stores
(`-d` não loga stores; usar Unicorn p/ hook de mem-write, OU instrumentar em C um mprotect-trap na página
do buffer). Continua sendo trabalho de várias sessões de RE OU — atalho real — **rodar o APK num Android
de referência** (device físico OU emulador) com hook em dlsym → logar os 200 (lib,símbolo) que a VM
resolve → bindar em `eld_lazy_overrides` e PULAR a VM inteira. **PC tem internet+KVM+adb+qemu-system-aarch64,
mas SEM device conectado e SEM SDK/imagem Android** (montar emulador arm64 = projeto pesado, arm-em-x86
é TCG lento/deprecado). adbkey existe (NextOS já usou adb com device) → **se NextOS plugar um Android,
o dump dinâmico vira trivial e destrava tudo de uma vez.**

**PRÓXIMO (s5), em ordem de aposta:**
0. **Achar o decryptor do buffer** (quem escreve buffer[0x12369]=0xfcef): instrumentar em C —
   após a VM alocar o buffer, mprotect a página RO e capturar o SIGSEGV do 1º store (revela o decryptor
   + sua chave). OU Unicorn-emular executeVM com hook UC_HOOK_MEM_WRITE filtrando o range do buffer.
1. **Achar o branch que diverge**: single-step/trace comparando 2 envs OU achar o opaque-predicate que
   lê um valor de ambiente (a chave/flag) e manda pro caminho-tamper. NOPá-lo.
2. **Atalho dinâmico (DESTRAVA TUDO)**: Android de referência + hook dlsym → 200 símbolos → bind.
---


## 🟢🟢 s3 (2026-06-23) — FUREI VÁRIAS CAMADAS DO ANTI-TAMPER (progresso REAL, sem regressão)
Usei o harness qemu + objdump + gdb p/ destrinchar o que `executeVM` REALMENTE faz, achei e corrigi
BUGS de verdade no shim que travavam TUDO. O crash do pairip AVANÇOU por 3+ camadas distintas.

**BUGS CORRIGIDOS (críticos, reusáveis p/ qualquer port):**
1. 🔑🔑 **`class_for`/`reg_mid` guardavam o PONTEIRO de nome CRU** (não strdup). O pairip passa nomes
   em buffers TEMPORÁRIOS → o ponteiro DANGLING → `mid_name`/comparações de classe viravam LIXO →
   File.length() não casava "length", NewObject(File) não casava → null → NPE. **FIX: strdup(name/sig)
   em ambos.** Isso corrompia TODO dispatch por nome. (jni_shim.c class_for + reg_mid)
2. **Ring-buffer de `g_classreg`(128)/`g_midreg`(1024) RECICLAVA** → tags de jclass/jmethodID mudavam
   de slot em runtime → o pairip guarda a jclass de FindClass e compara depois em NewObject → falhava
   (off-by-0x10). **FIX: arrays 4096/8192, clamp estável (nunca reset).**
3. **ExceptionCheck no índice ERRADO (205); é 228.** Regerei `jni_idx_stubs.gen.c` com NOMES JNI reais
   por índice + log sob `g_vmtrace` (toda chamada JNI logada na fase VM, não só 3×).
4. **AllocObject(27) implementado**; **NewObject(28/29/30)** agora cria objeto File REAL (não-null) qdo
   1º arg é path ('/'), pega o path via va_arg. **File.length()** lê o tamanho REAL de /proc (stat dá 0).
   **CallLongMethodA(idx 54) ligado** (o pairip chama File.length por aí, era stub→0).

**ARQUITETURA DESCOBERTA (decisiva):**
- `libunity._init` → **`ExecuteProgram`(0x68b18, NATIVO)** monta `NewStringUTF(nome)` + `Object[]` (boxa
  os args do sp) e chama **`VMRunner.invoke(nome,args)` via CallStaticObjectMethod** (idx 114). VMRunner é
  classe DEX (NÃO TEMOS) → no app real ela lê o asset e chama `executeVM(bytes,args)`. **O ELO QUE FALTA
  é o BRIDGE VMRunner.invoke**: interceptar CallStaticObjectMethod(VMRunner.invoke) → ler asset[nome] →
  chamar executeVM(bytes, **o Object[] que ExecuteProgram montou**) → devolver o resultado.
- Hoje chamamos executeVM DIRETO com args DUMMY (g_pairip_args_sentinel) → a VM reclama. Provável que o
  "Attempt to get length of null array" final seja por causa dos args FALSOS (a VM faz `args[k]...length`).
- `executeVM`(0x69280) lê o programa e chama o **interpretador VM em 0x46878**. O anti-tamper roda dentro.

**ANATOMIA DO ANTI-TAMPER (o muro atual):**
- A VM faz `new File("/proc/self/maps")`/`/proc/self/status` + `.length()` (✅ resolvido) e LÊ o conteúdo
  de /proc p/ montar uma **hash-map de integridade**. Em ambiente NÃO-Android (qemu/emuelec) a estrutura
  interna CORROMPE → SIGBUS num `ldadd` (atomic) em pairip+0x29130 (x1 = BYTES de conteúdo, ex "amlogic-o"
  do path do toolchain, ou "7efd5c-..." de range de map_files). Caller pairip+0x52bd8 (insert/refcount).
- **COMO o pairip lê /proc**: NÃO via libc open/read. Tem **1 `svc` inline em +0x153f0** (wrapper de
  syscall próprio: x0=num,x1..x6=args) — **patcheei p/ saltar pra `pairip_syscall`** (main.c, ldr x16/br
  x16/.quad). Mas só vi `clock_gettime(113)` por ali. O conteúdo de maps vem por OUTRO caminho (provável
  `opendir`/`readdir` de **/proc/self/map_files** — nomes = ranges de endereço, batem com "7efd5c-...";
  pairip importa opendir/readdir/readlink). Interceptei opendir/readdir/closedir (GOT) mas na última run
  o caminho MUDOU (a VM passou a estourar "null array" CEDO, antes do File — provável dependência de
  tempo via clock_gettime OU de args reais).

**INFRA NOVA (env-gated, sem regressão — boot normal device-flags INTACTO, valida até GfxDevice igual):**
- `ELD_VMTRACE=1` (qemu-run.sh já liga): loga TODA chamada JNI da fase VM + caller relativo ao pairip.
- `ELD_SYSTRACE=1`: loga syscalls do pairip (via wrapper patcheado).
- `pairip_syscall` (intercepta openat→`forge_maps_fd` que reescreve paths p/ estilo Android), 
  `pairip_dl_iterate_phdr` (basenames limpos), `pairip_opendir/readdir/closedir` (map_files vazio).
  TUDO em main.c, ligado dentro do bloco ELD_PAIRIP (GOT patches + patch binário do svc).

**✅ BRIDGE VMRunner.invoke IMPLEMENTADO (ELD_PAIRIP_BRIDGE=1) — confirma a arquitetura:**
`_init`→ExecuteProgram monta `NewObjectArray(1)` + `NewObject(Long(magic ELF de uma lib))` e chama
`CallStaticObjectMethod(VMRunner.invoke, "iSE2wyMILH8qDvya", args)`. Interceptamos (jni_shim
CallStaticObjectMethodV nm=="invoke" → `pairip_vmrunner_invoke` em main.c lê o asset e chama executeVM
com o **args REAL**). Implementei NewObjectArray(172)/SetObjectArrayElement(174)/oarr (args[] eram null →
"null array"). **RESULTADO: com args reais a VM AINDA bate no MESMO crash NÚCLEO** = FNV over-read em
**pairip+0x58eac, x26=0xfffffcef, fault=0x200000 (fim do heap)** — IDÊNTICO ao fim da s2. Logo
File/maps/args NÃO são o núcleo; o núcleo é o loop FNV (interpretador VM 0x46878→0x58eac) com count-lixo.
Hipótese forte: a VM hasheia código/região de libunity e COMPARA com valor baked-in; nossa libunity
RELOCADA (loader custom, GOT diferente) não casa → ramo "tamper" → x26 vira estado inválido → over-read.
→ Furar exige RE do interpretador achatado (0x46878) p/ achar a comparação de hash e a origem de x26/
x8(buffer)/x10(offset 101141≈tam prog) e NOPá-la, OU device Android real. NÃO é mais bug de shim.

## 🔥 s3 (cont.) — CADEIA COMPLETA DO PAIRIP REVELADA (furei +3 camadas, é crackável SEM Android)
Provei que **x26=0xfffffcef é constante entre ASLRs** (3 runs, bases diferentes) → é **derivado de DADOS
(do programa), NÃO de endereços** → NÃO é integridade-que-exige-Android, é **bug de dado consertável**.
Depois, com 2 patches experimentais (ELD_FNVCAP + RWX), DESCOBRI a cadeia inteira do pairip:
1. **FNV loop @0x58eac** lê w26 bytes de um buffer (count w26=0xfffffcef no nosso env = errado) e calcula
   um **hash**. (Capei o loop p/ não estourar o heap → avançou.)
2. **Auto-descriptografia @0x41858**: `strb w11,[x10]` com w11=conteúdo XOR chave, escrevendo na PRÓPRIA
   .data do pairip (+0x503e4). Falhava com SIGSEGV pq a página era R-X → **FIX: mprotect libpairipcore
   RWX** (no bloco ELD_PAIRIP, base por g_pairip_base, DEPOIS do so_finalize). A chave XOR (key@[sp+528],
   keylen x20) vem do passo 1.
3. **Executa o código descriptografado** → com FNV capado a chave fica ERRADA → decrypt vira lixo →
   **SIGILL @0x48f70** (lr=0x469d8 = interpretador VM 0x46878).
➡️ **CONCLUSÃO**: o FNV NÃO é só checagem — o **hash dele É a chave de descriptografia**. Logo NÃO dá p/
capar; tem que fazer **w26 (o count) ficar CERTO**. w26 é o tamanho de um container `[[x23+104]+8]` que no
nosso env tem size-field = 0xfffffcef (lixo). Achar onde esse container é montado (do programa) e por que o
size vem -817 = a chave de tudo. Tudo determinístico/in-process → **crackável sem Android** (confirmado).
Patches no código (env-gated): ELD_FNVCAP (diagnóstico — NÃO usar em prod, quebra a chave), RWX da
libpairipcore (NECESSÁRIO, manter). ELD_PAIRIP_BRIDGE=1 (caminho correto via _init→VMRunner.invoke).

**PRÓXIMOS PASSOS (em ordem de aposta p/ FURAR):**
0b. **Achar a origem do container `[[x23+104]+8]` e seu size=-817** (é o count w26 = a chave). Provável
    montado a partir do programa decifrado OU de um valor que um JNI/shim devolve errado. gdb fixo
    (setarch -R, base=0x7fffdc400000) + bp absoluto; rastrear x23/x8 e o campo de size.
0. **ATACAR o FNV/0x58eac** (núcleo): no qemu, bp absoluto = pairip_base+0x58e00; subir o stack do
   interpretador 0x46878 p/ achar QUEM seta x26 (cmp w26 no loop) e de onde vem o "expected hash".
   x8=0xf08e0(buffer no heap), x10=x8+101141. Procurar a leitura do hash-esperado + o branch tamper.
1. **BRIDGE VMRunner.invoke REAL**: não chamar executeVM direto. Deixar `_init`→ExecuteProgram rodar;
   interceptar `CallStaticObjectMethod` p/ methodID "invoke" da classe "com/pairip/VMRunner" → ler
   asset[nomeStr] → chamar executeVM(bytes, **o argsArray REAL** que ExecuteProgram passou) → retornar.
   Isso dá à VM os args CERTOS (boxados do sp), provável fim do "null array".
2. Confirmar via gdb/log se o conteúdo de /proc/self/maps entra por opendir/readdir(map_files) ou
   readlink; então FORJAR esses (entradas estilo Android, paths /data/app + /apex) — a hash-map de
   integridade precisa de paths "normais" p/ não corromper.
3. Se a hash-map ainda corromper: é a libc++ interna do pairip lendo algo do ambiente; cavar 0x52bd8
   (insert) + a fonte do nó (bucket vira conteúdo = overrun ou tabela size 0).
4. DEPOIS do anti-tamper: a VM resolve os 200 slots lazy via dlopen/dlsym (essa é a fase que QUEREMOS).
- ⚠️ `qemu-run.sh` roda de `~/elderand-build` (cwd reseta; rodar `cd ~/elderand-build && ./qemu-run.sh`).
- gdb sob qemu: breakpoints em símbolo do PIE NÃO disparam confiável; use `continue`→SIGSEGV e leia regs,
  ou instrumente em C (caller via __builtin_return_address + g_pairip_base).

## 🚨 DIRETRIZ DO NEXTOS (absoluta)
**Trabalhar SOZINHO, no automático, PEGANDO FIRME. NUNCA PARAR, NUNCA pedir decisão / encher
o saco do NextOS.** Só falar com ele quando tiver IMAGEM do jogo na TV (validar fb0→PNG) ou se
precisar de algo que só ele pode fazer (login interativo). Nada de status longo, nada de
AskUserQuestion. Loops apertados build→run→fix→repeat. Pode consumir todo o contexto. Tomar
TODAS as decisões de implementação sozinho. [[feedback_nao_parar_nao_explicar_ate_imagem]]

## 🚀🚀 BREAKTHROUGH DE FERRAMENTA (s2 fim) — HARNESS QEMU LOCAL (sem device!)
**`~/elderand-build/qemu-run.sh`** roda o elderand so-loader sob **qemu-aarch64-static no PC** e
reproduz a fase PAIRIP/executeVM **IDÊNTICA ao device** (mesmo crash, RESOLVIDOS=0). Isso destrava o
RE de verdade — rápido, local, scriptável — que o gdb limitado do device (sem Python, HW-watch lento)
não permitia. **REUSÁVEL p/ QUALQUER jogo pairip.**
- Deps no PC: qemu-aarch64-static ✓, capstone ✓, gdb com Python ✓. sysroot do toolchain tem ld+libc+SDL2.
- work dir: `/tmp/.../scratchpad/qemu-eld` (binário + .so + assets). `qemu-run.sh` (re)monta e roda.
  `qemu-run.sh gdb` abre gdbstub :1234 → `gdb -ex 'set sysroot <SYS>' -ex 'file <W>/elderand'
  -ex 'set architecture aarch64' -ex 'target remote :1234'` (gdb-Python local funciona!).
- Asset path agora por env `ELD_PAIRIP_ASSETDIR` (default /storage/...); no PC aponta p/ work dir.
- **TRACE qemu `-d in_asm -D /tmp/asm.log`**: log dos blocos traduzidos = só ~792K/39k linhas (cada
  bloco 1×, em ordem de 1ª execução). Filtrar pelo range do libpairipcore (base+0x153a0..+0x6b470)
  dá os **1535 blocos da VM executados**, do VM-entry 0x46878 até o crash 0x58ea8 (FNV loop). Base do
  pairip por run = (addr do executeVM no stdout) − 0x69280.
- **PRÓXIMO (s3) com essa ferramenta**: (1) trace ordenado dos últimos ~30 blocos antes do crash p/ ver
  o caminho que leva ao FNV-lixo; (2) achar a função de DECODE/decrypt (loop XOR pesado) e ver se foi
  executada ou gated; (3) gdb-Python: single-step com dump do buffer + detectar onde/se decifra; (4)
  com capstone, emular a rotina de decode isolada (Unicorn via pip --break-system-packages se precisar).
  Regiões grandes executadas: 0x4abbc-0x4dc9c(286), 0x64284-0x6833c(231), 0x5fdf8-0x61a10(138=JNI_OnLoad).

## 🎯🎯 GATE DO PAIRIP IDENTIFICADO (s2 fim, via harness qemu + ThrowNew instrumentado)
**Instrumentei `jni_ThrowNew` (vtable[14], antes caía no stub-swallow) p/ logar a mensagem da exceção
que a VM lança.** REVELOU o que a VM faz no executeVM (sequência exata):
1. `GetArrayLength(args)=4` + `GetObjectArrayElement(0..3)` — lê os **4 args** (passei sentinela
   `g_pairip_args_sentinel`, antes NULL→"null array").
2. `NewStringUTF("/proc/self/maps")` → `FindClass(java/io/File)` — **monta `new File("/proc/self/maps")`**.
3. Lança **"Attempt to execute instance method of null object"** + **"Attempt to get length of null array"**.
→ **É a ROTINA DE ANTI-TAMPER/INTEGRIDADE do pairip**: lê /proc/self/maps (via Java File API) p/ verificar
as libs carregadas, e processa os 4 args (contexto). Falha porque nosso ambiente JNI fake não dá os
objetos reais (o objeto que a VM quer usar é null → throw → e como não desenrola, cai no FNV-lixo→crash).
**Não é decriptação** (esse era engano meu): a VM interpreta certo, mas BATE no anti-tamper e morre.
**ENTENDIMENTO FINAL do crash (corrigido)**: o "FNV/length-lixo" era engano — o `ldrsw x8,[x8,w26]`
em 0x589f0/0x58abc/0x58d1c usa w26 como **ÍNDICE numa tabela de dispatch**; w26/x26 é o **hash de ESTADO**
do interpretador CFG-achatado (`eor x26,x8; cmp x9,x24; csel` = opaque-predicate dispatch, basis/prime
FNV são as constantes do gerador de estado). CADEIA REAL: o **anti-tamper falha** (env fake: File/maps/
objetos null) → o state-machine da VM **diverge** → o hash de estado vira um valor inválido → indexa a
tabela de dispatch FORA do range → SIGSEGV. Logo o ÚNICO fix é fazer o **anti-tamper PASSAR** (não há
length/cifra a consertar). = prover um ambiente Android/JNI convincente: /proc/self/maps mostrando
libpairipcore/libunity como mapeamentos FILE-BACKED (nossas são mmap anônimo), Context real, e os args/
objetos certos. Substancial mas BEM DEFINIDO agora.
**LEADS S3 (defeat do anti-tamper, agora preciso):**
  - Os **4 args**: descobrir o que cada um deve ser (o objeto null que a VM usa vem de processar um arg
    OU do contexto). Instrumentar Call*Method/unbox nos args dummy p/ ver o que a VM extrai.
  - **`new File("/proc/self/maps")`**: implementei NewObject(File)→sentinela + File.length()→0, mas a VM
    construiu via outro caminho (não chamou NewObject idx28 — idx228=ExceptionCheck). Rastrear como a VM
    constrói/usa o File e o que faz com /proc/self/maps (provável: lê o arquivo e confere libpairipcore/
    libunity como mapeamentos file-backed; NOSSAS libs são mmap ANÔNIMO → falha). Possível fix: servir
    um /proc/self/maps FORJADO (via FileInputStream no shim) mostrando as libs como file-backed.
  - testar ELD_THROW_OK=1 (não seta exceção-pendente) vs default p/ ver se a VM tem try/catch que recupera.
**Instrumentação nova (env-gated, sem regressão): ThrowNew loga msg; g_pairip_args_sentinel (args[4]);
NewObject(File)+File.length(); ELD_NULLARGS/ELD_THROW_OK p/ A/B.**

## OBJETIVO
Elderand (com.pid.elderand, Unity 2021.3.42 IL2CPP+pairip, URP 2D) JOGÁVEL na TV (render +
áudio + controle) no device **192.168.31.100** (Amlogic S905L, Mali-450 utgard, fbdev, 832MB).
ssh: `sshpass -p archr ssh -o StrictHostKeyChecking=no root@192.168.31.100`.

## ONDE ESTAMOS (s1 — progresso REAL do zero)
Loader booting COMPLETO. Cadeia que ANDA com as flags:
`ELD_LAZYPROBE=1 ELD_INITGUARD=1 ELD_REGNATIVES=1 ELD_FAKESINGLETON=1`
→ init_array (186/426 pulam via guard) → JNI_OnLoad → **42 nativos registrados** → initJni →
nativeRecreateGfxState. **PAREDE: o GfxDevice (device GL do Unity) não reconstrói** (crash
repetido 0x423308 `strb w8,[x0]` x0=lixo) porque ~186 inits pulados quebram o grafo de objetos.
NÃO há imagem ainda.

## 🎯 RAIZ VERDADEIRA = pairip (NÃO é "muro", é nó a resolver)
`ExecuteProgram` (libpairipcore.so @0x68b18) é a VM do pairip; libunity/libil2cpp IMPORTAM e o
`_init` do libunity a chama. **A lazy PLT (177 slots .got.plt = 0xbf7c0 cru, sem reloc) é
resolvida pela VM do pairip em runtime.** Bypassando o pairip, os slots ficam crus → todos os
crashes. As funções NÃO são bytecode (provei: bindei pthread/memcpy/std::string nativos e
funcionaram) — pairip só ATRASA a resolução pra runtime. Pairip do Elderand é AGRESSIVO
(≠ Terraria que era só wrapper Play).

## CAMINHO RECOMENDADO (atacar nesta ordem)
### 1) DEFEAT PAIRIP (resolve os 177 de uma vez = boot limpo) — PRIORIDADE
- Testei (bloco `ELD_PAIRIP` em main.c): carregar libpairipcore + chamar `_init` E `JNI_OnLoad`
  do pairip → ExecuteProgram rodou mas resolveu 0 slots (integrity falha silenciosa OU
  resolução on-demand por-slot).
- PRÓXIMO: desmontar `ExecuteProgram` (libpairipcore 0x68b18) e `JNI_OnLoad` (0x5fdf8) p/
  entender: (a) o que ExecuteProgram faz na GOT; (b) se resolução é on-demand (PLT0→GOT[2]=
  resolver pairip + load_base nos slots) → setar GOT[2] + load_base; (c) qual integrity-check
  faz bail (NOP/bypass). _init passa x0=libunity+0x149c040 (programa), x1=sp. HIPÓTESE FORTE:
  pairip resolve só p/ libs que ELE carregou; nosso load manual não é rastreado → precisa
  fazer o pairip "adotar" nosso libunity (registrar no estado dele) OU replicar a resolução.

### 2) GRIND de binding (fallback determinístico, lento)
- PRONTO: `lazy_slot_of_ra` (decoder runtime bl@RA-4→PLT→slot GOT) + `eld_lazy_overrides()`
  (tabela T[] {offset GOT link-time → impl real}). ELD_LAZYALL=1 loga TODA chamada lazy
  (`[LZ] slot=0x.. a0=0x..`). Identificar função pelo uso (objdump PLT + call-site) → add em T[].
  18 já bindados. Cascata: inits [0..53] passam, [54]@0xc7570 1º crash. Bindar as funções que
  [0..53] usam CORRETAMENTE corta a cascata (stub no-op-vtable tem teto, não reconstrói GfxDevice).

## 🔑 AVANÇO s2 (2026-06-22) — anatomia EXATA do muro pairip
Re-verifiquei tudo do zero e ACHEI os artefatos. Estado factual confirmado:
- **libunity: 354 entradas PLT; 155 com rela.plt (resolvidas pelo nosso loader); 200 ÓRFÃS
  (sem rela.plt), TODAS com GOT==0xbf7c0 (PLT0)**. Os 200 resolvem via UM resolver = GOT.plt[2]
  (vaddr 0x1384f98). pairip stripou os JUMP_SLOT desses 200 e resolve em runtime.
- **pairip resolve por NOME via dlopen/dlsym** (strings `dlopen`/`dlsym`/`libc.so` + demangler
  Itanium no libpairipcore). NÃO é bytecode por-função: só **2 call-sites de ExecuteProgram no
  libunity** (5 no libil2cpp). `_init`@0x149c000 chama `ExecuteProgram(x0="iSE2wyMILH8qDvya"@0x149c040, x1=sp)`.
- **🎯 OS PROGRAMAS DA VM ESTÃO NO APK**: `assets/` tem **18 arquivos de nome aleatório 16-char**
  (mesmo formato do id), magic **`\0IAP` ver=2** (IAP = Integrity Application Protection). O resolver
  é `assets/iSE2wyMILH8qDvya` (103701 bytes). **CORPO CRIPTOGRAFADO (entropia 7.898 bits/byte)**.
  libpairipcore NÃO tem símbolos AAssetManager → a camada **Java (classes.dex) lê o asset e passa os
  bytes** pro ExecuteProgram. Logo: rodar a VM precisa de (a) AssetManager shim servindo o arquivo,
  (b) a CHAVE de decriptação (provável no dex / derivada), (c) dlopen/dlsym funcional. Decode estático
  = decifrar + reimplementar a VM (pesado).
- **A cascata dos 155 crashes é SINTOMA, não raiz**: pular init via longjmp deixa estado C++ sujo;
  189/426 inits pulam. Histograma: 155 crasham no MESMO call-site libunity+0xf7e228 (dispatcher de
  estado cujo objeto[+16] vira ponteiro-lixo p/ a string "__cxa_guard_acquire detected recursive
  initialization"); 8 nos inits INICIAIS reais (idx 1,4,5,8,16,18,21,23) via funcs c0ae44/c5cfcc;
  23 chamam ponteiro NULL (pc=0,lr=0, retorno na pilha em libunity+0xc7570). RAIZ = inits iniciais
  que chamam slot órfão não-resolvido → null/lixo. Stub genérico (lazy_stub) tem TETO: devolve objeto
  falso, não reconstrói GfxDevice.
- **2 CAMINHOS REAIS p/ destravar os 200 (ambos pesados):**
  1. **DINÂMICO (mais tratável, precisa Android real):** rodar o APK ORIGINAL num device/emulador
     Android com hook em dlsym (LD_PRELOAD) → logar os 200 (lib,símbolo) que o pairip resolve →
     bindar todos em eld_lazy_overrides. Precisa device Android do NextOS (so-loader aqui não tem
     o ambiente Java/AssetManager/chave).
  2. **ESTÁTICO (puro RE, muito pesado):** achar a chave (classes.dex), decifrar os IAP, reimplementar
     a VM ExecuteProgram. Semanas.
- ferramentas desta sessão: /tmp/pairip_dis.txt, /tmp/eld_dis.txt (libunity), /tmp/il2_dis.txt,
  /tmp/eld_initcrash.log (histograma de inits). Guard instrumentado loga pc/lr/fault por init pulado.

## 🚀 AVANÇO s2 (parte 2) — RODEI A VM DO PAIRIP (executeVM) — inédito
Passo que o prévio NUNCA fez: chamar o nativo **executeVM** com os BYTES do programa.
- pairip JNI_OnLoad registra **1 nativo**: `executeVM ([B[Ljava/lang/Object;)Ljava/lang/Object;`
  (capturado via jni_find_native). executeVM@pairip+0x69280: GetArrayLength(prog)→aloca→
  **GetByteArrayRegion** copia nossos bytes→interpreta no **VM main 0x46878** (dispatcher de
  control-flow ACHATADO/ofuscado, transições por constantes mágicas — OLLVM-style).
- **IMPLEMENTADO** (flag ELD_PAIRIP=1, prog via ELD_PAIRIP_PROG, default iSE2wyMILH8qDvya):
  lê `assets/<prog>` do disco (copiei os 18 IAP p/ o device em
  `/storage/roms/ports/elderand/assets/`), monta jbyteArray (`jni_make_bytearray`), chama
  executeVM. so_patch_lazy_plt(0,...) conta slots lazy antes/depois. Recovery por sigsetjmp
  (g_eld_init_jmp) p/ não matar o processo no crash da VM. Resultado em
  `/dev/shm/eld_pairip_result.txt`.
- **RESULTADO EMPÍRICO (decisivo): os 18 programas crasham IDÊNTICOS** — todos no MESMO ponto
  (pairip+0x58eac, loop **FNV-1a**: primo 0x100000001b3, basis 0xcbf29ce484222325) fazendo
  `ldrsb [x10,x11]` com **count w26≈1M FIXO** (não varia com o programa) → over-read 1 página
  além do heap. **RESOLVIDOS=0 em todos.** Como o count NÃO depende do conteúdo do programa,
  vem de **estado GLOBAL / chave**, não da seleção de programa.
- **CONCLUSÃO**: a VM roda mas mis-steppa SISTEMICAMENTE — depende de algo do ambiente que o
  so-loader não dá. Hipótese forte = **self-integrity**: o FNV provavelmente hasheia uma região
  (a própria libpairipcore / um mapeamento de lib) cujo tamanho/endereço o pairip deriva de
  /proc/self/maps ou dl_iterate_phdr; nosso load custom (mmap RWX 24MB, libs em endereços não-padrão)
  dá params errados → count/buffer batem errado → over-read. OU a decriptação per-instrução é
  keyed numa chave de integridade que falha silenciosa.
- **PRÓXIMO (s3) — leads concretos p/ destravar a VM:**
  1. Investigar como o pairip deriva o buffer+count do FNV (desmontar 0x46878→…→0x58e00 que monta
     x23=[sp+256]/x8=[x23+104]/w26). Achar se é self-hash de lib (ler /proc/self/maps) e forjar.
  2. Conferir se executeVM precisa do **args Object[]** (passei NULL) — o VM pode ler tamanho de args.
  3. Ver se pairip JNI_OnLoad spawna thread/lê system_property/depende de TLS que nosso shim não dá.
  4. Alternativa que continua valendo: DINÂMICO num Android real (hook dlsym) — NextOS não deu device.
- código novo desta sessão (NÃO regredir): bloco executeVM em main.c (ELD_PAIRIP), helper
  `jni_make_bytearray` (jni_shim.c/.h), guard de init instrumentado (pc/lr/fault).

## 🔬 AVANÇO s2 (parte 3) — RAIZ do crash da VM ISOLADA = PROGRAMA NÃO DECIFRADO
- Descartei **dl_iterate self-integrity**: pairip importa dl_iterate_phdr; registrei libpairipcore
  em g_so_mods + apontei o GOT do pairip p/ o dl_iterate_phdr REAL (so_patch_got, "got patches=1").
  **Crash IDÊNTICO** → não é essa a causa.
- Análise dos registradores do crash (FNV @pairip+0x58eac): `add x10,x8,w10` com x8=buffer do
  programa, **w10≈101141 (≈ tamanho do programa 103701)** = offset perto do FIM; depois
  `ldrsb [x10,x11]` hasheia **w26 bytes** a partir daí. w26 é enorme (lixo) → over-read.
  **w26 é um campo de LENGTH lido do próprio programa** perto do offset 101141.
- **CONCLUSÃO REFINADA**: o corpo do programa tem entropia 7.9 (cifrado) e a VM lê length-fields
  CRUS (lixo) → **o programa NÃO está sendo DECIFRADO** antes de interpretar. A decriptação não
  acontece no nosso ambiente. Todos os 18 crasham igual = decriptação é **gated por chave/integridade**
  (provável: chave derivada da assinatura do APK / estado que o pairip JNI_OnLoad real montaria com
  Context/PackageManager reais, que o jni_shim não dá).
- **PRÓXIMO (s3):** desmontar o caminho de DECRIPTAÇÃO no VM main (0x46878) — achar onde lê a chave
  (libpairipcore global? header do programa? JNI getPackageInfo/signature?) e onde decifra o corpo.
  Se a chave for self-contida → forjar. Se for assinatura-do-APK → ou forjar a assinatura via jni_shim
  (PackageManager.getPackageInfo→Signature[]→o cert do APK, que TEMOS em META-INF), ou caminho dinâmico
  (Android real, hook dlsym). A pista da assinatura é o lead mais promissor: o cert está em
  `apk/META-INF/CERT.RSA` e o jni_shim pode servir getPackageInfo(GET_SIGNATURES).

## 🎯 GDB no device (s2 fim) — CLUE DECISIVO: w26 = LENGTH NEGATIVO (-817)
Device TEM gdb/gdbserver/strace (/usr/bin). Rodei sob gdb (/tmp/eld.gdb com os env), parei no SIGSEGV:
- **pc=pairip+0x58eac; x8 = o BUFFER DO PROGRAMA** (`x/16xb $x8` = `00 49 41 50 02 00 00 00 08 04...`=\0IAP).
- **w26 (x26) = 0xfffffcef = -817** (NÃO um 32-bit aleatório de cripto!). Usado como contador
  unsigned no `cmp w26,w9; b.ne` → loop ~4 bilhões → over-read. x10=x8+101141 (w10≈ tamanho prog).
  x23 = STACK (0x7ffe...) = registrador-file virtual da VM; [x23+0x18] tem fragmento de assinatura
  JNI ("ject;)Lj").
- **REINTERPRETAÇÃO**: -817 é "pequeno errado" = **UNDERFLOW numa subtração (fim-início)**, não lixo
  de criptografia. O VM interpreta MOSTLY certo mas UM valor de entrada está off (provável: um
  registrador virtual setado por uma instrução VM anterior que chamou JNI/nativo via nosso shim e
  voltou errado). Suspeitos: (a) **args=NULL** — passei executeVM(bytes, NULL); o resolver é nativo
  (libunity._init chama ExecuteProgram(name, **sp**), não executeVM(bytes,args)) → talvez precise
  invocar via **native ExecuteProgram(name, sp)** com registry populado, NÃO via executeVM; (b) um
  GetArrayLength/string-length do shim devolvendo 0.
- **TESTE FEITO (gdb por programa)**: w26 **VARIA** por programa (iSE2..=0xfffffcef, 6jQo..=0x9777fb80,
  ek9k..=0x855ad2db) → **program-derived, lido de dados CIFRADOS** (o -817 foi coincidência de bytes
  cifrados; 0x855ad2db é inclusive uma das constantes-de-estado do dispatcher da VM em 0x468e4). 
  **CONFIRMADO DEFINITIVO: o programa NÃO está sendo DECIFRADO** → a VM lê opcodes/lengths cifrados →
  lixo → crash. Não é args/env/framing.
- **PRÓXIMO (s3) — a ÚNICA questão que falta: ONDE/COMO a VM decifra (e por que não decifra aqui)**:
  single-step no gdb DESDE a entrada do interpretador (executeVM nativo → 0x46878) procurando o
  primeiro loop que TRANSFORMA o buffer (XOR/AES/etc.) — ou a falta dele. Achar a fonte da chave
  (global do pairip setada no JNI_OnLoad? header do programa? syscall/property?). Se a decriptação
  nunca é chamada → descobrir o gate (uma flag global que o JNI_OnLoad real setaria). Ferramentas no
  device: gdb/gdbserver/strace. `strace` pode revelar syscalls/property reads que o pairip faz e que
  falham no nosso ambiente. Alternativa que segue valendo: Android real + hook dlsym (precisa device).

## 🔥 strace (s2 fim) — o que o pairip TOCA do ambiente (gate provável = anti-tamper silencioso)
`strace -f` na run ELD_PAIRIP (corrigido): o `openat("/proc/self/maps")` vem DEPOIS do 1º SIGSEGV =
é o NOSSO crash-handler (maps_snapshot), **NÃO** o pairip. Descartado o lead-maps.
O que o PAIRIP de fato toca do ambiente (por imports UND): **`__system_property_find/get/read`**
(stubados em imports.gen.c → retornam 0/vazio), **`readlinkat("/proc/self/exe")`** → 
"/storage/roms/ports/elderand/eld..." (caminho NÃO-Android, app_process esperado), `opendir/readdir`
(varre algum /proc/self/<dir>), `dlopen/dlsym`, `getrandom`.
**REFRAME (consistente com tudo)**: pairip é **anti-tamper SILENCIOSO** — provável que detecte ambiente
errado (exe path estranho, propriedades vazias, libs anônimas, sem assinatura) e mande o VM por um
caminho de execução **corrompido/decoy** em vez de abortar limpo → por isso a VM "roda" mas lê
opcodes/lengths-lixo e crasha diferente por programa (w26 = constante de estado da VM, não length).
**LEADS S3 (em ordem de aposta):**
  1. **__system_property**: dar respostas REAIS de um Android (ro.product.model, ro.build.fingerprint,
     ro.build.version.sdk, etc.) no stub — pairip pode gate a decriptação/caminho nisso. Hoje retorna
     vazio → caminho-tamper. Fácil de testar (preencher o stub).
  2. **/proc/self/exe**: o readlinkat retorna nosso path; forjar p/ algo tipo
     `/data/app/com.pid.elderand-xxx/lib/arm64/...` ou app_process. Hookar readlinkat no pairip.
  3. **single-step gdb** do interpretador (executeVM→0x46878) p/ achar o PRIMEIRO branch que diverge
     (a checagem que decide caminho-bom vs caminho-tamper) e NOPá-la.
  4. **Dinâmico**: Android real + hook dlsym (precisa device do NextOS) — contorna tudo isso.
Nota: a decriptação provável NÃO é device-keyed (o programa tem que abrir em qualquer device), então a
chave é embutida; o que falha é uma CHECAGEM de ambiente que desvia o fluxo, não a chave em si.
**TESTEI lead #1 (propriedades): implementei __system_property_get/find/read/read_callback com valores
de Android real (Samsung S21, sdk 30, ro.secure=1, ro.debuggable=0, kernel.qemu=0). CRASH IGUAL
(RESOLVIDOS=0).** → propriedades NÃO são o gate. Mantive as props falsas (mais corretas que vazio).
**TESTEI lead #2 (/proc/self/exe): pairip NÃO importa readlink/readlinkat → ele NEM checa o exe path
(o readlinkat do strace era nosso código). Descartado.**
**GDB DECISIVO (lead #3 parcial): bp em jni_make_bytearray (marca início do executeVM) + bp em dlsym
só logando DEPOIS → ZERO dlsym durante o executeVM antes do crash.** O resolver do pairip **CRASHA
ANTES de chegar na fase de resolução** (os ~150 dlsym de libc que vi antes eram da NOSSA resolução de
imports, não do pairip). A VM falha CEDO interpretando o programa, nunca resolve os 200.
**RESUMO HONESTO DO MURO (s2 fim)**: a VM do pairip roda mas mis-steppa cedo na interpretação do programa
(que fica cifrado/denso no buffer, lido cru). Testei e DESCARTEI todo gate fácil: 18-programas,
dl_iterate, header-skip(0..32), propriedades Android, exe-path, fase-dlsym. Sem AES/crypto-lib
(cifra custom XOR, 410 eors). **Furar daqui = single-step RE focado da VM (precisa Ghidra/IDA, horas de
trabalho — batch-gdb por SSH não dá conta de um VM ofuscado de CFG-achatado) OU device Android (hook
dlsym no APK real).** NÃO é viável por black-box/cheap-fix — esses estão exauridos.
**Ferramentas/scripts prontos no device**: /tmp/eld.gdb, /tmp/eld2.gdb, /tmp/eld3.gdb (templates gdb com
env). gdb/gdbserver/strace em /usr/bin. assets dos 18 programas IAP em /storage/roms/ports/elderand/assets/.

## ARQUIVOS / FERRAMENTAS
- Port: `~/nextos_ports_android/ports/elderand` (src/main.c=tudo; so_util.c=loader+init guard).
- `~/elderand-build/cycle.sh [seg] "ENV=v..."` = build+matar+scp+run+screenshot+log. Log device
  `/dev/shm/eld.log`; fb `/dev/shm/eld_fb.raw` (1280x1440 BGRA32). Já tem TER_SCREEN_W/H=1280/720.
- Toolchain: `~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain`.
  objdump full em `/tmp/eld_dis.txt`. APK extraído `~/elderand-build/apk`. Memória:
  `project_elderand_mali450.md`.

## FIXES JÁ FEITOS (não regredir)
1. so_relocate pular reloc tipo 0. 2. 🔑 so_load PRIMEIRO PF_X=texto (6 PT_LOAD). 3. stdio __sF
map_sf+sf_* wrappers. 4. ELD_INITGUARD (sigsetjmp por init). 5. ELD_NOFATAL (FatalError
0xf7c354→ret + brk 0x452484→ret). 6. ELD_REGNATIVES (10 RegisterNatives individuais=42 nativos).
7. JNI_OnLoad/initJni/RecreateGfxState guardados. 8. window fbdev forçada 1280x720.
9. ELD_FAKESINGLETON (popula singleton global 0x140f7e0).

## DEPOIS DO LOADER (não esquecer)
Render ES3→ES2 (URP exige ES3; rewriter estilo Mina `~/mina-build`), áudio FMOD (libfmod.so
SEPARADO), controle (gamepad.c), empacotar ES/PortMaster. ⚠️ matar elderand antes do scp.
Regras git: master only, sem co-autor Agente.
