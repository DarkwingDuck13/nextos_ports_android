# NFS Most Wanted — RESUMO p/ próxima sessão (handoff)

## ONDE PAROU (~95% do boot — engine boota, LÊ o OBB, ENTRA no render loop; trava na construção de objetos de asset CORROMPIDOS)

### Rodar (device nextos-87, ES mascarado, Mali fbdev)
```
cd ~/nextos_ports_android/ports/nfs && ./build.sh && scp -q nfs nextos-87:/storage/roms/nfs/
ssh nextos-87 'cd /storage/roms/nfs && export LD_LIBRARY_PATH="/usr/lib32:/storage/roms/nfs" \
  SDL_VIDEODRIVER=mali NFS_INIT=1; timeout 70 ./nfs 2>&1 | tail -50'
```
✅ **NFS_SKIPCTOR NÃO é mais necessário** — o shim de sigaction (abaixo) fez os 3 últimos ctors
(186/187/188 = detecção de CPU) completarem. init_array roda 100%.
OBB (623MB) já no device: `/storage/roms/nfs/data/Android/obb/com.ea.games.nfs13_row/main.1003128.com.ea.games.nfs13_row.obb`
Recon local: `~/Downloads/nfs-analysis/lib/armeabi-v7a/` e `~/Downloads/obb_extract/com.ea.games.nfs13_row/`
objdump: `~/NextOS-Elite-Edition/build.NextOS-...-Amlogic-ng.aarch64-4/toolchain/bin/armv8a-emuelec-linux-gnueabihf-objdump`

### O QUE JÁ FUNCIONA (não re-investigar)
- Build armhf, multi-módulo (libc++_shared+libNimble+FMOD+libapp), 0 unresolved.
- init_array com NFS_INIT=1 + **malloc PADDING +64B** (NFS_PAD, default 64) — CONFIRMADO necessário:
  com PAD=0/16 crasha cedo em `malloc(): invalid size`. NÃO mexer.
- JNI/GameActivity boot completo + render loop (RunLoop tick).
- **softfp ABI shim** + **stat/fstat/statfs → *64** (NOVO: o stat antigo da glibc lê st_size 32-bit
  no offset errado; rotear p/ stat64/fstat64 corrige o tamanho do OBB). + fseek/ftell/lseek/lseek64 hooks.
- A engine ABRE+LÊ o OBB: é um ZIP (PK\3\4, magic 0x504b0304). Header 1028B + central dir no fim.
  Antes do crash: **4826 seeks, ~375KB lidos** (varre o índice ZIP dos 2411 arquivos).

### O MURO REAL (causa-raiz ainda aberta) — OBJETOS C++ CORROMPIDOS no parse de asset
A engine, ao parsear o índice/tweaks do OBB, constrói objetos C++ **estruturalmente corrompidos** e
faz `dynamic_cast`/RTTI neles → crash. DIAGNÓSTICO PRECISO desta sessão:

1. **Crash = `__dynamic_cast` da libc++** (`__dynamic_cast@0x6fd85`, off +0x36 = `ldr.w ip,[r0,#0x18]`).
   O objeto-fonte tem `vtable[-1]` (slot de typeinfo) apontando p/ **CÓDIGO** (libapp+0x47a920, um `bx lr`)
   em vez de um `__class_type_info` real → libc++ desreferencia typeinfo lixo e faz `blx` num ptr inválido.
   = ponteiro de vtable do objeto **deslocado/corrompido** (lê função virtual como typeinfo).

2. Tipos de lixo observados nos slots de ponteiro dos objetos:
   - `0xe12fff1e` = opcode `bx lr` (lê 1ª instrução de função como ponteiro de typeinfo).
   - `0x313d5448` = **ASCII "HT=1"** (bytes 48 54 3d 31) — dados de TWEAK/config caíram onde devia ter
     ponteiro de função! (ex: trecho de "...HEIGHT=1" / "WEIGHT=1"). PROVA de que o parser põe bytes de
     asset onde espera struct/objeto → **leitura de estrutura errada** (offset/contagem/tamanho).

