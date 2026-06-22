# Castlevania: Symphony of the Night (DotEmu) → Mali-450 so-loader

Port de `com.dotemu.castlevania` (APK v1.0.6) para NextOS Amlogic-old (aarch64, Mali-450 fbdev, device .164).
**Início do zero 2026-06-22.** Objetivo: render + gameplay + controle + áudio, fluxo do jogo original.

## Arquitetura do binário (recon)
- `libsotn.so` 3.6MB, **SDL2 2.0.8 ESTÁTICO** (NDK r21d), arm64-v8a.
- NEEDED: libz, libandroid, libdl, libGLESv1_CM, libGLESv2, liblog, libm, libc. (EGL é dlopen "libEGL.so" pelo SDL.)
- Entry: SDLActivity padrão → `JNI_OnLoad` → `nativeSetupJNI` → `nativeRunMain` (= roda `SDL_main`).
- **Diferença vs Crazy Taxi:** SDL é estático ⇒ o JOGO cria janela/contexto GL e roda o próprio loop. O loader NÃO gerencia render loop — só carrega, resolve imports, monta JNIEnv falso, chama `nativeRunMain`, e injeta input chamando `onNativeKeyDown/onNativePadDown` exportados.
- Render: GLES2 nativo (#version 100). dlopen libEGL/libGLESv2 → Mali. ANativeWindow_fromSurface → fbdev_window.
- Texturas: PNG (assets/ui) + dados PSX no OBB.

## Assets
- APK `assets/`: game.properties + assets/ui/*.png (10 arquivos UI).
- OBB `assets/res2` = zip contendo `main.57.jp.konami.epjCastlevania2.obb` (230MB) = zip com TUDO sob `assets/`:
  sound(797) pack(422) hdbin(170) ui(42) pspbin(6) bin. + snapshots/.
- Estratégia: extrair OBB pra disco; servir via AAssetManager shim + SDL_AndroidGetExternalStoragePath. (RE em andamento p/ confirmar mecanismo exato.)

## Toolchain / device
- TC: `~/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain` (aarch64-libreelec-linux-gnu-gcc), sysroot tem libMali (GLESv2/EGL fbdev), SDL2.
- Device .164: `sshpass -p emuelec ssh root@192.168.31.164`. Mali-450 fbdev, 832MB RAM, /storage ~930MB livre.
- ⚠️ REGRA: matar+confirmar 0 instâncias por /proc/*/exe antes de lançar. Abrir foreground: `systemctl stop emustation; bash run.sh` (sem redirect/&/nohup → senão tela preta).
- Vídeo SDL_VIDEODRIVER=mali; áudio pulse.

## Diário
- s1 2026-06-22: recon completo. Scaffold de crazytaxi. Loader construído.
- s1 🎉 **RENDER ATINGIDO** 2026-06-22: tela de EULA (利用規約) do "悪魔城ドラキュラX 月下の夜想曲" renderiza com texto JP, caixa, fontes, botão OK no Mali-450. Fixes essenciais até aqui:
  1. **so_resolve fallback dlsym(RTLD_DEFAULT)** — resolve libc/GL/zlib do host automaticamente.
  2. **R_AARCH64_ABS64 p/ imports indefinidos** — so_relocate punha malloc etc. em text_base+0 (header ELF→SIGILL). Fix: ABS64 com st_shndx UNDEF é resolvido pelo so_resolve (não so_relocate).
  3. **stdio bionic __sF** — jogo usa &__sF[1/2] (stdout/stderr de bionic); glibc fprintf rejeita FILE fake. Wrappers sf_* mapeiam fake_sF→stream real (map_sf).
  4. **CANARY BIONIC (tpidr+0x28)** — libsotn lê stack-guard de tpidr_el0+0x28 (TLS slot 5 bionic); sob glibc esse campo do TCB muda → __stack_chk_fail. Fix (igual Bully/Dysmantle): `_Thread_local char g_bionic_guard_pad[256]` no loader → pad estável cobre tpidr+0x28.
  5. **assets case-insensitive** — OBB feito em FS case-insensitive (ex. `ui/Buttons/` vs pedido `ui/buttons/`). ci_resolve() resolve cada componente do path por strcasecmp.
  - dlopen self-ref (nativeRunMain dlopen libsotn+dlsym SDL_main), ANativeWindow_fromSurface→fbdev_window, jni_shim SDL2.0.8 (getNativeSurface non-null, etc.), onNativeResize p/ resolução, SDL_AUDIODRIVER=dummy (áudio depois).
  - Fluxo: load→relocate→resolve→init_array→JNI_OnLoad→nativeSetupJNI(+Audio+Controller)→onNativeResize(1280,720)→nativeRunMain→SDL_main. Vídeo = SDL android driver→EGL Mali fbdev (/dev/fb0 + /dev/mali mapeados).
  - Captura tela: `dd if=/dev/fb0 bs=5120 count=720` → BGRA 1280x720 → PNG.
- s1 🎉 **MENU PRINCIPAL ATINGIDO** 2026-06-22: EULA confirmada + menu principal (続ける/作成読込/設定/実績, logo, © 2025 Konami) renderiza. INPUT FUNCIONA:
  - **nativeAddJoystick é SDL 2.0.6-style SEM vendor/product**: `(env,cls, device_id, name, desc, is_accel, button_mask, naxes, nhats, nballs)`. Args deslocados davam nbtn/naxes errados.
  - **Critério de seleção do jogo** (process_joysticks @0x13af80): `NumButtons>=7 AND NumAxes>=4`. Registrar com button_mask=0xFFFF (nbtn=65535), naxes=6, nhats=1 → "joystick 'SOTN Controller' selected" ✓.
  - Após selecionado, onNativePadDown(BUTTON_A=96) confirma a EULA → menu.
  - Verificar joystick: chamar SDL_NumJoysticks/SDL_JoystickOpen/NumButtons via so_find_addr no loader.
- ⚠️ áudio: SDL_AUDIODRIVER=dummy → "RingBuffer::read asked 4096 only 0" (sem consumidor). Implementar JNI AudioTrack sink.
- s1 🎉🎉🎉 **JOGÁVEL — GAMEPLAY + ÁUDIO + CONTROLE COMPLETOS** 2026-06-22:
  - **GAMEPLAY**: navegado TODO o fluxo pelo controle (EULA→título→menu→作成/読込→entrada de nome→OK) até a abertura do jogo ("JOURNEY BACK TO 1792 / 1792年 トランシルバニア地方" + intro do castelo). Fluxo original preservado.
  - **ÁUDIO** ✅ (audio.c): jogo usa SDL "android" driver→JNI audioOpen/audioWriteShortBuffer. Sink = pipe pro `pacat` (PulseAudio). ⚠️ audioOpen retorna **0=sucesso** (SDL faz `cbz w0`, não o nº de frames!). jni_shim: NewShortArray/NewByteArray + GetShortArrayElements (handle==data ptr) + registro de arrays; audioWriteShortBuffer→write no sink. PCM flui contínuo.
  - **CONTROLE** ✅ (input.c): registrar joystick (nativeAddJoystick **SDL 2.0.6-style sem vendor/product**), critério de seleção do jogo = NumButtons>=7 && NumAxes>=4 (button_mask=0xFFFF,naxes=6,nhats=1). Confirmar=**B (círculo)**, voltar=A (japonês). Menu navega com **HAT LIMPO** (onNativeHat sozinho; combinar hat+stick+button CONFUNDE o menu). Jogo lê input via SDL_PumpEvents+PeepEvents.
  - **evdev real**: input.c lê /dev/input/event2 (" USB Gamepad", botões BTN_TRIGGER 0x120-0x12b, ABS X/Y/Z/RZ+HAT0X/Y)→onNativePad/Hat/Joy. open_gamepad detecta por BTN joystick/gamepad+ABS_X. Stick normalizado por EVIOCGABS. Default boot = modo evdev real (sem autotest).
  - **SAVES/EULA persistem**: /snapshots/→./snapshots/ mapeado (util.c). sotn.cfg salvo → boot limpo PULA a EULA, vai direto ao título.
  - **autotest** (debug): SOTN_AUTONAV="tokens" (d/u/l/r=hat, a/b/s=botões, t=touch OK, S=scroll, O=開始, 1-4=linhas do menu). SOTN_VERBOSE=1 liga logs do jogo.
  - **launcher ES**: ports_scripts/"Castlevania SOTN.sh" (cd gamedir; HOME+LD_LIBRARY_PATH; ./sotn). Sem gptokeyb (lemos evdev direto).
- FALTA (polimento): NextOS validar na TV (jogar a fase real além do intro), confirmar mapeamento de botões in-game ao gosto, empacotar R2/PortMaster, perf.
</content>
