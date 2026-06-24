# crazytaxi-src — referência (port da comunidade)

Loader so-loader do **Crazy Taxi Classic** (com.sega.CrazyTaxi, Android aarch64).

- **Loader original:** [initdream/crazytaxi-aarch64](https://github.com/initdream/crazytaxi-aarch64),
  construído **em cima deste framework** ([nextos_ports_android](https://github.com/felc18-blip/nextos_ports_android)).
- **Adaptação Mali-450 (Utgard, fbdev):** recompilação no toolchain NextOS
  Amlogic-old, mapeamento teclado→keycode Android para **gptokeyb**, e ajustes
  de áudio (PulseAudio). ✅ jogável (GLES2, 1280x720, 30fps).

Diferente de `syberia-src`/`lswtcs-src` (que são a **base** do framework, do
mtojek), este é um exemplo **downstream**: alguém usou o framework para portar
um jogo, e nós adaptamos esse loader para o Mali-450.

> **Só o código/loader.** Nenhum dado de jogo (copyright Sega) vai pro repo —
> os assets são BYO (you bring your own). O port completo e buildável fica em
> [`ports/crazytaxi/`](../../../ports/crazytaxi/).
