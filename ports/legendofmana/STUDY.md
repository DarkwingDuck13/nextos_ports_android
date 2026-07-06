# Legend of Mana Android 2021 - estudo inicial de port

APK analisado: `/home/felipe/Downloads/legend-of-mana-2021.1122.1-mod.apk`

Alvo de teste: `root@192.168.31.79`

Estado: checkpoint visual bom salvo, port ainda nao finalizado.

> 2026-07-06: causa raiz dos elementos PRETOS diagnosticada — ver
> `ESTUDO-PRETOS-CLUT-2026-07-06.md` (GL_ALPHA vs shaders lendo .r/.g em
> GLES2; fix proposto = expandir uploads GL_ALPHA para LUMINANCE_ALPHA).

## CHECKPOINT 2026-07-06 11:45 - imagem boa restaurada

Este e o estado que deve ser retomado na proxima sessao.

Alvo sagrado de teste: `root@192.168.31.79`. Nao trocar para outro aparelho/IP
sem pedido explicito do Felipe.

Build local atual:

```text
ports/legendofmana/legendofmana
ELF 64-bit LSB pie executable, ARM aarch64
```

Deploy atual confirmado em:

```text
/storage/roms/ports/legendofmana/legendofmana
```

Estado visual confirmado pelo Felipe depois do rollback:

```text
OK: voltou imagem limpa.
OK: logos iniciais aparecem.
OK: loading aparece.
OK: menu principal aparece com fundo.
OK: opcoes/animacoes do menu aparecem, incluindo New Game.
FALTA: titulo/logo "Legend of Mana" no menu principal.
FALTA: personagens na selecao ainda aparecem pretos.
FALTA: alguns fundos/elementos ainda ficam pretos.
NAO FINALIZADO: gameplay/audio/controles ainda precisam validacao completa.
```

Captura local deste checkpoint:

```text
/tmp/lom6_bgra.png
```

O frame capturado localmente durante a retomada mostrou o logo `M2` em fundo
preto, antes de o jogo avancar ate loading/menu no aparelho.

Logs copiados do aparelho neste estado:

```text
ports/legendofmana/logs/legendofmana-debug-20260706-good.log
ports/legendofmana/logs/legendofmana-run-20260706-good.log
```

Observacao: o final dos logs esta cheio de `GgcGetStatusSignIn -> 0`. Filtrar
esse spam quando for procurar GL/asset/CLUT.

### O que ficou no codigo neste checkpoint

Manter:

```text
src/main.c
- so-loader direto de libmain.so.
- Play Asset Delivery fake mantendo os counts originais:
  install=3, fastfollow=1, ondemand=2, download=3.
- USE_ONDEMAND_ASSET_PACK=1.
- hooks que desligam VirtualPad Android.
- crash handler/logs, sem derrubar o port em sinais fatais gerados pelo jogo.

src/jni_shim.c
- g_obb_path em /storage/roms/ports/legendofmana/payload/assets/
- caches JNI ampliados.

src/util.c
- log em /tmp/legendofmana-debug.log.
- path map para ASSETS: e :ASSET:.

src/imports.c
- ASTC decode usando libs/libsor4astc.so.
- wrappers de glCompressedTexImage2D/glTexImage2D/glTexSubImage2D.
- conversao BGRA -> RGBA quando necessario.
- pixel-store compat para row/skip/alignment.
- glShaderSource com patch highp -> mediump.
- glTexParameteri ignora 0x813d e 0x8072, converte GL_CLAMP para
  GL_CLAMP_TO_EDGE e filtros mipmap/invalidos para GL_LINEAR.

src/egl_shim.c
- eglGetProcAddress intercepta apenas:
  glCompressedTexImage2D, glTexImage2D, glTexParameteri,
  glTexSubImage2D, glPixelStorei.
```

### O que NAO repetir

Estas experiencias quebraram o estado visual bom e devem ser usadas apenas
como aviso negativo:

```text
NAO voltar a preservar highp globalmente.
- Resultado: blocos coloridos/ruido onde deveria haver logo/UI/personagem.
- O patch highp -> mediump deve continuar ativo por enquanto.

NAO reativar glTextureSafeDefaults.
- Resultado: texturas globais quebradas, sem fundo e ruido/chiado visual onde
  deveriam estar botoes e elementos de UI.

NAO trocar mipmap/min/mag para GL_NEAREST global.
- O estado bom usa fallback para GL_LINEAR.

NAO transformar glGenerateMipmap em no-op global.
- O estado bom passa glGenerateMipmap direto.

NAO interceptar globalmente glBlendEquation/glBlendFunc,
glFramebufferTexture2D, glVertexAttribPointer, glEnable ou glDisable.
- O estado bom usa passthrough direto para essas funcoes.

NAO usar ports incompletos/WIP como base positiva.
- Regra do Felipe: referencia so port 100% funcional.
```

### Hipotese atual

O problema restante parece estar concentrado no caminho PSX/CLUT/palette e/ou
shader especifico de sprite/personagem, nao mais em asset-pack geral.

Simbolos importantes em `payload/lib/arm64-v8a/libmain.so`:

```text
MOGLShader3DSpritePsx
TransferPSXClut
DrawMeshPSXClut
M2Tim::DecodeTim
CPaletteTransfer
MOGLTexture::AttachTexture()
MTex::CreateTexture
MOGLBase::TransferPSXClut
```

Enderecos anotados:

```text
MOGLTexture::AttachTexture() = 0x8968fc
MTex::CreateTexture        = 0x89e8d4
MOGLBase::TransferPSXClut  = 0x895614
.text                      = 0x570cb4
```

Objdump correto:

```text
/home/felipe/NextOS-Elite-Edition/build.NextOS-Retro-Elite-Edition-Amlogic-old.aarch64-4/toolchain/bin/aarch64-libreelec-linux-gnu-objdump
```

Proxima sessao:

```text
1. Nao mexer em estado global de textura/blend.
2. Capturar /tmp/legendofmana-debug.log e /tmp/legendofmana-run.log do estado bom.
3. Instrumentar de forma cirurgica shaders/programas PSX/CLUT e uniforms.
4. Mapear texturas CLUT/TIM: unidade ativa, formato, tamanho, pixel-store,
   upload inicial e subimage.
5. Testar uma mudanca pequena por vez, sempre com rollback para este checkpoint.
```

### Comandos do checkpoint

Build:

```sh
cd /home/felipe/nextos_ports_android/ports/legendofmana
./build.sh
```

Deploy no aparelho correto:

```sh
ssh -F /dev/null -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@192.168.31.79 'killall legendofmana 2>/dev/null; sleep 1; mount -o remount,rw /storage/roms 2>/dev/null; mount | grep " /storage/roms "'
rsync -rltvP legendofmana root@192.168.31.79:/storage/roms/ports/legendofmana/legendofmana
```

Run:

```sh
ssh -f -F /dev/null -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@192.168.31.79 'rm -f /tmp/legendofmana-run.log /tmp/legendofmana-debug.log; cd /storage/roms/ports/legendofmana; ./legendofmana >/tmp/legendofmana-run.log 2>&1 &'
```

Captura framebuffer:

```sh
ssh -F /dev/null -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@192.168.31.79 'dd if=/dev/fb0 bs=3686400 count=1 2>/dev/null' > /tmp/lom6.fb
ffmpeg -y -hide_banner -f rawvideo -pix_fmt bgra -s:v 1280x720 -i /tmp/lom6.fb -frames:v 1 -f image2 /tmp/lom6_bgra.png
```

## Veredito

Nao e Cocos2d-x. Chrono Trigger nao deve ser a base principal.