3. A corrupção é PROFUNDA: guardar `__dynamic_cast` faz o MESMO lixo reaparecer 1 nível abaixo (no
   handler de typeinfo `__do_dyncast`, que faz `blx r0` com r0="HT=1"). Band-aids de RTTI só adiam.

### DEFESAS já implementadas (defense-in-depth, NÃO são o fix — mas avançaram o boot)
Em `src/imports.c` + `src/main.c`:
- **my_dynamic_cast** (shim na tabela + **inline-hook na entrada do __dynamic_cast** p/ pegar a recursão
  interna da libc++ que não passa pela GOT da libapp): valida a cadeia sub→vtable→typeinfo→ti_vt e que
  o handler `ti_vt[0x18]` é EXECUTÁVEL (via /proc/self/maps cacheado); senão retorna NULL. Trampolim
  com os 8 bytes originais p/ rodar o dcast real (`g_dyncast_tramp`). `nfs_install_dyncast_hook()`.
- **DCAST-REC** no crash_handler: se faultar em `__dynamic_cast+0x36`, força retorno NULL pulando p/ o
  epílogo (+0xa6, `mov r0,r4; add sp,#64; pop`) com r4=0. (NFS_NODCASTREC=1 desliga.)
- **mem_readable** à prova de fault: sigsetjmp(g_probe_jmp)+leitura guardada; crash_handler honra
  g_probe_armed (siglongjmp). (Substituiu o probe por pipe que era furado.)
- crash_handler robusto: regiões de TODOS os módulos (maps), hexdump @PC só se mapeado (g_pc_mapped),
  stack scan resolve retornos em qualquer região r-x.

### ✅ RESOLVIDO nesta sessão — sigaction ABI (era a causa dos ctors 186/187/188 crasharem)
Os 3 últimos ctors são detecção de CPU (probes crypto/`mrrc` CNTVCT que dão SIGILL e são
capturados). Eles preenchem um `struct sigaction` **BIONIC** (16B: handler@0, mask@4 4B, flags@8,
restorer@12) e chamam a `sigaction` da **GLIBC** (sa_mask=128B → flags@132, restorer@136) → glibc
lia flags/restorer LIXO → handler de SIGILL retornava p/ `0xba`. **FIX (src/imports.c):
my_sigaction + my_sigprocmask** traduzem o struct/sigset bionic↔glibc (dropam SA_RESTORER/
SA_THIRTYTWO; glibc põe o restorer certo). NFS_NOSIGSHIM=1 desliga. → ctors completam, sem SKIPCTOR.
**OBS: isso NÃO consertou a corrupção de asset** (mesma cadeia RTTI; lixo mudou HT=1→TDBG).

### 🎯🔑 DESCOBERTA — o muro é o GERADOR DE SHADERS, não o parse do OBB!
O `dynamic_cast` que crasha (em `0x46b6fc`, dentro de `0x46b220`) é:
**`dynamic_cast<im::isis::shadergen::UniformNode*>( (im::isis::shadergen::Node*)sub )`**
(type_infos resolvidos: src=`N2im4isis9shadergen4NodeE`, dst=`N2im4isis9shadergen11UniformNodeE`).
- `sub` = `[fp-32]`. O loop em `0x46afe0` itera um array de **nós de grafo de shader** (`[sl+24]`,
  índice `[r0+r8<<2]`) e faz dcast de cada Node→UniformNode. UM nó está SELVAGEM (vtable lixo).
- `0x47a920` (que aparecia como "typeinfo lixo") é na verdade a **vtable legítima de um type_info**
  da libapp (o type_info @0xab36a4 tem [0]=0x47a920) — confirma que o nó selvagem tinha
  `vtable[-1]` apontando p/ uma vtable de type_info em vez de um OBJETO type_info.
- **Implicação**: a engine gera shaders no load (provável p/ GLES2/Mali). O grafo de shader tem um nó
  inválido — misparse dos dados de shader OU um tipo de nó cujo construtor/factory não rodou (vtable
  não setada) OU RTTI cross-módulo quebrado no factory de nós.
