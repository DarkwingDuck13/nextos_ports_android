# BADLAND port - triagem inicial

Fonte analisada:

- `/home/felipe/Downloads/BADLAND_Completo - v3.2.1.5_Tekmods.com.apk`
- APK 352 MB, `arm64-v8a`

## Decisao

Da para portar. Nao e Unity; e Cocos2d-x Android nativo com render GLES2 e audio FMOD Studio.

O risco principal nao e boot/input. O ponto duro e textura: o APK usa PVR v3 com ETC2 (`0x9274`/`0x9278`) e o alvo Mali-450/GLES2 nao deve receber ETC2 direto. Decisao para o primeiro boot: hook de `glCompressedTexImage2D` igual ao Dysmantle e decode forçado ETC2/EAC -> RGBA8888. Usar assets SD primeiro para controlar VRAM. Depois que abrir e jogar, otimizar para ETC1/dual-layer ETC1 se precisar.

## Biblioteca principal

Arquivos nativos importantes:

- `lib/arm64-v8a/libbadland.so` - engine/game, 22 MB
- `lib/arm64-v8a/libfmod.so`
- `lib/arm64-v8a/libfmodstudio.so`

`libbadland.so` precisa de:

- `libfmod.so`
- `libfmodstudio.so`
- `libGLESv2.so`
- `libandroid.so`
- `liblog.so`
- `libdl.so`
- `libc.so`
- `libm.so`

## Entry points JNI uteis

Render/Cocos:

- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit`
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender`
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume`
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause`
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnStart`
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnStop`

Assets/APK:

- `Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath`
- `Java_com_frogmind_badland_Badland_nativeInitializeAssetManager`
- imports `AAssetManager_fromJava`, `AAssetManager_open`, `AAsset_getLength`, `AAsset_read`, `AAsset_close`

Input:

- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin`
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove`
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd`
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesCancel`
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown`
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativePadAction`
- `Java_org_cocos2dx_lib_Cocos2dxRenderer_nativePadDeviceLost`

Audio:

- `Java_com_frogmind_badland_Badland_nativeInitFmod`
- `Java_com_frogmind_badland_Badland_nativeReportAudioProperties`
- `Java_com_frogmind_badland_Badland_nativeSetPreferredFmodOutput`
- `Java_com_frogmind_badland_Badland_nativeSetForceAudioTrackOutput`
- `Java_com_frogmind_badland_Badland_nativeSetAudioPaused`

## Assets

Audio FMOD:

- `assets/audio_fmodstudio/MasterBank.bank`
- `assets/audio_fmodstudio/MasterBank.strings.bank`
- `assets/audio_fmodstudio/BadlandSoundEvents_*.bank`

Video/textura:

- `assets/graphics/hd/**/*.pvr`
- `assets/graphics/hd_common/**/*.pvr`
- `assets/graphics/sd/**/*.pvr`
- `assets/graphics/sd_common/**/*.pvr`

Headers PVR conferidos:

- `assets/graphics/hd/dawn/background-hd.pvr`: PVR v3, pixelFormat `12`, 1024x1024
- `assets/graphics/sd/dawn/background.pvr`: PVR v3, pixelFormat `12`, 512x512
- `assets/graphics/hd/boosters-hd.pvr`: PVR v3, pixelFormat `14`, 512x256
- `assets/graphics/sd/boosters.pvr`: PVR v3, pixelFormat `14`, 256x128
- `assets/graphics/hd_common/splash-hd.pvr`: PVR v3, `rgba8888`, 1024x512

No `libbadland.so`, a tabela custom de `CCTexturePVR` mapeia:

- key `12` -> GL internal format `0x9274` (`GL_COMPRESSED_RGB8_ETC2`)
- key `14` -> GL internal format `0x9278` (`GL_COMPRESSED_RGBA8_ETC2_EAC`)

Nao existem diretorios reais `assets/hd_etc2`, `assets/sd_etc2`, `assets/hd_pvrtc` ou `assets/sd_pvrtc` no APK; essas strings existem no binario, mas o pacote atual usa `assets/graphics/...`.

## Controles

`assets/controllers.json` declara `Gamepad`, `Remote` e `SecondScreen` com DPad/analog/digital buttons. O gameplay e essencialmente toque/hold, entao o mapeamento deve ser simples:

- A/B/X/Y/R -> touch hold ou `nativePadAction`
- D-pad/analog -> menus
- Select+Start -> matar processo no loop host, como Magic Rampage

## Referencias locais

- `ports/magicrampage`: loop SDL/GLES2 + Cocos JNI + APK/assets + Select+Start.
- `ports/dysmantle/src/imports.c` e `ports/dysmantle/src/etc2_decode.c`: hook `glCompressedTexImage2D` e decode ETC2/EAC para RGBA no Mali/GLES2.
- `ports/re4` e `ports/megamanx`: FMOD Android/AudioTrack/OpenSL bombeado para SDL, util se o FMOD do BADLAND nao abrir audio direto.

## Plano de implementacao

1. Criar loader arm64 do BADLAND baseado no Magic Rampage.
2. Carregar `libfmod.so`, `libfmodstudio.so`, depois `libbadland.so`.
3. Implementar/ligar shims Android: `liblog`, `libandroid`/AAssetManager, JNI minimo, `Cocos2dxHelper_nativeSetApkPath`.
4. Criar janela/contexto GLES2 via SDL e chamar `nativeInit(width,height)` seguido de `nativeRender`.
5. Interceptar `glCompressedTexImage2D` para ETC2 (`0x9274..0x9279`) com decoder do Dysmantle; decode forçado para RGBA8888 e teste SD primeiro.
6. Inicializar FMOD pelo caminho nativo; se falhar, reaproveitar bomba FMOD/AudioTrack/OpenSL de RE4/MegaManX.
7. Mapear gamepad por `nativePadAction` e fallback por touch events.
8. Adicionar launcher limpo com stop/mask de ES, logs persistentes e Select+Start sem restart.

## Estado em 2026-07-04

Port criado em `/storage/roms/ports/badland` e validado no device `192.168.31.79`.

- Boot/render GLES2 funcionando em 1280x720.
- PVR v3 funcionando com hooks nos slots de `CCTexturePVR`.
- ETC2/EAC (`0x9274`/`0x9278`) decodificado para RGBA8888 no hook GL para Mali-450/GLES2.
- Resolver de assets cobre `hd_etc2` -> SD/SD common e caminhos relativos como `dawn/layer-1.pvr`.
- Cenário HD restaurado para `dawn/day/dusk/night`: o loader serve os plists/PVRs HD de `background`, `layer-*`, `creatures` e equivalentes, reescrevendo `*-hd.pvr` para o nome esperado pelo Cocos.
- Atlases globais de gameplay em HD ativados para `obstacles`, `items`, `particles`, `eyes` e avatars, corrigindo escala de grama/musgo/cipós/pretos de cenário dentro do layout HD.
- `CCFileUtilsAndroid::getFileData` tem fallback direto para assets locais.
- Audio FMOD real validado no device.
- Controle por gamepad validado no menu e gameplay.
- `BADLAND.sh` usa logs persistentes em `logs/run-*.log`, heartbeat, watchdog e `systemctl stop + mask --runtime` para `emustation`/`es-de` durante o jogo.
- Select+Start sai pelo loop host; ultimo encerramento manual retornou `status=0`.

Validacao de gameplay em 2026-07-04: usuario confirmou que o cenario ficou bom com o loader HD atual. Manter como base boa:

- `BADLAND_HD_SCENERY=1` com `dawn/day/dusk/night` em HD.
- `BADLAND_HD_GLOBALS=1` com `obstacles`, `items`, `particles`, `eyes` e avatars em HD.
- `BADLAND_HD_UI=1` com menu/seletor em HD: `menus/level-pack-*`, `menus/level-packs`, `UI Assets/*` e fontes bitmap `font*.fnt`/`font*-hd.pvr`.
- Shader `lighten.fsh` corrigido com alpha explicito.

Validacao extra no device: usuario confirmou que a correcao arrumou tudo ate o menu. O log validado em `/storage/roms/ports/badland/logs/run-20260704-165343-23598.log` mostra `UI Assets`, `font-hd.pvr` e `menus/level-pack-0-*` servidos em HD.

Ponto aberto: se aparecer algum resto preto/quadrado em telas mais avancadas, tratar por asset especifico olhando o log de fallback/HD, sem desfazer a base HD validada.
