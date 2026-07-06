# Goat Simulator Android arm64-v8a - estudo de viabilidade

APK estudado:

`/home/felipe/Downloads/com.coffeestainstudios.goatsimulator.free_2.19.10-94423163_2arch_7dpi_24lang_2feat_50b9e9359233ebf65df036bf285708a9_apkmirror.com.apkm`

Foco pedido: `arm64_v8a`.

## Achados objetivos

- App: Goat Simulator, pacote `com.coffeestainstudios.goatsimulator.free`, versao `2.19.10`, versionCode `94423163`.
- APKM contem splits `arm64-v8a` e `armeabi-v7a`; o alvo correto para NextOS moderno e `arm64-v8a`.
- Engine: Unreal Engine 4, nao Unity.
- Binario principal: `split_config.arm64_v8a.apk!/lib/arm64-v8a/libUE4.so`, AArch64, ~179 MB.
- Dependencias nativas diretas:
  - `libGLESv2.so`
  - `libEGL.so`
  - `libandroid.so`
  - `libOpenSLES.so`
  - `libc++_shared.so`
  - libc/libdl/liblog/libm/libz
- Entrada/Android:
  - tem `ANativeActivity_onCreate`
  - tem `android_main`
  - tem muitos JNI `Java_com_epicgames_ue4_GameActivity_*`
  - usa `AAssetManager_*`, `AInputQueue_*`, `ANativeWindow_*`
- Controle:
  - tem `FAndroidInputInterface`
  - tem joystick axis/button
  - tem `AndroidThunkCpp_IsGamepadAttached`
  - controle nao parece ser o bloqueio principal.
- Audio:
  - usa `libOpenSLES.so` e simbolos de audio Android/UE.
- Renderer:
  - linka GLES/EGL diretamente.
  - importa `glCompressedTexImage2D` e `glCompressedTexSubImage2D`.
  - contem codigo Vulkan/RHI, mas tambem caminho OpenGL ES.
  - strings confirmam `bBuildForES31`, `bMultiTargetFormat_ASTC`, `bMultiTargetFormat_ETC2`, `TextureFormatPriority_ASTC`, `TextureFormatPriority_ETC2`.
- Dados:
  - `assets/UE4CommandLine.txt`: `../../../Goatsim_UE4/Goatsim_UE4.uproject`
  - pacote de dados principal esta em `split_common.config.astc.apk`.
  - PAKs encontrados:
    - `pakchunk0-Android_ASTC.pak` 333 MB
    - `pakchunk0_s1-Android_ASTC.pak` 282 MB
    - `pakchunk0_s2-Android_ASTC.pak` 218 MB
    - `pakchunk0_s3-Android_ASTC.pak` 72 MB
  - PAKs usam assinatura UE4 e compressao `Zlib`.
  - indice/nomes aparecem via strings, entao nao parece totalmente opaco; ha caminhos `.uasset` visiveis.
- Java/servicos:
  - base APK tem `com.epicgames.ue4.GameActivity`.
  - contem Billing, Firebase, Crashlytics, ads/AppLovin/AdMob/IronSource/Vungle/Unity Ads e Play Asset Packs.

## Comparacao com ports 100% funcionais

- Terraria/Rockman ajudam em padroes de loader Android, EGL/GLES, audio e JNI, mas nao sao referencia direta de engine porque Goat e UE4.
- Salt/TMNT/Panzer/Dead Cells mostram uma regra confirmada para Mali-450: ASTC precisa ser convertido, decodificado ou evitado.
- Dead Cells documenta o device como `OpenGL ES 2.0, Mali-450 MP, GLSL ES 1.00`; Utgard nao suporta ASTC.
- Receitas do arquivo indicam que Mali-450 tambem nao deve ser tratado como ETC2 confiavel; caminho seguro e ETC1 para opaco ou RGBA8/downscale quando necessario.

## Viabilidade

Para um device arm64 com GPU moderna, ES3/Vulkan e ASTC: viabilidade media/boa, porque o APK ja traz `arm64-v8a`, `libUE4.so`, paks locais e caminho Android padrao da UE4.

Para nosso alvo Mali-450/NextOS antigo: viabilidade baixa a media-baixa no estado atual. O maior bloqueio e que este APKM so traz dados `Android_ASTC`. O Mali-450 nao suporta ASTC, e a UE4 vai tentar subir texturas comprimidas via `glCompressedTexImage2D`.

Sem resolver ASTC/ES3.1, o resultado esperado e tela preta, texturas brancas/cinzas/magenta, crash no renderer ou shaders invalidos.

## Caminhos possiveis

1. Procurar uma variante do APK/APKM com dados `ETC1` ou `ETC2` em vez de `ASTC`.
   - Melhor caminho se existir.
   - Reduz muito o risco.

2. Extrair/reempacotar os `.pak` e converter texturas ASTC para RGBA8 ou ETC1.
   - Possivel em tese, mas pesado.
   - Precisa ferramenta compativel com a versao do PAK/UE4 e cuidado com referencias de asset.

3. Interceptar `glCompressedTexImage2D` e decodificar ASTC em runtime/cache.
   - Funciona para engines menores nos nossos ports, mas em UE4 e com muitos assets grandes pode estourar RAM/CPU.
   - Ainda restaria o problema de shaders/ES3.1.

4. Forcar OpenGL ES e bloquear Vulkan.
   - Necessario, mas nao suficiente.
   - O binario contem caminho GLES, porem strings indicam build ES3.1.

## Veredito

Nao e impossivel, mas nao e um port simples. O `arm64_v8a` ajuda muito no loader, som e controles. O que derruba a chance no Mali-450 e o pacote ser `Android_ASTC`/UE4 ES3.1.

Antes de iniciar port completo, o proximo passo recomendado e procurar uma build/split nao-ASTC. Se so existir ASTC, o projeto vira pesquisa pesada de UE4 pak + conversao de textura/shader.