- **Refino (ring de dcast no crash)**: só ~2-3 dcasts antes do crash. A cadeia:
  (1) `libapp+0x46addc` faz `dcast<Node>(obj=0x47c6858)` — obj VÁLIDO, vtable=libapp+0xab3830.
  (2) recursão INTERNA da libc++ (caller na libc++) caminha a hierarquia de Node e CRASHA
      (handler faz `blx r0`=lixo "HT=1").
  (3) `libapp+0x46b700` faz `dcast<UniformNode>(sub=NULL)` → retorna NULL (ok, array tem nó nulo).
  Ou seja: castar um objeto VÁLIDO (0x47c6858, typeinfo @0xab3854) p/ Node crasha caminhando a
  hierarquia de classes-base → **um type_info BASE da cadeia está corrompido** (vtable do type_info
  = região "HT=1").
- ❌ **DESCARTADO: bug de relocação.** libapp usa REL padrão (DT_REL 388264B, RELCOUNT=45699, tudo
  em `.rel.dyn` que o so_relocate processa 100%). SEM packed/RELR/DT_ANDROID_REL.
- ❌ **DESCARTADO: corrupção dos type_infos estáticos.** `NFS_RELRO=1` (so_protect_relro) protege
  `.data.rel.ro[.local]` do libapp como SOMENTE-LEITURA após relocar → **NENHUM fault de escrita**
  (o crash continua sendo LEITURA de ponteiro selvagem). Logo a .data.rel.ro NUNCA é sobrescrita;
  os type_infos estão ÍNTEGROS. (relro é seguro de ligar — não quebra nada; bom como safety-net.)
- ✅ **CONCLUSÃO: o OBJETO castado é SELVAGEM** (não os type_infos). O array de nós do shadergen
  (`[sl+24]`) tem uma entrada que aponta p/ DADOS de heap/asset (não um Node real). O dcast nesse
  ponteiro selvagem lê *obj=vtable-falsa→typeinfo-falso→handler=lixo("HT=1"/"TDBG"/"HAIN"/"DLRO",
  muda c/ heap) → blx em lixo. **Ponteiro de nó inválido / use-after-free / slot não inicializado.**
- **ESTRUTURA DO LOOP (mapeada)**: em `0x46ad34`+: `r6`=contador (de 0), elemento
  `r5 = [r4 + r6*12]` (**stride 12 bytes/entrada**; 1ª palavra = ponteiro do nó + refcount em r5+4).
  Se `r5==0` pula. Fim do loop em `0x46afe0`: `cmp r6, [sp+12]` (count). `r4`=base do array e
  `[sp+12]`=count são setados ANTES de 0x46ace0 (achar onde — se count estiver grande demais, o loop
  lê ALÉM do fim → entradas selvagens; OU `r4` aponta errado). Há um 2º array em `[sl+24]` (usado em
  0x46afd0: `[r0+r8<<2]`).
- **PRÓXIMO PASSO**: (1) desmontar de ~0x46ac00 p/ trás achando onde `r4`(array) e `[sp+12]`(count)
  vêm — ver se count é misparse. (2) instrumentar: hookar 0x46ad34 (ou logar no my_dynamic_cast o
  índice `r6`=[sp+20] e o elemento) p/ ver em qual índice o nó fica selvagem e se é > count real.
  Tipos: desmangle `N2im4isis9shadergenXX...E`.
- **Hipótese forte = use-after-free OU ABI**: um Node é construído por uma função cujo retorno
  (ponteiro) vem errado por ABI (ex: softfp/struct-return) OU um Node é liberado e o slot reusado.
  Conferir se alguma factory de Node retorna ponteiro via registrador/struct que nosso ABI erra.
- Ferramentas prontas: NFS_TICHAIN (dump da cadeia type_info, c/ heurística __si/__vmi — tem falso-
  positivo em leaf __class_type_info, cuidado), NFS_RELRO (protege .data.rel.ro), g_dcring no crash.

