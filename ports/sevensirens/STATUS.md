# Shantae and the Seven Sirens — so-loader — STATUS

## 🔴 VEREDITO: MURO ARQUITETURAL no Mali-450 (engine VULKAN-ONLY)
Recon 2026-06-24. A engine WayForward "wf" do Seven Sirens **renderiza exclusivamente
via Vulkan**. O Mali-450 (Utgard) só tem **OpenGL ES 2.0** e **nenhum** suporte a Vulkan
(Vulkan exige Mali Midgard+). Sem caminho GLES = muro arquitetural (régua do NextOS:
"engine sem caminho GLES2 = o único bloqueio real").

### Provas (libShantaeSiren.so + assets do APK)
- Família `wfRenderDevice` 100% Vulkan: `wfVulkanUtils::VulkanStartup`, `wfVulkanHeap`,
  `wfVulkanDescriptorSetBuilder`, `wfVulkanDynamicConstantBuffer`, `wfVulkanVertexUtils`,
  `vkCreateBuffer`/`vkGetBufferMemoryRequirements`/etc. O engine `dlopen("libvulkan.so")`.
- **Nenhum** backend GL: `wfGL*`/`wfGles*`/`eglCreate*`/`glDraw*` = **zero** símbolos.
- Shaders = **só SPIR-V**: único pak é `assets/_wfshadersvk.pak` (`.svkd` = Vulkan), com
  `shaderupdateVK.mtime`. NÃO há `_wfshaders.pak`/`_wfshadersgl` (nenhum shader GL embarcado)
  → mesmo que houvesse um backend GL, não há shader p/ alimentá-lo.
- Não há fallback GL no código (sem string de "opengl fallback"/"vulkan required→gl").

### Conclusão
Roda só em device com **Vulkan**. No Mali-450 é impossível renderizar 1 frame — não é
tuning, é a GPU não ter o driver. **Alvo possível = R36S (RK3326/Mali-G31 Bifrost), que JÁ
TEMOS e tem Vulkan 1.x** (Bully/SOTN rodaram nele). ⚠️ R36S = 1GB RAM vs 2.6GB de assets
(apertado, testar). NUNCA X5M (banido).

## ✅ O QUE JÁ FOI FEITO (aproveitável num device Vulkan)
- Recon completa: ELF64 aarch64, NativeActivity (`android_main` @0x90387c + `JNI_OnLoad`),
  pacote `com.crunchyroll.gv.shantae7sirens`, **sem pairip** (só `integrity.properties`=Play
  Integrity → stub JNI). Deps: libc++_shared + libfmod + libfmodstudio + libplaycore (stub)
  + libandroid/log/z. Áudio FMOD (recipe RE4/DuckTales: `org.fmod.FMODAudioDevice` fake→pull).
  Assets 2.6GB (954 arq, .pak+.txt). Engine usa Bullet (btSingleSweepCallback) + Spine 2D.
- **so-loader de 4 módulos COMPILA** (scaffold Dysmantle arm64): `build.sh` carrega
  libc++_shared → libfmod → libfmodstudio → libShantaeSiren com snapshots encadeados
  (main.c). Binário `sevensirens` ELF64 aarch64 PIE OK. so_util ELF64, canary bionic
  tpidr+0x28, eh_frame @0x5a7970, sem softfp (arm64 ABI unificada). NÃO testado em device
  (sem sentido sem GPU Vulkan).
- imports.c = base Dysmantle com hooks de textura/VB INATIVOS (g_vb_*=0); `bake_stubs.c`
  stuba `bk_last_bmp_name`. Renomeado `sevensirens_overrides`.

## ⏭️ SE FOR PORTAR (em device Vulkan)
1. Renderer Vulkan real (precisa libvulkan no device + WSI p/ a janela SDL/fbdev).
2. FMOD bridge (org.fmod.FMODAudioDevice fake → SDL/pulse).
3. Extrair assets 2.6GB (BYO-data: assets/ do APK → dir servido pelo AAssetManager shim).
4. Input WayForward (wfAndroidDeviceGlue::HasJoystick + glue JNI).
5. libplaycore stub (Play asset-delivery), Play Integrity stub.

ARQUIVOS: ports/sevensirens/ (src/, build.sh). APK: ~/Downloads/Telegram Desktop/
Shantae and the Seven Sirens.apk (2.8GB). .so em lib/, device_libs/.
