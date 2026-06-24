# DuckTales: Remastered — ESTUDO / PLANO DE PORT (pré-go)

> Recon 2026-06-23. Engine WayForward nativa **ES2** + NativeActivity → perfil amigável p/ Mali-450.
> COMEÇAR só no "go" do porter. Base de dev = **Mali-450 .164** (`<device-ip>`, senha `<senha>`,
> EmuELEC, fbdev Utgard, fb 1280x720). Régua: Crazy Taxi (NativeActivity-ish so-loader + fmod),
> RE4 (bridge fmod→AudioTrack), SOTN/Mina (ES2 puro).

---
## APK
- **`/home/root/Downloads/DuckTales_Remastered_Android.apk`** (485 MB compactado, ~600 MB descompactado).
- Dev: **WayForward** (port do XBLA/PSN de 2013). 2.5D (sprites 2D em cena 3D Bullet).
- ⚠️ confirmar `package` + nome da Activity no `AndroidManifest.xml` (AXML binário — usar `aapt`/`apkanalyzer` OU parser AXML; aapt não estava no host no recon).

## Recon (FATOS confirmados)
- **`lib/armeabi-v7a/` SÓ** — **armv7 32-bit** (ARM EABI5). NÃO tem arm64. → toolchain **armhf** (a do RE4/Terraria-armv7, NÃO a aarch64 do Crazy Taxi/SOTN).
- **3 libs nativas:**
  - `libducktales.so` (11.5 MB) — o jogo.
  - `libfmodex.so` + `libfmodevent.so` — áudio **FMOD**.