### 🔬 REFINO 3 (esta sessão) — crash é na CLASSIFICAÇÃO de type_info da libcxxabi
- O nó castado é VÁLIDO (refcount=1, vtable na imagem libapp). NÃO é UAF nem nó selvagem.
- A função libc++ envolvida na recursão que crasha é **`0x6f878` = índice 4 da vtable do
  `__si_class_type_info`** (vtable @0x8a7d8: [4]=0x6f879, [6]=0x700c1). Ela carrega DOIS type_infos
  FIXOS da libcxxabi (`_ZTIN10__cxxabiv1...` = __si e __vmi) e faz `dynamic_cast(arg1, __si, __vmi)`
  → é a libcxxabi **CLASSIFICANDO um type_info** (descobrindo se é __si ou __vmi) durante o walk.
- ❌ NÃO é exceção: libapp NÃO importa `__cxa_throw/_Unwind_/personality` (sem máquina de exceção).
- **Mecânica do crash**: a chamada interna recebe arg1 = ponteiro p/ um local cujo 1º word é um
  type_info (0xab36a4=Node ti). O dcast trata isso como vtable → lê `[0xab36a4-4]`=0x47a920 (CÓDIGO)
  como typeinfo → `*0x47a920`=0xe12fff1e → `[+0x18]`=lixo → blx em lixo (heap-dependente).
- Ou seja: a libcxxabi, ao classificar um type_info do walk, recebe/computa um ponteiro de objeto
  ERRADO (aponta p/ um holder de type_info, não p/ o type_info em si). Sutil — provável diferença de
  layout/ABI de type_info OU o __dynamic_cast interno recebe args trocados no nosso ambiente.
- **PRÓXIMO PASSO (precisa de runtime preciso)**: GDB no device (sem python; usar -no-pie + breakpoint
  por scan) OU hookar `0x700c1` (__do_dyncast) e `0x6f878` p/ logar args (this, info, obj, path) e ver
  qual ponteiro está deslocado. Comparar com o fonte da libcxxabi (__class_type_info::process_*).
  Alternativa: testar se DESLIGAR nosso my_dynamic_cast (passthrough puro do real, sem validação/REC)
  muda algo — nossa intervenção pode estar passando args errados na recursão interna.

### ✅❌ TUDO ESTRUTURAL DESCARTADO (precisa de GDB no device agora)
Verificado e CORRETO: relocações libapp E libc++ (R_ARM_ABS32/RELATIVE aplicadas), headers RTTI das
vtables de type_info (0x8a7dc etc. relocam p/ os _ZTIN... certos), resolução cross-módulo (type_info
vtables+objetos→bundled libc++ via snapshot .dynsym/.symtab), relro intacto, nós válidos (refcount
ok). A máquina RTTI está toda consistente e bem montada. O crash é um ponteiro computado errado
DENTRO do walk da libcxxabi — sutil demais p/ análise estática. **PRÓXIMA SESSÃO: GDB no device**
(gdbserver, breakpoint em 0x700c1=__do_dyncast e 0x6f878, single-step, ver qual arg/ponteiro desvia)
OU comparar com o fonte exato da libcxxabi (versão do NDK do APK) das funções __do_dyncast/process_*.
Possível ângulo não testado: o `this`/`obj`/`offset_to_top` passado ao handler pode estar deslocado
por um detalhe de ABI (ex: a engine é SOFTFP — improvável aqui, são ints — ou o __dynamic_cast tem
um 5º aspecto). Tooling pronto: NFS_TICHAIN, NFS_RCLOG, NFS_WILDLOG, NFS_ZTVLOG, NFS_NOHOOK, NFS_RELRO,
g_dcring no crash.

### 🔬 REFINO 2 — RTTI é CONSISTENTE; nó NÃO é UAF (refcount saudável)
- ✅ **RTTI 100% consistente** (descartado mismatch de runtime): as 5 vtables de type_info
  (`_ZTVN10__cxxabiv1...`) que libapp importa resolvem CERTO p/ a **libc++ BUNDLED** (val=base+0x8a7b0
  etc., via snapshot de `.dynsym`). `__dynamic_cast`, type_infos (`_ZTI*`) e vtables: TODOS bundled.
  Sem GNU/LLVM misturado. (Confirmado c/ NFS_ZTVLOG.)
- ✅ **A recursão malformada é REAL da libcxxabi** (não é artefato do nosso hook): com `NFS_NOHOOK=1`
  (sem inline-hook) o MESMO crash acontece (DCAST-REC dispara em +0x36, depois crasha).