O jogo usa engine nativa M2/Square Enix em `libmain.so`, com NativeActivity,
GLES2, OpenSL ES, AAssetManager, PSB e pacotes GFS. O caminho correto e
so-loader ARM64 com ponte Android/Bionic -> Linux/Glibc, mais shims de
NativeActivity, assets, EGL/GLES2, OpenSL e input.

O alvo foi confirmado como `Amlogic-old.aarch64`, entao o APK ARM64 pode ser
testado diretamente. Se o mesmo pacote fosse usado em alvo 32-bit-only, nao
serviria.

## Device confirmado

SSH:

```text
Linux EMUELEC 3.14.79 ... aarch64 GNU/Linux
NextOS-Retro-Elite-Edition 4.8.2-Nexus_devel_20260602211610
LIBREELEC_ARCH=Amlogic-old.aarch64
Hardware: Amlogic
model name: Amlogic S905L rev e
MemTotal: 852864 kB
```

Video/GPU:

```text
/dev/fb0
/dev/fb1
/dev/mali
/dev/ion
/usr/lib/libEGL.so
/usr/lib/libGLESv2.so
/usr/lib/libMali.m450.so
/usr/lib/libMali.so
```

Audio:

```text
card 0: AML-M8AUDIO
playback device 0: I2S
playback device 1: SPDIF
playback device 2: PCM
```

Input:

```text
/dev/input/js0
/dev/input/event2
USB Gamepad Vendor=0810 Product=0001
```

Deploy padrao:

```text
/storage/roms/ports/legendofmana
```

`/storage/roms` esta em VFAT e tem cerca de 60 GB livres. Nao usar
`/storage` puro para payload grande; ele tem pouco espaco livre.

## APK

Tamanho:

```text
669 MB zip
614 entradas
```

Arquivos relevantes:

```text
classes.dex
resources.arsc
assets/and/spec.txt
assets/anddata/boot/font/testfont.psb.m
assets/assetpacks_000.gfs
assets/assetpacks_001.gfs
assets/assetpacks_002.gfs
assets/assetpacks_003.gfs
lib/arm64-v8a/libmain.so
lib/arm64-v8a/libmyloader.so
lib/arm64-v8a/libpairipcore.so
lib/arm64-v8a/libRMS.so
```

Nao ha `armeabi-v7a`.

Native libs:

```text
libmain.so       78,489,616 bytes
libmyloader.so       10,080 bytes
libpairipcore.so    327,000 bytes
libRMS.so           513,832 bytes
```

`libRMS.so` tem header ELF problematico/corrompido para `readelf`. Evitar no
primeiro loader.

## Manifest / Java

Package:

```text
com.square_enix.android_googleplay.LOMww
```

NativeActivity:

```text
android.app.MyNativeActivity
android.app.lib_name=myloader
android.app.func_name=AMyLoader
screenOrientation=landscape
```

Application Java:

```text
com.pairip.application.Application
```

Riscos Java:

```text
PairIP
Google Play Licensing
Play Core Asset Packs
Saved Games / cloud save
```

Estrategia inicial: nao executar a camada Java. Carregar `libmain.so`
diretamente e ignorar `libpairipcore.so`/`libRMS.so` ate prova em contrario.

## libmyloader.so

Exports:

```text
AMyLoader
getJavaPackageName
onDestroy
android_getCpuFamily
android_getCpuFeatures
android_getCpuCount
android_setCpu
```

Strings:

```text
lib%s-neon.so
main
lib%s.so
loader
dlopen
dlsym
```

Provavelmente carrega `libmain.so` por `dlopen`. Nao e a base do nosso loader;
o port deve carregar `libmain.so` diretamente.

## libmain.so

ELF:

```text
ELF64 AArch64
Android API 21
NDK r23
BIND_NOW
not stripped
```

NEEDED:

```text
libc.so
libm.so
liblog.so
libGLESv2.so
libEGL.so
libOpenSLES.so
libz.so
libandroid.so
libdl.so
```

Entradas e lifecycle:

```text
ANativeActivity_onCreate
android_main
android_app_read_cmd
android_app_pre_exec_cmd
android_app_post_exec_cmd
app_dummy
main_OnCreate(char const*, AAssetManager*, tag_app_global_state*)
main_OnStart()
main_OnResume()
main_OnPause()
main_OnStop()
main_OnDestroy()
main_OnSaveState(void**, int*)
M2Main(int, char**)
M2MainC
M2Init
M2ExecApp
M2InitializeContext
```

Isto permite duas rotas de boot:

1. Simular `ANativeActivity_onCreate` e deixar o `android_native_app_glue`
   interno chamar `android_main`.
2. Chamar mais direto `main_OnCreate`/`M2MainC`, se o contexto minimo for
   suficiente.

A rota 1 tende a ser mais fiel; a rota 2 e boa para teste rapido de imports.

## ABI / Bionic

Imports criticos:

```text
__sF
__errno
android_set_abort_message
dl_iterate_phdr
pthread_*
pthread_attr_*
pthread_cond_*
pthread_mutex_*
pthread_rwlock_*
sem_*
localeconv/newlocale/uselocale/freelocale
stat/statfs64/open/read/write/lseek/fopen/fread/fwrite
```

Necessario:

- `pthread_bridge` para mutex/cond/rwlock/once/attr bionic.
- `__sF`/stdio bionic.
- `__errno` bionic.
- wrappers de locale.
- stubs Android para log/properties/abort.
- cuidado com `BIND_NOW`: todos imports precisam resolver antes de rodar init.

## Assets / GFS

`assets/and/spec.txt`:

```text
M_SPEC = and
SOFTKEYPAD_TYPE = 3
PORTRAIT = 0
USE_ONDEMAND_ASSET_PACK = 1
INSTALL_ASSET_PACK_NUM = 3
FASTFOLLOW_ASSET_PACK_NUM = 1
ONDEMAND_ASSET_PACK_NUM = 2
USE_FULL_SCREEN = 1
USE_CLOUD = 1
CLOUD_BASE = 2
USE_ALPHA_CHANNEL = 1
USE_STENCIL_BUFFER = 0
KEEP_SCREEN_ON = 1
EXTERNAL_DATA_PATH = .
```

GFS:

```text
assetpacks_000.gfs  284,081 bytes
assetpacks_001.gfs  342,833,843 bytes
assetpacks_002.gfs  238,895,133 bytes
assetpacks_003.gfs   90,762,276 bytes
```

Headers:

```text
assetpacks_000.gfs: GFS3 + varios headers gFS3
assetpacks_001.gfs: gFS3 ... mdf\0 ... zlib
assetpacks_002.gfs: gFS3 ... mdf\0 ... zlib
assetpacks_003.gfs: gFS3 ... mdf\0 ... zlib
```

`assetpacks_000.gfs` parece indice/manifest. Categorias por strings:

```text
4212 system
 479 hdmapimg
 479 hdmap
 274 sound
 102 event_patch
   8 text
   6 config
   4 battle_popup
   1 debugmenu
```

Entradas relevantes:

```text
config/touch_area.psb.m
config/touch_area_steam.psb.m
config/sound_control.psb.m
sound/*.psb
system/movie/*.webm
event_patch/*.adx.bin.m
```

Imports Android de assets:

```text
AAssetManager_open
AAsset_openFileDescriptor
AAsset_read
AAsset_seek
AAsset_getLength
AAsset_close
```

Simbolos M2:

```text
M2SetAssetManager
M2ReadSpecFile
M2AssetFileOpen/Read/Seek/Tell/Close
M2GasFsCreateFileMap
M2GasFsGetFileMap
M2OnDemandAssetPack*
GasFsSupport::*
```

Estrategia inicial:

- manter layout do APK dentro de `assets/`;
- servir tudo via AAsset shim;
- fazer `M2OnDemandAssetPack*` responder como "ja instalado/local";
- se necessario, patchar `M2ANDUseOnDemandAssetPack=0` ou ajustar paths
  `M2ANDAssetFolder`/`M2ANDBaseFolder`/`M2ANDSpecFileName`.

