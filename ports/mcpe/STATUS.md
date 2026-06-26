# 🟢🏆 Minecraft Bedrock (MCPE) → Mali-450 — JOGÁVEL (s1 2026-06-26)

**Marco:** Minecraft **Bedrock 1.16.201** RODANDO E JOGÁVEL no **Mali-450 Amlogic** (NextOS, device .79):
menu "Create New World / Game Settings" renderiza limpo, **mundo criado e jogado** (NextOS confirmou).
Inédito — Bedrock no Utgard/fbdev.

## 🔑 A SACADA (rota totalmente diferente do so-loader)
Este NÃO é um so-loader nosso. É o **mcpelauncher-manifest** (projeto `minecraft-linux`, prebuilt
ARM32 glibc/armhf) que JÁ emula o runtime Android e carrega o `libminecraftpe.so` (armeabi-v7a) do
APK. O port veio do "McpeLauncher-Port" (ImpressiveStay) feito pra **RK3326/R36S com Mesa/Panfrost+
kmsdrm** — stack que o Mali-450 Amlogic NÃO tem (sem /dev/dri, só fbdev + blob Mali Utgard).

**O que destravou tudo (1 variável):** o `mcpelauncher-client` empacota SDL3 (3.1.6) estático, mas
respeita **`SDL3_DYNAMIC_API`**. Apontamos pro **nosso SDL3 mali-fbdev do sistema** (`/usr/lib32/
libSDL3.so.0`, fork 3.5.0, driver `src/video/mali-fbdev/`) → o client cria o contexto GLES2 no blob
Mali via fbdev. Log confirma: `[GL] Vendor: ARM / Renderer: Mali-450 MP / Version: OpenGL ES 2.0`.
SDL3 dynamic API é forward-compat (3.5.0 sobrepõe 3.1.6 sem dor — é o design dele).

## Receita (device .79, NextOS Amlogic Mali-450, glibc 2.43 multilib)
Tudo 32-bit (o client e o `libminecraftpe.so` armeabi-v7a vivem no MESMO processo → stack inteiro armhf):
- **SDL3:** `SDL3_DYNAMIC_API=/usr/lib32/libSDL3.so.0` (nosso mali-fbdev). NÃO forçar SDL_VIDEODRIVER
  (regra: vídeo vem automático do mali-fbdev). Remover TODO o lixo Mesa/kmsdrm/dri/gbm/wayland do
  script original do R36S.
- **GL:** passar **`--force-opengles`** ao client (Mali-450 é GLES2-only; sem isso ele tenta o
  glcorepatch/desktop-GL que o Utgard não faz).
- **libs (LD_LIBRARY_PATH):** `versions/<ver>/lib/armeabi-v7a` (libminecraftpe/libfmod do APK) :
  `mcpe_launcher/lib/armeabi-v7a` (libc++_shared bundled — o 1.16.201 não traz) : **`/usr/lib32`**
  (blob Mali libEGL/libGLESv2 + OpenSSL3 + SDL3 + libstdc++/libm/etc, todos glibc 2.43).
  ⚠️ **NÃO usar as `lib/armhf-system/` bundled do port** (são glibc ANTIGO do DarkOS → `version
  GLIBC_2.43 not found` quando o libasound do sistema as puxa). O device já tem tudo em /usr/lib32.
- **ES:** `systemctl stop emustation` ANTES de lançar (senão briga pelo fb0 e o mali-fbdev cai no
  fallback 640x480). Reiniciar o ES ao sair.
- **launch:** `nohup bash mcrun.sh` (regra #8 — nunca setsid no Amlogic velho). Matar+confirmar 0
  instâncias antes (regra #3).
- **runner pronto:** `mcrun.sh` (neste dir). Roda `mcpelauncher-client --game-dir versions/<ver>
  --force-opengles`.

## Setup (1ª vez)
1. APK **Minecraft Bedrock armeabi-v7a (ARM32), versão 1.2–~1.19** — recomendado **1.16.201** (sem
   pairip, GLES2-friendly). APK arm64/pairip (ex.: 1.26) NÃO serve (o SetupMcpe rejeita sem
   armeabi-v7a; client é 32-bit). NÃO versionar o APK aqui (legal + tamanho).
2. Pôr o APK em `mcpe_launcher/` e rodar `SetupMcpe.sh` → extrai `lib/armeabi-v7a/*.so` + `assets/`
   pra `versions/<nome-do-apk>/`. Depois remover o APK.

## 🔴 Pendências (s1)
- **RESOLUÇÃO:** renderiza ~640x360 no canto sup-esq, não preenche 1280x720 (mesmo com ES parado).
  O mali-fbdev/SDL3 dá um surface menor que o painel 720p. PRÓX: ver como o `SDL_malivideo.c` escolhe
  o tamanho (lê /sys/class/display/mode=720p60hz e fb0 virtual_size=1280x1440) e forçar 1280x720.
- **Áudio:** FMOD host não carregou (`libfmod.so.10.20` não achado) → cai pro backend sdl3/opensl
  stub. Validar som (provável mudo). `mcpelauncher-client-settings.txt: audio_backend=sdl3`.
- **Xbox Live:** falha de SSL CA cert (esperado, offline). README roda em `unshare --net`; nós
  rodamos direto — validar se precisa isolar rede.
- **Controle:** "Gamepad connected #2" detectado; mapear via gptokeyb (.gptk) + validar in-game.
- **Empacotar** launcher ES (menu LÖVE — device TEM /usr/bin/love) + matar/voltar ES no .sh.

## Refs
- Upstream: github.com/minecraft-linux/mcpelauncher-manifest (client prebuilt aqui é 3rd-party).
- Zip original do port: `~/Downloads/McpeLauncher-Port.zip` (era pra RK3326/R36S/DarkOS).
- Nosso SDL3 mali-fbdev: `/home/nextos/sdl3-debug/SDL3-mali-fbdev` (fork 3.5.0, no sistema em
  /usr/lib32/libSDL3.so.0).