- **Mecânica exata**: o crash é `__dynamic_cast+0x36` = `ldr ip,[r0,#0x18]` com r0=`*r6`=vtable do
  type_info-FONTE. r6 = `*(obj_vtable - 4)` (slot de typeinfo da vtable do objeto). Para um objeto
  VÁLIDO (vtable=libapp+0xab3830) r6=0xab3854 (ok). No crash, r6 vira um ENDEREÇO DE CÓDIGO (ex:
  0x47a920) → o slot de typeinfo da vtable lê uma FUNÇÃO VIRTUAL → vptr do objeto está **deslocado
  (+4?) ou o objeto é LIXO**. (my WILD-NODE não pega pq vtable±4 ainda cai na imagem do libapp.)
- **Hierarquia**: a Node deriva de `std::__ndk1::__shared_weak_count` (libapp importa
  `_ZTINSt6__ndk119__shared_weak_countE`) = **refcount intrusivo estilo shared_ptr**. O loop
  incrementa/decrementa refcount (ldrex/strex em [r5+4]) em cada nó.
- **🎯 HIPÓTESE MAIS FORTE = USE-AFTER-FREE de nó shared_ptr**: um Node é liberado (refcount→0) mas
  ainda está no array → memória reusada → vptr vira lixo → dcast lê typeinfo errado → crash. Casa com
  a heap-dependência (lixo muda c/ PAD/layout). Possível causa do refcount errado: ABI dos atomics
  (ldrex/strex ok?), OU nossa my_dynamic_cast/DCAST-REC retornando NULL quebra a lógica de posse, OU
  o __shared_weak_count::__release_shared (vtable[?]) chama deleter cedo demais.
- **PRÓXIMO PASSO**: (1) checar refcount: logar [node+4] (contador) de cada nó no loop — se algum
  chega a 0/negativo durante a iteração = UAF. (2) testar SEM my_dynamic_cast/DCAST-REC (NFS_NOHOOK +
  retornar real puro) p/ ver se nossa intervenção no dcast quebra a posse. (3) inspecionar
  0x46ad50+ (incremento de refcount no loop) e onde os nós são liberados.

### (antigo) caracterização genérica do ponteiro selvagem (ainda válida)
- O objeto passado ao dynamic_cast tem vtable apontando p/ lixo. Os valores ("HT=1", "TDBG") são
  **ASCII e MUDAM** conforme PAD/ctors (não são tags fixas; NÃO existem em libapp nem no OBB) →
  o ponteiro do OBJETO/vtable é SELVAGEM, aponta p/ memória arbitrária com dados de asset.
- **Depende do layout do heap**: NFS_PAD=64 → crash no RTTI do parse (consistente). PAD=256 → chega
  ao render loop e crasha em addr=0x4 (libc). PAD=128/512 → outros. Ou seja, é **overflow de heap /
  misparse** que o padding só REALOCA, não elimina. (PAD=0/16 crasha cedo em malloc invalid size.)
- Hipótese principal: a engine lê um array de PONTEIROS de objeto do índice do OBB e uma entrada
  aponta p/ DADOS de asset em vez de um objeto construído (misparse de offset/tamanho), OU um
  overflow de heap esmaga a vtable de um objeto vizinho.
- **Próximo passo**: instrumentar `0x46b220` (processa cada elemento; chamado no loop de
  `0x46afe0`, indexa array `[r0+r8<<2]`) e a construção do objeto — logar o ponteiro do elemento +
  1as palavras p/ achar a 1ª entrada ruim e de qual offset do arquivo veio. Cadeia (pilha):
  `0x46afe0 ← 0x3de064 ← 0x46b958 ← 0x43c6fc ← 0x5643cc ← 0x466f28 ← 0x3eabdc ← 0x3d4808`.
- Suspeitar: leitura byte-a-byte do índice (vi 4826 seeks perto do EOF lendo 1 byte/vez) — formato
  CUSTOM da EU (não ZIP padrão); algum campo lido com tamanho/sinal errado desalinha tudo.