## Input

Imports Android:

```text
AInputQueue_attachLooper
AInputQueue_detachLooper
AInputQueue_getEvent
AInputQueue_preDispatchEvent
AInputQueue_finishEvent
AInputEvent_getDeviceId
AInputEvent_getSource
AInputEvent_getType
AKeyEvent_getAction
AKeyEvent_getKeyCode
AKeyEvent_getMetaState
AKeyEvent_getRepeatCount
AMotionEvent_getAction
AMotionEvent_getFlags
AMotionEvent_getPointerCount
AMotionEvent_getPointerId
AMotionEvent_getRawX
AMotionEvent_getRawY
AMotionEvent_getX
AMotionEvent_getY
```

Nao apareceu `AMotionEvent_getAxisValue` nos imports. Isso indica que o caminho
Android puro le touch/key, nao eixos analogicos de joystick via MotionEvent.

Callbacks e classes internas:

```text
M2TouchPad_OnTouch
M2TouchPad_OnMove
M2TouchPad_OnRelease
M2HardKey_OnChange
M2HardKey_OnChangeAnalogStick
M2HardKey_OnChangeAnalogButton
M2SetVolumeButtonMode
MSoftKeyPad
MTouchPad
MHardKey
MInput
MInputHub
MSingleInputHub
TouchOperation::VirtualPad
TouchOperation::TouchInputTask
TouchOperation::VirtualPad::ConvertButtonBitsPSToMInput
```

Estrategia:

- primeiro boot: injetar eventos simples por callbacks M2, se os enderecos
  resolverem;
- alternativa fiel: montar `AInputQueue` fake e entregar `AKeyEvent`/
  `AMotionEvent`;
- no device, mapear `js0/event2` por SDL ou evdev;
- gptokeyb pode ser usado para teclado/dpad, mas o ideal aqui e traduzir
  direto para `M2HardKey_*` e `M2TouchPad_*`;
- descobrir bitmask de `M2HardKey_OnChange` na pratica/log, usando menu/title.

## Video

Imports EGL:

```text
eglGetDisplay
eglInitialize
eglGetConfigs
eglChooseConfig
eglGetConfigAttrib
eglCreateContext
eglCreateWindowSurface
eglMakeCurrent
eglQuerySurface
eglSwapBuffers
eglDestroySurface
eglDestroyContext
eglTerminate
eglGetError
ANativeWindow_setBuffersGeometry
```

Imports GLES2 principais:

```text
glShaderSource
glCompileShader
glGetShaderiv
glGetShaderInfoLog
glCreateProgram
glAttachShader
glLinkProgram
glUseProgram
glGetAttribLocation
glGetUniformLocation
glUniform*
glVertexAttribPointer
glDrawElements
glTexImage2D
glTexSubImage2D
glCompressedTexImage2D
glCopyTexImage2D
glReadPixels
glBindFramebuffer
glFramebufferTexture2D
glCheckFramebufferStatus
glStencil*
```

Simbolos:

```text
M2LoadShader
M2UnloadShader
M2DrawView
M2RequestDraw
M2GetGpuRenderer
M2SetTexturesBroken
MOGLShader*
MOGLTexture
MOGLBase
MTex::CreateTexture
MTex::CreateCompressedTexture
ETCTextureDecompress
PVRTDecompressETC
MMovieTexture
WebmDecoder
WebmVpxDecoder
WebmVorbisDecoder
```

Shaders embutidos:

```text
precision mediump float;
varying highp vec2 v_texCoord;
uniform highp vec2 u_scrSize;
#define HIGHP highp
#define LOWP lowp
gl_FragColor = ...
```

Riscos Mali-450:

- `highp` no fragment shader pode falhar. Interceptar `glShaderSource` e
  converter highp para mediump no fragment shader se necessario.