- **ES2 NATIVO ✅** — `gl_FragColor`, `glCreateShader`/`glDrawElements`/`glVertexAttribPointer`, linka `libGLESv2.so`. **ZERO símbolo ES3-only** (sem glMapBufferRange / glGenVertexArrays / glDispatchCompute / glTexStorage2D). → roda no **Mali-450 Utgard ES2 sem conversão de shader**. (Filtro #1 do backlog PASSA limpo — é o caminho mais fácil, tipo SOTN/Crazy Taxi.)
- **Entrypoint = NativeActivity** (engine nativa, NÃO Unity/so-loader-de-Java):
  - exporta `android_main` (native_app_glue), `ANativeActivity_onCreate`, `JNI_OnLoad`.
  - ⟹ o loader é estilo **native_app_glue**: a gente fornece a struct `ANativeActivity` + `ALooper`/`AInputQueue`/`ANativeWindow` e chama `ANativeActivity_onCreate` (ou direto `android_main`). **Mais parecido com Crazy Taxi do que com Unity.**
- **Middleware no binário:** **Scaleform GFx** (UI/menus — string "Scaleform Eval"), **Bullet physics** (`HullLibrary`/`btHullTriangle`), **Lua** (`LuaBindings`/`lua_State`). Tudo estático dentro de libducktales.so.
- **DADOS = `assets/*.pak` (~600 MB) DENTRO do APK** — NÃO precisa OBB separado (apesar do código ter `getOBBPath`/"Open OBB file"; o jogo lê dos `.pak` em assets/). Principais: `music.fsb` (75 MB, sound bank fmod), `global.pak`, `*_env.pak`/`*.pak` por nível (mines/transylvania/...), `gallery*.pak`, `font_*.pak` (CJK).
- ⚠️ **License gate:** string `"Failed to open the license file. Please see the readme... Error: 101"` — há checagem de licença. Pode travar boot → preparar stub (retornar "licenciado") como no anti-pirataria do SOR4/pixelcup.

## Régua de viabilidade — 🟢 VERDE
- ES2 nativo + NativeActivity + engine 2.5D = perfil **Crazy Taxi** (NativeActivity-ish, fmod, ES2, rodou liso no Mali-450 fbdev). Risco BAIXO-MÉDIO. Pontos de atenção: (1) glue do NativeActivity, (2) fmod→áudio do device, (3) loader de `.pak`/AAssetManager, (4) license stub, (5) Scaleform (só rendering ES2 — sem segredo). Sem DRM pesado (sem pairip), sem Vulkan, sem UE4.

---
## PLANO (so-loader native_app_glue, reusar scaffold)
**Base recomendada:** `ports/crazytaxi` (NativeActivity + fmod + ES2 fbdev) e `ports/re4` (bridge fmod→AudioTrack, shims bionic). Toolchain **armhf** (mesma do RE4/Terraria-armv7).

1. **Extrair APK** → `ports/ducktales/payload/`:
   - `lib/armeabi-v7a/*.so` (3 libs).
   - `assets/` INTEIRO (os `.pak` + `music.fsb` + fonts) → o jogo lê via AAssetManager (shim → disco).
   - Manifest: extrair package + Activity name (aapt/apkanalyzer).

2. **so-loader / native_app_glue (copiar base do Crazy Taxi):**
   - Carregar `libducktales.so` + resolver imports vs host (libc/libGLESv2/EGL/libm/liblog/android).
   - **Glue NativeActivity:** montar `struct ANativeActivity` (callbacks, `vm`/`env` fake JNI, `internalDataPath`/`externalDataPath`/`obbPath` apontando p/ `/storage/roms/ports/ducktales`), `ALooper`, `AInputQueue`, `AConfiguration`. Chamar `ANativeActivity_onCreate(activity, savedState, size)` OU `android_main(android_app*)` — ver qual o engine usa (provável NativeActivity → onCreate cria a thread do android_main).
   - **AAssetManager → disco** (`AAssetManager_open`/`AAsset_read`/`AAsset_getLength`/`seek` lendo de `payload/assets/`), case-insensitive (lição SOTN).
   - **EGL → Mali fbdev** (libEGL direto, 1280x720) OU egl_shim→SDL2 do device (universal, igual SOTN — recomendado p/ portabilidade R36S depois).
   - **Shims provados (SOTN/RE4/Crazy Taxi):** canary bionic (tpidr), R_ARM ABS imports UNDEF, `__sF`/stdio, `pthread`/`sem` bridge bionic→glibc, `__android_log`→stderr, `__system_property_get` (sdk "25" p/ fmod), `AAsset_*`.
   - ⚠️ **armv7**: atenção a alinhamento/ABI 32-bit (struct sizes, varargs) — o RE4 (armv7) já resolveu vários; reusar `bionic_shims`/`pthread_fake`/`sem_shim` dele (versão armv7).

3. **ÁUDIO FMOD** (igual RE4/Dysmantle): o fmodex usa OpenSL OU Java AudioTrack via `org.fmod.FMODAudioDevice`. Rotear p/ **pulse** (pacat) com callback pull, OU SDL audio. (Confirmar no `libfmodex.so`: `strings | grep -iE 'AudioTrack|OpenSL|org/fmod'`.) `music.fsb` = sound bank fmod.

4. **License stub:** interceptar a checagem de licença (achar a função que abre o "license file"/Error 101) → retornar OK. Provável JNI ou leitura de arquivo; servir um arquivo válido OU patchar a função (estilo SOR4 `getInstallerPackageName`).

5. **Input:** evdev → eventos do NativeActivity (`AInputQueue` injetando `AInputEvent` de key/motion) OU gptokeyb fallback. Mapear Xbox (pad do device). DuckTales = plataforma 2.5D (dpad + pulo/bengala) → mapeamento simples.

6. **Iterar no .164** (regra: matar+confirmar 0 instâncias ANTES de lançar; foreground `bash` puro; `dd /dev/fb0` 1280x720x4 BGRA → PNG p/ ver render). `CUP_FRAMES`-style cap p/ runs curtos se precisar.

7. **Fullscreen res nativa automática** (sem hardcode 720p — lição SOTN/RE4), áudio, controle, empacotar tar.gz (Desktop + R2) com créditos felc18-blip/NextOS, launcher `.sh` em `ports_scripts/`.

---
## RISCOS / DESCONHECIDOS (resolver na ordem)
1. **NativeActivity glue completo** — é o maior item (ALooper/AInputQueue/ANativeWindow/AConfiguration). Crazy Taxi tem parte; pode precisar implementar mais callbacks. **MAIOR risco de tempo.**
2. **fmod backend** — confirmar OpenSL vs AudioTrack e rotear (receita RE4/Dysmantle pronta).
3. **`.pak`/`music.fsb` loading** — só precisa AAssetManager servir os bytes; o parser é interno. Verificar paths case-sensitive.
4. **License Error 101** — pode ou não travar; stub se travar.
5. **Scaleform** — só rendering ES2 (sem UBO/ES3); não deve ser muro. Verificar se gera shaders runtime ES2.
6. **armv7 ABI** — reusar shims do RE4 (mesmo armhf).

## REFS DE CÓDIGO (reusar)
- `ports/crazytaxi` — **NativeActivity + fmod + ES2 fbdev** (base #1; aarch64, mas o fluxo NativeActivity serve).
- `ports/re4` — **armv7** + bridge fmod→AudioTrack + shims bionic 32-bit (base #2, ABI armv7).
- `ports/sotn` — egl_shim universal Mali-450+R36S, canary, ABS imports, assets case-insensitive.
- `ports/terraria` — toolchain armhf + so_util (layout PT_LOAD).
- Toolchain armhf: a mesma usada no RE4 (`build.sh` dele aponta o CC armhf).

## REGRAS HERDADAS (memória — OBRIGATÓRIO)
- **Git:** SÓ master, commits limpos (zero co-autor). (notas internas do projeto)
- **Matar+CONFIRMAR 0 instâncias** antes de lançar (2 jogos juntos travam o device). Kill por `/proc/*/exe`. [[feedback_matar_confirmar_jogo_antes_de_lancar]]
- **Não parar / não explicar até IMAGEM na tela** em loop de port. [[feedback_nao_parar_nao_explicar_ate_imagem]]
- Limpar/apagar com cuidado (alvos explícitos, staging primeiro). [[feedback_limpar_apagar_arquivos_com_cuidado]]
- Display 100% automático no launcher; abrir por SSH em foreground (`systemctl stop emustation; bash X.sh`). [[reference_nextos_abrir_jogo_por_ssh_foreground]]
- ⚠️ Limpar `/tmp` do device entre runs (tmpfs 416MB enche → ENOSPC; lição do pixelcup).

## PRÓXIMOS PASSOS (quando der o "go")
1. `aapt`/apkanalyzer no manifest → package + Activity (1ª coisa).
2. Extrair payload (libs + assets) p/ `ports/ducktales/payload/`.
3. `cp -r` scaffold do Crazy Taxi + shims armv7 do RE4 → `ports/ducktales/src/`, ajustar paths/package.
4. Montar o glue NativeActivity + carregar libducktales.so → primeiro objetivo = **chegar no android_main sem crash** (logar até o 1º `eglSwapBuffers`).
5. Loop build→run→fix até **IMAGEM** (logo WayForward / menu Scaleform), depois áudio, depois controle.
