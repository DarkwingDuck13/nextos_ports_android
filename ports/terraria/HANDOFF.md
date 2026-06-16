# HANDOFF — Terraria (Unity 2021.3.56f2 IL2CPP) → Mali-450 so-loader

> Para a próxima sessão: o usuário vai dizer **"continuar terraria"**. Leia este arquivo
> inteiro primeiro. Projeto iniciado DO ZERO em 2026-06-16. Device `.89` (Amlogic-old,
> Mali-450 Utgard, fbdev, EmuELEC 3.14.79 aarch64, senha ssh `nextos`/root).

## ⚡ TL;DR — onde estamos
Boot do Unity 2021.3 **FUNCIONA quase inteiro** (libunity+libil2cpp carregam, JNI_OnLoad,
il2cpp_init COMPLETO, engine `MemoryManager`/`SystemInfo`/display 1280x720). **MURO ATUAL:**
o engine morre em `Unable to initialize the Unity Engine`, causado por
`[ALOG:6 Unity] assets/bin/Data/unity_app_guid is empty. Will re-extract il2cpp resources`.
O `data.unity3d` NEM É LIDO (morre na extração il2cpp antes). **Objetivo do usuário: IMAGEM do jogo.**

## 🧱 O BUG A RESOLVER AGORA (unity_app_guid lido vazio)
- O arquivo `/storage/roms/terraria/bin/Data/unity_app_guid` tem **36 bytes válidos**
  (`9b73490b-5e49-488d-9968-182f2630deff`).
- Unity o abre por `open()` nativo: log `[open-redir] assets/bin/Data/unity_app_guid -> /storage/roms/terraria/bin/Data/unity_app_guid` (my_open redireciona CERTO, fd válido).
- MAS o engine conclui que está **vazio**. Já descartado: NÃO é `openFd`/AssetFileDescriptor
  (não usado no log), NÃO é `mmap` (my_mmap é passthrough real), my_open retorna fd correto.
- **PRÓXIMO PASSO**: RE da função em libunity que loga "is empty" (string @ vaddr `0x9291a8`,
  sem `adrp 0x929000` direto → carregada via literal pool). Achar como ela LÊ o guid e o
  size-check. Opções: (a) Ghidra em `~/re-tools`; (b) hook do `read()`/`pread()` p/ logar o
  que retorna no fd do guid; (c) checar se lê via outro caminho (fopen? fread? uma 2ª abertura
  que pega path com prefixo errado?). Hipótese viva: Unity lê via um path/método diferente do
  `[open-redir]` visto, e esse retorna 0.
- ⚠️ NÃO-FATAL? A msg diz "on NEXT run" (parece warning), mas `data.unity3d` não carrega e o
  engine morre logo após — então É o bloqueio. Confirmar.

## 🔁 LOOP DE TRABALHO (build → deploy → test)
```sh
cd ~/nextos_ports_android/ports/terraria
./build.sh                                   # cross arm64 -> ./terraria (erros SDL2 "subsection" = warning, ignore)
ssh root@192.168.31.89 'killall -9 terraria; rm -f /storage/roms/terraria/terraria'
scp terraria root@192.168.31.89:/storage/roms/terraria/
ssh root@192.168.31.89 'cd /storage/roms/terraria; export TER_SHOT=12 CUP_DLLOG=1; sh test.sh 60 250'
# ler: ssh ... 'cd /storage/roms/terraria; grep -aE "ALOG|CRASH|Unable|<r0]|GfxDevice|SHOT" eng.log; tail -8 eng.log'
```
- **`test.sh N F`** = SEMPRE `timeout -s KILL N` + mata leftovers (N seg, F=CUP_FRAMES).
- **CUP_DLLOG=1** liga log de open/stat redirect (`[open-redir]`, `[stat-MISS]`).
- **TER_SHOT=N** = grava `/storage/roms/terraria/shot.ppm` na N-ésima troca de buffer
  (glReadPixels). Verificar imagem: `scp shot.ppm` local → `python3 -c "from PIL import Image; Image.open('shot.ppm').save('shot.png')"` → Read shot.png.
- Env de bypass: `TER_NOSTORAGEPATCH=1` (desliga NOP storage), `TER_NOPTSHIM=1` (desliga pthread shim).

## ☠️ REGRA CRÍTICA: NUNCA rodar sem timeout
Run sem `timeout` deixa a thread `UnityMain` (detached) IMORTAL em busy-spin → pina os 4 cores
→ sshd não responde (banner timeout) → OOM não mata → **só power-cycle físico do `.89` resolve**.
Já aconteceu (1h+ travado). SEMPRE `test.sh`. Se travar: pedir ao usuário pra religar o device.

