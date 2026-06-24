# Pixel Cup Soccer — ESTUDO / PLANO DE PORT (pré-go)

> Recon 2026-06-22. Vitória mais rápida do backlog (ES2 nativo, IL2CPP dominado via Terraria, SEM DRM).
> COMEÇAR só no "go" do porter. Base de desenvolvimento = **Mali-450 device** (= `<device-ip>` wifi, senha `<senha>`).

## APK
- **`/home/root/Downloads/Telegram Desktop/Pixel Cup Soccer 1.b320.apk`** (589M)
- Dev: **Batovi Games** (Pixel Cup Soccer). Package: confirmar no manifest (aapt não instalado no recon).

## Recon (fatos confirmados)
- **Unity 2022.3.62f3, IL2CPP** (global-metadata.dat presente).
- **arm64-v8a ONLY** — nossa toolchain aarch64 (Amlogic-old) cobre. Sem armv7.
- Libs: `lib/arm64-v8a/libil2cpp.so` (47MB, código do jogo AOT) + `libunity.so` (17MB, engine) + `libmain.so` (6KB, entry Android).
- **ES2 NATIVO**: libunity tem `force-gles20` + "OpenGL ES 2.0 supported"; backlog anotou 8×`#version 100`. → roda no Mali-450 Utgard ES2 sem conversão de shader.
- **SEM pairip / SEM DRM / SEM playcore** — APK limpo (mais fácil que Terraria, que tinha pairip).
- Dados: `assets/bin/Data/` (data.unity3d 25MB único + Managed/Metadata/global-metadata.dat + Resources).

## Régua de viabilidade
- = perfil **Terraria** (Unity IL2CPP ES2) e **RE4** (Unity Android so-loader), AMBOS jogáveis no Mali-450. IL2CPP NÃO é problema (so-loader carrega o .so direto). Aqui é AINDA mais fácil: sem pairip.
- Risco: BAIXO. Esperado = port mais rápido do backlog.

## Plano (so-loader, reusar scaffold Terraria/RE4)
1. **Extrair APK** → `ports/pixelcup/`: `lib/arm64-v8a/*.so` + `assets/` inteiro (chdir + Unity lê de disco via AAssetManager shim).
2. **so-loader** (copiar base de `ports/re4` ou `ports/terraria` — Unity Android flow):
   - Carregar `libunity.so` + `libil2cpp.so`; resolver imports vs host libc/GLESv2/EGL/libm.
   - Fake JNI/JavaVM + UnityPlayer Java class falsa; entry via libmain.so `JNI_OnLoad`/`UnityPlayer` nativo → init player → surface → render loop.
   - Shims provados (SOTN/RE4/Bully): **canary bionic** (`_Thread_local pad[256]`, tpidr+0x28), **R_AARCH64_ABS64** p/ imports UNDEF, **stdio __sF**, **AAssetManager → disco** (AAsset_* lê de `assets/`), **EGL → Mali fbdev** (libEGL direto OU egl_shim→SDL2 do device p/ universal igual SOTN).
   - Áudio: Unity usa FMOD ou OpenSL/AudioTrack → rotear p/ pacat (igual RE4/SOTN) OU SDL.
   - Input: evdev → Unity (onNativePad/eventos), gptokeyb fallback.
3. **Iterar no device** (matar+confirmar antes de lançar; foreground bash; dd /dev/fb0 p/ ver render).
4. Fullscreen res nativa automática (sem 720p hardcode — lição SOTN), áudio, controle (mapeamento Xbox), empacotar tar.gz (Desktop + R2) com créditos felc18-blip/NextOS.

## Regras herdadas (memória)
- Git: master only, ZERO co-autor. [[feedback_no_coauthor_commit]] [[feedback_git_workflow_master_only]]
- Matar+confirmar 0 instâncias antes de lançar. [[feedback_matar_confirmar_jogo_antes_de_lancar]]
- Limpar/apagar com cuidado (alvos explícitos, staging primeiro). [[feedback_limpar_apagar_arquivos_com_cuidado]]
- Display 100% automático no launcher (SDL2 do device escolhe backend). [[castlevania-sotn-mali450]]

## Refs de código
- `ports/re4` (Unity 2018 Mono so-loader — JNI/Activity/audio/input shims).
- `ports/terraria` (Unity IL2CPP+pairip ES2 scaffold — IL2CPP load + metadata).
- `ports/sotn` (egl_shim universal Mali-450+R36S; canary; ABS64; assets case-insensitive).
