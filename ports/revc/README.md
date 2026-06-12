# reVC (GTA: Vice City) — port Android → NextOS / Mali-450 (so-loader)

**Status: 🏆 JOGÁVEL 100%** — mundo 3D, controle, áudio, menu de gráficos e **NPCs** todos
funcionando no Amlogic-old (S905X, **Mali-450 Utgard, GLES2-only, fbdev**).

Este é o **primeiro port feito do zero** com o framework `nextos_ports_android`, e serve de
**referência/template** para portar outros jogos Android (NativeActivity/SDL2 ARM64) para
Linux+Mali. As "Receitas" abaixo são o conhecimento reutilizável.

---

## 1. O que é (e o que NÃO é)

- Carregamos a engine **Android** do reVC (`libreVC.so`, arm64, do APK open-source `BuildReVC`)
  dentro de um processo **glibc aarch64 normal**, via um **so-loader** (mapeia o ELF, resolve
  relocations/imports, roda init_array, chama a entrada).
- **NÃO** recompilamos a engine. **Todos os fixes são por interceptação** (stubs + hooks de
  GL/SDL no loader). Isso mantém o binário Android otimizado (NDK clang + LTO) e torna as
  correções portáveis.
- Por que a versão Android e não o build nativo? Ports Android afinados pra GLES2 mobile rodam
  "lisos" no Mali-450.
- **Dados = BYO (Bring Your Own).** Os assets do Vice City (gta3.img, models, txd, anim, audio,
  text…) são copyright e **não** estão no repo. O usuário traz sua cópia original.

---

## 2. Arquitetura: so-loader de 2 MÓDULOS

A `libreVC.so` foi compilada contra o **libc++ do NDK** (namespace inline `__ndk1`), que o
libstdc++ do host **não** tem. Solução = carregar dois módulos:

```
Módulo A = libc++_shared.so   (NDK, ABI __ndk1)  -> provê std::__ndk1::*, operator new/delete,
                                                    __cxa_*, _Unwind_* com a ABI exata
Módulo B = libreVC.so         (a engine)         -> resolve UND via:
              tabela de stubs + so_snapshot_symbols(A) + dlsym(RTLD_DEFAULT)
              das libs do DEVICE (SDL2/GLESv2/EGL/OpenAL/mpg123/glibc) pré-carregadas
              com dlopen(RTLD_GLOBAL).
```

Entrada = **`SDL_main`** (a engine é SDL2; não é NativeActivity/JNI). Fluxo em `src/main.c`.

Arquivos:
- `src/so_util.c/.h` — loader ELF (multi-PT_LOAD, RELA, GOT, init_array, snapshot de símbolos).
- `src/main.c` — orquestra os 2 módulos, crash handler, patch de runtime, chama SDL_main.
- `src/stubs.c` — stubs bionic + **todos os hooks de GL/SDL** (onde moram os fixes).
- `src/pthread_bridge.c` — ponte de ABI pthread bionic→glibc.
- `build.sh` — cross-compila o loader (toolchain aarch64 da NextOS).
- `run.sh` — launcher estilo PortMaster (controles, fullscreen, symlink de dados).

---

## 3. 🔑 RECEITAS Mali-450 / GLES2 (reutilizáveis em qualquer port)

Estas são as correções que destravaram o jogo. Servem de checklist pros próximos.

### 3.1 Loader / ABI
- **R_AARCH64_ABS64 para símbolo UNDEF**: o framework antigo resolvia como `base+0` (bug). Tratar
  ABS64 igual GLOB_DAT (resolver por nome + addend). Foi o que travava `RwEngineOpen`.
- **Multi-PT_LOAD (NDK r27)**: mapear TODOS os segmentos PT_LOAD ancorados em `load_base`
  (vaddr 0), não assumir 2 segmentos. Endurecer só o text (RX) no fim; resto fica RWX.
- **Ponte pthread ABI bionic→glibc** (`pthread_bridge.c`): `pthread_mutex_t` do bionic (40B) ≠
  glibc (48B). Guardamos um **ponteiro** pro objeto glibc real no storage bionic (lazy-init).
  `IS_HEAP_PTR(>0x10000)` distingue ponteiro de inicializador-estático bionic. TODO mutex vira
  **recursivo** (evita self-deadlock). cond com `CLOCK_MONOTONIC` (default do bionic). **Sem
  isso = deadlock** (foi o que travava a cutscene de abertura).

### 3.2 Shaders (Mali-400/450 GP — vertex)
Reescrevemos a fonte no hook `glShaderSource`:
- **`highp` → `mediump`**: o GP do Utgard **não** suporta `highp`.
- **`MAX_LIGHTS 8` → `4`**: 8 luzes = 316 vec4 > limite. **`MAX_VERTEX_UNIFORM_VECTORS = 256`**
  no Mali-450 (medido) — cuidado com arrays de uniform grandes.
- **im2d z-fix**: `gl_Position.xyz *= gl_Position.w` joga o Z fora do clip no Mali → 2D/HUD some.
  Trocar por `gl_Position.xy *= gl_Position.w; gl_Position.z = 0.0;`.