## 🗺️ Arquitetura
- DO ZERO reusando o **plumbing do loader Unity do cuphead** (`ports/cuphead/src/*`): so_util,
  jni_shim (FalsoJNI), egl_shim, sem_shim, **pthread_fake.c**, opensles_shim. A RE de offsets
  2017.4 do cuphead foi DESLIGADA (`if (0 ...)`) — NÃO se aplica ao 2021.3.
- Imports: `gen-unity-imports.sh libunity.so libil2cpp.so` → `src/imports.gen.c` (passthrough
  via dlsym + stub log). `recon_fill_passthrough()` é chamado antes de cada `so_resolve` (main.c).
  Ao regenerar, sempre `mv src/imports_unity.gen.c src/imports.gen.c`.
- **Payloads** (`.gitignore`, BYO-data, JÁ no device): `payload/lib/*.so`, `payload/assets/assets/bin/Data/**`.
  Origem: APK `/home/nextos/Downloads/Terraria-v1.4.5.6.4terariaapk.com.apk` (Unity IL2CPP + PAIRIP;
  PAIRIP IGNORADO, global-metadata LIMPO `af1bb1fa`).
- **Device**: `/storage/roms/terraria/{terraria, lib*.so, bin/Data/**, userdata/}`. O `boot.config`
  no device (≠ do APK) tem `gfx-disable-mt-rendering=1`, `androidUseSwappy=0`, `gfx-enable-*gfx-jobs=0`.

## ✅ FIXES JÁ FEITOS (commitados — ordem dos muros vencidos)
1. boot.config: MT-render OFF + Swappy OFF → destravou hang do `nativeRender` frame 0.
2. **setjmp/longjmp** stub→passthrough (SIGSEGV); +sig*/gettid/prctl/newlocale; `__errno`→`__errno_location`.
3. **PAD** (Play Asset Delivery): `getAssetPackState`→`nativeStatusQueryResult(name,4,0)` COMPLETED;
   `getAssetPackPath`→"/storage/roms/terraria"; package `com.and.games505.TerrariaPaid` (era hollowknight).
4. **"Not enough storage space"**: NOP em libunity `0x2d8fac` (`tbz w0,#0,0x2d9068`; gate 0x22b7e0 retorna falso). Gated `TER_NOSTORAGEPATCH`.
5. **statfs/statfs64** interceptados (my_statfs64 mede GAMEDIR real).
6. **FORTIFY bionic** (`__memmove_chk`/`__strlen_chk`/`__vsnprintf_chk`/`__memcpy_chk`/`__strcpy_chk`/
   `__strcat_chk`/`__snprintf_chk`/`__FD_SET_chk`) stub→impl reais (heap corruption "free invalid size").
   **strlcpy/strlcat** impl reais. readlink + wide-char (swprintf/wcs*/isw*/tow*) passthrough.
7. **🔑 pthread bionic→glibc** (RAIZ do SIGBUS pós-il2cpp_init): cond/mutex/rwlock eram passthrough
   (bionic struct + glibc op = ponteiro lixo). `install_pthread_shim()`/`patch_pthread_shim()` wira
   o conjunto completo → `pthread_fake.c`. Gated `TER_NOPTSHIM`.
8. **SDK_INT**: `GetStaticIntField(SDK_INT)`→30 (era 0 → Unity abortava).
9. asset_open tira prefixo "assets/".

## 🪤 ARMADILHAS APRENDIDAS
- **objdump rotula `@plt` por heurística — pode ERRAR.** O crash "num_get" era na verdade
  `pthread_cond_wait` (confirmar símbolo pelo GOT slot via `readelf -r <offset>`).
- so_resolve só resolve UNDEF; símbolos WEAK DEFINIDOS (C++ templados) já são bindados por
  so_relocate (base+st_value) — OK.
- regex SAFE do gerador precisa cobrir bionic-isms (strlcpy, `__*_chk`, wide-char, setjmp);
  senão viram stub-0 = corrupção silenciosa. pthread mutex/cond NÃO podem ser passthrough.

## 📋 PRÓXIMOS MUROS PROVÁVEIS (depois do guid)
1. Carregar `data.unity3d` (asset pack) — pode precisar do path certo no getAssetPackPath / VFS.
2. **Criar contexto GLES2 no Mali fbdev** — Unity cria EGL via libEGL real do Mali + ANativeWindow
   (my_aw_* → g_fbdev_win). Pode ter quirks (config, força `-force-gles20` já default no cmdline).
3. 1º frame → `shot.ppm`. Quirks Mali Utgard depois (highp→mediump, FBO depth-stencil, etc.).
4. Áudio (opensles_shim/FMOD) e controle (gamepad.c) — fase final.
