# DYSMANTLE (PortMaster) — CHANGELOG

## v1-test (2026-06-12) — primeiro pacote pra comunidade

**Render**
- 🏆 Mundo COMPLETO renderizando (chão, pedras, lixeiras, árvores, água). Causa-raiz
  histórica: shaders `*Shadows` (feature_level=2) eram pulados no target GL tier-1 →
  vertex format 0 → geometria do mundo nunca era criada. Fix: fallback automático de
  shader (degrada `Shadows/Reflections/Heights/Specular/Normals/Glow/Fur→Diffuse/Lit`
  até variante que carrega). 43 aliases em jogo, 0 erros de vertex buffer.
- Resolução dinâmica: segue o framebuffer do device (Mali-450 fbdev até KMSDRM 1080p).
- `DYSMANTLE_TEXSCALE` (default 1.3): texturas reduzidas por fator arbitrário
  (bilinear) = mais FPS/menos memória; configurável no launcher.

**Performance**
- Áudio sem engasgo: pump thread dedicada de 4ms pros callbacks OpenSLES (antes o
  ring esecava a cada frame = underrun constante) + buffer SDL 4096→1024 frames
  (latência 93ms→23ms). Underruns: zero.
- JNI de bateria implementado (CallFloatMethod + status): a engine via "bateria 0%
  descarregando" e ativava power-save com cap de 30fps. Agora 100%/carregando.
- vsync off por default (`DYSMANTLE_SWAPINT=0`): o limiter da engine cuida do pacing
  (vsync por cima = double-pacing = trava em 30fps).
- Medidores embutidos no log (a cada 5s): `[PERF]` frame-time, `[PERFAUD]` áudio,
  `[PERFCPU]` threads — colem o log.txt nos reports!

**Controles**
- Padrão PortMaster: gptokeyb + `dysmantle.gptk` (botões via teclado uinput), sticks
  e gatilhos ANALÓGICOS direto do pad. D-pad = quick slots. SELECT+START sai.
- Sem gptokeyb no device → controle nativo direto no binário (fallback).

**Base (sessões anteriores)**
- so-loader 2 módulos (libc++_shared + libNativeGame) com pad TLS anti-canary bionic.
- Oboe real via shim OpenSLES→SDL2; Paddleboat alimentado direto do C.
- GameActivity (AGDK) emulado; saves Android compatíveis (gamedata/).

**Limitações conhecidas**
- Dynamic Shadows: manter OFF (crash no load em GPU Utgard; sem efeito visual após o
  fallback de shaders).
- Shadow maps dinâmicos não renderizam (materiais usam a variante sem sombra).
- glibc ≥ 2.38 (sem runtime bundlado nesta versão de teste).