### (histórico) ctor 186 — detalhes da investigação que levou ao fix de sigaction
init_array tem 189 ctors (0-188); **186/187/188 são os 3 ÚLTIMOS** (init de subsistemas do app).
- **ctor 186 @ libapp+0x7433c = DETECÇÃO DE CPU** (ARM mode): instala sigaction(SIGILL)+sigsetjmp e/ou
  lê getauxval p/ montar flags de feature (AES/PMULL/SHA/CRC32) num global, e seleciona rotinas
  otimizadas por feature.
- Por que crasha (e era pulado): **PC=0xba (SIGILL), chama um ponteiro de função LIXO (0xba)** —
  provável rotina feature-selected (CRC32/hash/memcpy HW) cujo ponteiro não foi setado.
- Já tentei: **getauxval shim** força AT_HWCAP bit 0x1000 (NFS_NOAUXHACK desliga) → ctor 186 PEGA o
  caminho sem-probe (lê HWCAP2=0x1f real: AES/PMULL/SHA1/SHA2/CRC32) — CONFIRMADO via log — **mas
  AINDA crasha em 0xba**. Então o 0xba não é o probe SIGILL; é uma chamada indireta depois das flags.
- Também: nosso init-recovery (so_execute_init_array+crash_handler) só auto-pula SIGSEGV/SIGBUS, NÃO
  SIGILL → por isso 186 precisa de SKIPCTOR explícito (a recovery não o pega).
- **HIPÓTESE-CHAVE (testar)**: a engine usa **CRC32/hash de HARDWARE** (feature ARMv8) p/ o hash dos
  NOMES de asset no índice do OBB. Com ctor 186 pulado, o ponteiro da rotina de hash fica lixo (ou cai
  num default que calcula hash DIFERENTE) → lookups de asset acham offsets ERRADOS → objetos
  construídos sobre bytes errados → typeinfos/vtables lixo ("HT=1"). Isso LIGA ctor 186 à corrupção.
- **Próximo passo concreto**: desmontar de 74538 até o fim de ctor 186 (e o que ele chama) p/ achar
  ONDE o ponteiro 0xba é carregado/chamado e qual tabela/rotina ele seleciona. Consertar esse ponteiro
  (ou prover a rotina) → rodar 186 → ver se a corrupção de asset SOME. Ferramenta: `objdump -d
  --start-address=0x74538 --stop-address=0x74630 libapp.so` (ARM). Os literais PC-rel estão em
  0x74604-0x7461c.

### OUTROS PRÓXIMOS PASSOS
2. **Achar a leitura de estrutura errada**: "HT=1" vem do OBB. Hookar a alocação/construção do objeto
   corrompido e ver de qual offset do arquivo os bytes vêm. Cadeia de chamada (da pilha):
   `0x46afe0 ← 0x3de064 ← 0x46b958 ← 0x43c6fc ← 0x5643cc ← 0x466f28 ← 0x3eabdc ← 0x3d4808 ← 0x75e54`
   (libapp). Desmontar 0x46afe0 / 0x3de064 (perto do parse que cria o objeto).
3. **Suspeitar de tamanho de struct/ABI**: a engine é armeabi-v7a (Android). Algum campo lido como
   32/64-bit errado, ou packing diferente, desalinharia o parse → bytes de string viram ponteiros.
   Conferir se algum import de I/O (read/pread/mmap) devolve tamanho/posição divergente.
4. Comparar com como Bully/reVC tratam o resource/asset system (mesmo framework so-loader).

### Flags de debug (env)
`NFS_INIT` `NFS_SKIPCTOR` `NFS_PAD`(default 64) `NFS_FOPENLOG` `NFS_SEEKLOG` `NFS_DCASTLOG`
`NFS_NODCASTREC`(desliga recovery do dcast) `NFS_NOASSERTIGNORE` `NFS_INITDBG`(log por ctor)
`NFS_PTLOG` `NFS_DLSYMLOG` `NFS_TICKRECOVER` `NFS_FRAMES=N`

### Estado git
Trabalho commitado em ~/nextos_ports_android/ports/nfs (ver git log). Device .87 (DHCP, IP pode mudar).
