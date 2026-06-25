# Elderand (Unity 2021.3.42f1 IL2CPP+pairip) → Mali-450 so-loader — DIÁRIO

Port baseado no scaffold do **Terraria** (mesma engine Unity 2021.3 IL2CPP). Objetivo:
jogável na TV com áudio e controle no device Amlogic **192.168.31.100** (S905L, Mali-450
utgard, fbdev, 832MB). Regra: NÃO parar até render+áudio+controle.

## Estado atual (s1)
🟡 LOADER em progresso. Chega a rodar o **init_array do libunity** (init 5-6 de 426) e
crasha num **FatalError do Unity** (brk em libunity+0x452484) disparado durante
`init_array[6]` (função c12e0 → ee9b8 → f79e20 → FatalError f7c354). Causa raiz =
**LAZY PLT** (ver abaixo): ~200 slots .got.plt de funções internas (libc++/libc estáticas)
sem relocação → workaround com stub retorna lixo → fatal.

## Recon do APK
- `~/Downloads/Telegram Desktop/Elderand 1.3.20.apk` (274MB). Package `com.pid.elderand`.
- arm64-v8a só. libil2cpp.so 70MB (NÃO cifrado, il2cpp_init etc. exportados), libunity.so
  21MB, libpairipcore.so (DRM — IGNORADO, carrega libil2cpp direto), libfmod.so+libfmodstudio.so
  (áudio FMOD SEPARADO — no Terraria era embutido no libunity → arquitetura áudio diferente!).
- Unity **2021.3.42f1** (Terraria=2021.3.56f2; imports/JNI quase idênticos).
- 🔴 libunity diz "OpenGL ES 3.0 is required" → URP exige ES3 → vai precisar spoof ES3 +
  rewriter shader ES3→ES2 (estilo Mina, ~/mina-build/libminashim.c). Shader custom achado:
  "Elderand/Effects/Fog_Mobile".
- Assets: assets/bin/Data/ (73MB) → deploy em /storage/roms/ports/elderand/bin/Data.

## Build/deploy/iterar
- `ports/elderand/build.sh` → binário `elderand` (toolchain Amlogic-old aarch64). Os erros
  "bad subsection length libSDL2.so" do ld são WARNINGS conhecidos (link completa).
- `~/elderand-build/cycle.sh [segundos] [ENV=v...]` = build + matar+confirmar + scp binário +
  rodar com timeout + screenshot fb0 + tail log. Log no device em /dev/shm/eld.log.
- Deploy device: /storage/roms/ports/elderand/ (binário + todas .so + bin/Data/). 50GB livres
  (apaguei a pasta psx a pedido do NextOS).
- run.sh: flags MÍNIMAS (TER_NOSTORAGEPATCH=1 CUP_NOLOGFILE=1 CUP_FRAMES=999999999). NÃO ligar
  flags com RVA hardcoded do Terraria (CUP_GCOFF/TER_FIXNANPART/etc apontam pro il2cpp/unity
  do Terraria 2021.3.56, ERRADO aqui).

## FIXES já aplicados (src/)
1. **so_util.c so_relocate**: pular tipo 0 (R_AARCH64_NONE) na tabela RELA.
2. **so_util.c so_load — seleção de segmento de texto** 🔑: libunity tem 6 PT_LOAD com 2
   executáveis ([0]=20MB código vaddr0, [3]=4096 trampolins). O loop pegava o ÚLTIMO PF_X
   (errado) → text_base off 0x149c000 → relocations liam lixo. FIX: PRIMEIRO PF_X = texto;
   PRIMEIRO não-X = data; load_size = fim máximo de TODOS os segmentos.
3. **bionic_shims.c + main.c — stdio __sF** 🔑: libunity loga via fputc/fwrite/vfprintf com
   FILE* = &__sF[i] (bionic, stride 152B: +0=stdin +0x98=stdout +0x130=stderr). Resolviam pro
   glibc → crash. FIX: `map_sf()` traduz &__sF[i]→glibc stdin/out/err + wrappers sf_* +
   eld_wire_stdio_setimport/patchgot (F0 e F1). __sF aponta pro buffer (não stub___sF).
4. main.c: package "com.pid.elderand", paths /storage/roms/ports/elderand, imports.gen.c
   regenerado do conjunto .so do Elderand (gen-unity-imports.sh).

## 🔴 BARREIRA ATUAL: LAZY PLT (a resolver)
- libunity é **BIND_NOW** mas tem **354 entradas .plt** e só **155 JUMP_SLOT** (.rela.plt).
  Os ~199 restantes = funções INTERNAS (libc++/libc estáticas: __cxa_guard_acquire chamada
  74×, vfprintf/fputc internos do FatalError-logger, operator new, etc.).
- TODOS os 332 slots .got.plt têm valor de arquivo = `0xbf7c0` (= .plt sh_addr link-time =
  PLT0, default LAZY). Os 155 com JUMP_SLOT são sobrescritos no resolve; os 199 internos
  ficam `0xbf7c0` cru → `br x17` p/ ABSOLUTO 0xbf7c0 (unmapped) → SIGSEGV.
- NÃO há reloc (nem RELATIVE nem packed/RELR — confirmado readelf -d completo) p/ esses slots.
  Resolução lazy por índice .rela.plt NÃO funciona (offsets não-contíguos, 90 gaps; não-1:1).
- Identificação slot→símbolo dos internos é IMPOSSÍVEL pelo binário stripped (sem .symtab,
  sem dynsym p/ esses). Hipótese: lld os deixou lazy e bionic adiciona load_bias a .got.plt +
  resolve... mas verificação mostra que não-1:1 quebraria isso tb. INVESTIGAR como roda na Android.
- **Sonda implementada**: `so_patch_lazy_plt()` (so_util) + `lazy_stub` (main, ELD_LAZYPROBE=1)
  patcha todos os slots lazy (==0xbf7c0) p/ um stub que loga RA e retorna calloc fresco.
  Resultado: passa guard+stdio, mas chega num **FatalError** (init_array[6] valida algo que o
  lixo do stub quebra). 8 RAs lazy vistos, quase todos dentro do FatalError-logger f7c354.

## PRÓXIMOS PASSOS (lazy PLT)
- OPÇÃO A: bindar por-função os internos chamados no boot a impls reais host/wrapper
  (guard_acquire/release/abort proprios; logger stdio→sf_*; operator new/delete→malloc/free).
  Slots conhecidos: guard_acquire=GOT 0x1385210 (PLT 0xbfcc0, ldr #528). Logger f7c354 chama
  c01b0(#1160=slot 0x1385488, parece vfprintf), c00a0(#1024=0x1385400, fputc '\n'), c07a0,
  c06a0, c0010, c0320, c0210. FatalError-logger termina em `bl 452484`(brk) — é fatal POR DESIGN.
- OPÇÃO B: descobrir QUAL init (init_array[6]=c12e0→ee9b8→f79e20) fataliza e por quê; talvez
  neutralizar/skip esse init se for diagnóstico não-essencial (ee9b8 retorna bool — TryInitX).
- DEPOIS do loader bootar: render ES3→ES2 (task#3), áudio FMOD (task#4), controle (task#5).

## Refs
- Base: ports/terraria (mesmo loader Cuphead/Unity). Shader: ~/mina-build (Mina ES3→ES1.00).
- Device 192.168.31.100 (Amlogic S905L Mali-450 fbdev 832MB). ssh sshpass -p archr root@...
- ⚠️ matar elderand ANTES do scp (text-file-busy). Validar OLHANDO fb0 (dd→PNG).
