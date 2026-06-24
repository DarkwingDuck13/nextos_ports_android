# Chrono Trigger → Mali-450 (so-loader)

Port do **Chrono Trigger** (versão Android, engine **Cocos2d-x 3.14.1**, GLES2 nativo) rodando no **Mali-450 (Utgard, fbdev)** via so-loader do `libchrono.so` Android. **Jogável**: imagem + controle físico (padrão Xbox) + áudio + texto em inglês, 0 crash. Empacotado no padrão PortMaster (SELECT+START sai). **BYO-data** — você fornece os assets do APK que possui legalmente.

## Estado
- ✅ Render GLES2 1280x720 (título → menu → gameplay).
- ✅ **Controle físico nativo** (cocos `Controller`), normalizado pro padrão **Xbox** via `SDL_GAMECONTROLLERCONFIG` + gptokeyb pro hotkey de sair.
- ✅ Áudio (OpenSL ES → SDL2/Pulse) limpo.
- ✅ Texto/UI em **inglês** (sem japonês).

## Destraves (todos no `src/`, sem atalho)
- **so-loader 2 módulos**: carrega `libc++_shared.so` (libc++ do Android, `std::__ndk1`) como módulo auxiliar e resolve a engine contra ela.
- **Canário bionic** no TLS (`tpidr_el0+0x28`) com pad `_Thread_local`; **pthread_attr** bionic(56B)≠glibc(64B) (no-ops); **stdio `__sF`** bionic→glibc (wrappers); stubs bionic-only.
- **JNI `GetStringChars` UTF-16** (cocos `StringUtils` usa esse caminho).
- **Inglês**: o jogo decide idioma por **região** (`getLocationCode`), não por linguagem — forçado pra inglês.
- **Fontes da UI**: o jogo renderiza texto via `Cocos2dxBitmap.createTextBitmapShadowStroke` (fonte default do sistema); reimplementado nativo com **FreeType + Roboto**.
- **Controle (raiz)**: a ABI de `nativeControllerButtonEvent`/`AxisEvent` leva `jstring vendorName` ANTES do `controllerId` — sem isso os args deslocam e o keycode chega errado. Corrigido + `isConnected` forçado pra toda cena processar o controle.
- **Áudio (raiz da gagueira)**: o ring de saída era mantido raso demais vs. o consumo do callback SDL → underrun constante. Fix: alvo de buffer fixo + thread de áudio dedicada (refill desacoplado do framerate) + remoção de log em hot-path de I/O.

## Build
`./build.sh` (toolchain aarch64; FreeType do sysroot). Saída: binário `chrono`.

## Empacotamento (PortMaster)
- Launcher: `payload/Chrono Trigger.sh` → `/storage/roms/ports_scripts/`.
- Dados (BYO): binário + `libchrono.so`/`libc++_shared.so`/`libencrypt.so` + `assets/` (do APK) + `Roboto-Regular.ttf` → `/storage/roms/ports/chrono/`.
- Controle padrão Xbox via `SDL_GAMECONTROLLERCONFIG`; gptokeyb2 padrão pro SELECT+START.