### 3.3 Texturas (GLES2)
- **Formatos sized inválidos**: `GL_RGBA8`(0x8058)→`GL_RGBA`, `GL_RGB8`→`GL_RGB` no `glTexImage2D`
  (GLES2 só aceita formato base como internalformat).
- **🏆 `GL_TEXTURE_MAX_LEVEL` NÃO existe em GLES2** (é GLES3/desktop). A librw usa ele pra limitar
  a cadeia de mipmaps; no GLES2 é ignorado → texturas com **mipmap incompleto ficam incompletas
  → renderizam PRETO**. Foi o que deixava os **NPCs pretos/bugados** (player não usava).
  **Fix** (`my_glTexParameteri`): forçar `GL_TEXTURE_MIN_FILTER` de `*_MIPMAP_*` para `GL_LINEAR`
  (completude não exigida) + ignorar `GL_TEXTURE_MAX_LEVEL`.
- Mali-450: **sem texture-compression s3tc/DXT** e **`MAX_VERTEX_TEXTURE_UNITS = 0`** (sem VTF →
  sem bone-texture).

### 3.4 SDL / janela / input
- **Resolução**: o SDL3-mali reporta o connector (1920x1080); forçar 1280x720 nos hooks
  `SDL_CreateWindow`/`GetWindowSize`/`Get*DisplayMode` (e setar `format`+`refresh` p/ a lista de
  modos deduplicar).
- **Controle (Android-ism)**: `SDL_InitSubSystem` no Android só inicia VIDEO → forçar
  `|JOYSTICK|GAMECONTROLLER`, senão o pad nem abre.
- **Remap de botões**: estude a FONTE pra achar o mapa ação→botão (não chute). No reVC, o
  `ControllerConfig.cpp` (Xbox-branch) define A→bater, B→entrar, X→acelera, Y→freio; combine com
  o mapa físico→SDL do `gamecontrollerdb` e remapeie em `SDL_GameControllerGetButton` (polling;
  o menu usa eventos, não afetado).

### 3.5 Paths / dados
- A librw abre arquivos com paths **relativos** (`./models/`, `./audio/…`) relativos ao cwd, mas
  a engine faz `chdir` em runtime → quebra. **Interceptar `fopen`** e redirecionar os relativos
  das pastas de dados (models/audio/anim/data/text/txd/movies/mp3/…) para a pasta real, com
  resolução **case-insensitive**. ⚠️ Só os RELATIVOS — redirecionar os absolutos quebra o GXT.
  (O áudio `./audio/intro1.wav` da cutscene não-redirecionado = **freeze** esperando o áudio.)

### 3.6 Menu de gráficos (patch de runtime)
- O menu de resolução faz `AsciiToUnicode(_psGetVideoModeList()[m_nDisplayVideoMode])`; com a
  resolução forçada o índice fica inválido → string nula → **crash**. Fix = **patch binário em
  runtime** (via `mprotect`) trocando o load do índice por um índice fixo válido. Ver
  `patch` em `main.c` (offset `0x1a645c`, achado por disassembly do `.so`).

---

## 4. Build

```bash
# precisa da toolchain aarch64 da NextOS (Amlogic-old)
cd ports/revc
./build.sh          # gera ./revc (o loader)
```

`build.sh` usa `aarch64-libreelec-linux-gnu-gcc` + sysroot. O loader linka só `-ldl -lm -lpthread`;
SDL2/GLESv2/EGL/OpenAL/mpg123 são resolvidos em runtime das libs do device.

## 5. Setup no device (BYO data)

```
/storage/roms/ports/revc/
├── revc                 # o loader (deste repo)
├── libreVC.so           # engine (deste repo)
├── libc++_shared.so     # runtime NDK (deste repo)
├── run.sh               # launcher
└── gamedata/            # <-- SEUS dados do Vice City (BYO; NÃO no repo)
    ├── models/gta3.img
    ├── data/  anim/  txd/  audio/  text/  movies/  neo/
    ├── reVC.ini          # [VideoMode] Width=1280 Height=720 Depth=32 Windowed=0
    └── gamecontrollerdb.txt
```
A engine procura os dados em `/storage/emulated/0/reVC` — o `run.sh` cria o symlink → `gamedata`.

## 6. Metodologia de debug (como diagnosticar os próximos)

1. **Estude a FONTE antes de chutar** (re3 é open). Foi assim que resolvemos controles e menu.
2. **Instrumente por hook**: logue formatos de textura, shaders, caps de GL (`glGetIntegerv`),
   chamadas suspeitas. Conclua com DADO, não palpite.
3. **gdb / crash handler**: registradores + backtrace (offsets `libreVC.so+0xXXXX`), depois
   `objdump`/`nm` no `.so` pra achar a função (mesmo stripped, dá pra disassemblar o site).
4. **Isole**: o que difere o que funciona do que não (ex.: player OK vs NPC bugado → não é
   skinning, é textura/mipmap).
5. **Compare com o build nativo**: se o nativo buga igual, a causa é da engine/Mali, não do loader.

---

## 7. Créditos / licença

- Engine: **reVC** (mrxenginner / projeto re3) — reimplementação open-source.
- Base do framework so-loader: ports `syberia`/`lswtcs` de **mtojek** (Apache-2.0).
- Port/loader/fixes: felc18-blip. **Uso próprio.** Dados do jogo são do usuário (BYO).