- usa FBO/render-to-texture; cuidar de `glClear` em FBO.
- usa textura comprimida/ETC e decompress interno; validar alpha/ETC2.
- `USE_STENCIL_BUFFER=0`, mas ainda ha imports stencil. Nao habilitar stencil
  se nao precisar.
- WebM usa decoders internos VP8/VP9/Vorbis, deve sair como textura + PCM.

## Audio

Imports:

```text
slCreateEngine
SL_IID_ENGINE
SL_IID_PLAY
SL_IID_BUFFERQUEUE
SL_IID_VOLUME
SL_IID_ANDROIDCONFIGURATION
SL_IID_EFFECTSEND
SL_IID_PLAYBACKRATE
SL_IID_SEEK
```

Simbolos:

```text
SL_SetAssets(AAssetManager*)
SL_MP3_SetChannel
SL_OGG_SetChannel
SL_PCM_SetChannel
SL_PACKET_SetChannel
M2AndAudio::CreateDevice
M2AndAudio::DestroyDevice
M2AndAudio::LoadStream
M2AndAudio::LoadStreamAsPacket
M2AndAudio::StartChannel
M2AndAudio::StartChannelAsPacket
M2AndAudio::SetChannelVolume
M2AndAudio::SetChannelPanpot
M2AndAudio::SetChannelPitch
M2AndAudio::SetChannelCue
M2AndAudio::audioTimerThread
MSound::*
WebmVorbisDecoder
```

Assets de audio:

```text
sound/*.psb
event_patch/*.adx.bin.m
system/movie/*.webm
```

Estrategia:

- reutilizar `opensles_shim.c` dos ports funcionais;
- usar um unico device SDL/audio backend automatico do sistema;
- nao forcar `SDL_AUDIODRIVER`;
- aumentar folga de buffer se houver underrun, seguindo licao do Chrono.

## Bases de referencia

Usar como base principal:

- `facilitando_o_trabalho/kit_essencial`
- `ports/revc` para ARM64 so-loader, bionic bridge, GLES/OpenSL/AAsset
- `ports/crazytaxi` para ARM64 NextOS/Mali e deploy em `/storage/roms/ports`
- `ports/shantae` para NativeActivity/input/app glue, apenas como conceito

Nao usar como base principal:

- Chrono Trigger: Cocos2d-x, nao M2.
- Magic Rampage: GS2D/FMOD JNI, nao NativeActivity M2.
- Secret of Mana: plandroid/GLES1 JNI, nao M2.

## Plano tecnico de bring-up

1. Criar `ports/legendofmana`.
2. Extrair payload do APK para `payload/` local e depois para
   `/storage/roms/ports/legendofmana`.
3. Criar loader ARM64 com `so_util`, `pthread_bridge`, `egl_shim`,
   `opensles_shim`, `android_shim`, `asset_shim` e `imports`.
4. Resolver primeiro todos imports de `libmain.so`, sem PairIP/RMS.
5. Chamar init array e testar rota de boot.
6. Montar AAsset local para `assets/and/spec.txt`, `anddata` e GFS.
7. Stubar Play Asset Delivery como local/instalado.
8. Abrir EGL/GLES2 no fbdev Mali e capturar primeira tela.
9. Ativar OpenSL shim.
10. Injetar input minimo via callbacks M2 e depois mapear controle fisico.

## Riscos abertos

- PairIP/licenca pode estar dentro do fluxo nativo, apesar de a camada Java ser
  bypassavel.
- Asset packs podem exigir path/estado especifico de Play Core.
- M2 pode depender de `ANativeActivity` real em vez de `main_OnCreate` direto.
- Memoria: device tem menos de 1 GB RAM, e o jogo usa packs grandes.
- Controle: bitmask de `M2HardKey_OnChange` ainda precisa ser descoberto.
- Audio: se OpenSL usa configuracoes Android especificas, shim pode precisar de
  mais interfaces.
