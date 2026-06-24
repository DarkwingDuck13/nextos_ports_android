# 🛠️ Facilitando o Trabalho: O Ecossistema de Ports NextOS

Centro de inteligência do **nextos_ports_android**. O objetivo é transformar o porting de arte mística em engenharia documentada e replicável: cada fix que destrava um jogo vira **receita reusável** aqui.

O conhecimento foi acumulado portando jogos reais para a Mali-450 (Utgard) e GPUs vizinhas — de NativeActivity puro a Unity IL2CPP, Cocos2d-x, MonoGame e GameMaker. A matriz no fim mapeia cada port à lição que ele ensinou.

---

## 📖 O Manifesto do Portador (Mali-450 / Kernel 3.14)

Portar para o Amlogic-old não é só "fazer rodar"; é negociar com o hardware. Os pilares:

1.  **Direct-to-Hardware:** Não emulamos. Carregamos o `.so` nativo do Android e o fazemos acreditar que ainda está em casa.
2.  **A Ponte de Vidro (Bionic → Glibc):** O Android fala Bionic; o NextOS fala Glibc. Construímos stubs e bridges (especialmente `pthread`) pra essa conversa não quebrar.
3.  **Alquimia de Shaders:** A Mali-450 é Utgard (GLES 2.0 estrito). Convertemos shaders modernos em algo que ela desenhe sem engasgar.
4.  **Guardião da VRAM/RAM:** Pouca memória. Cortamos no upload de textura, gerenciamos zram, e fazemos o teardown certo na saída.
5.  **Tudo no automático:** Vídeo e áudio vêm do sistema. **Nunca** hardcodamos `SDL_VIDEODRIVER`/`SDL_AUDIODRIVER`.

---

## 🗺️ Guia de Navegação

### Receitas
1.  [Iniciando um Port do Zero](receitas/01-iniciando-um-port.md) — fluxo do `new-port.sh`, ler o `imports.gen.c`, o que atacar primeiro.
2.  [A Ponte Pthread e ABI](receitas/02-pthread-e-abi.md) — por que o mutex do Android crasha o Linux e como a `pthread_bridge` resolve.
3.  [Domando a Mali-450 (GLES 2.0)](receitas/03-domando-a-mali450.md) — `glClear`, `LUMINANCE`, `highp→mediump`, limite de instruções, render-to-texture.
4.  [Fake JNI e Android Shim](receitas/04-fake-jni-shim.md) — enganar o jogo (package name, paths, assets, system properties).
5.  [Áudio: OpenSLES/FMOD → SDL → PulseAudio](receitas/05-audio-opensles-sdl.md) — finja a API, puxe o PCM por callback, dê folga no refill.
6.  [Controle: gamepad, gptokeyb e ABI](receitas/06-controle-input-gptokeyb.md) — teclado→keycode, callback nativo, e nunca clonar o pad físico.
7.  [Memória, VRAM e Teardown](receitas/07-memoria-vram-e-teardown.md) — cap de textura, zram multi-stream, mate por /proc/*/exe, teardown EGL.
8.  [Texturas ETC1/ETC2](receitas/08-texturas-etc1-etc2.md) — formato por GPU, bake offline, o bug do path stale no FBO.
9.  [Display e o SDL estático](receitas/09-display-e-sdl-driver.md) — vídeo automático e a armadilha do SDL embutido do jogo.
10. [Empacotando no PortMaster (BYO-data)](receitas/10-empacotando-portmaster.md) — layout ports_scripts, foreground bash puro, inglês, mate+confirme.
11. [Ponteiros: handles, GOT/PLT e trampolins](receitas/11-ponteiros-handles-e-hooks.md) — o fio condutor do so-loader: handle-as-pointer, hook por GOT, pool de trampolins.

### Troubleshooting
1.  [Diagnóstico de Crash](troubleshooting/01-diagnostico-de-crash.md) — crash handler, stack scan, gdb, BRK traps.
2.  [Deadlock do Job-System](troubleshooting/02-deadlock-job-system.md) — congelou ao carregar? Semáforo/pthread da engine.

### Kit Essencial
O [`kit_essencial/`](kit_essencial/) tem o `core/` (so_util, egl_shim, opensles_shim, pthread_bridge) e o `templates/main_universal.c` — ponto de partida copiável pra um port novo.

---

## 🧩 Matriz de Ports — qual jogo ensinou qual lição

| Port | Engine | Lição reusável | Receita |
|---|---|---|---|
| **Castlevania SOTN** | SDL2 estático ES2 | SDL estático vaza o env do driver; relocações `ABS64` UNDEF, canário bionic no TLS (`tpidr+0x28`), `__sF` da stdio, `dlopen` self-ref | [09](receitas/09-display-e-sdl-driver.md) · [11](receitas/11-ponteiros-handles-e-hooks.md) |
| **Bully: AE** | RenderWare (libGame) | hook ARM64 com pool de trampolins; fix do `glClear` no FBO; cap/meia-res de textura | [11](receitas/11-ponteiros-handles-e-hooks.md) · [03](receitas/03-domando-a-mali450.md) · [07](receitas/07-memoria-vram-e-teardown.md) · [08](receitas/08-texturas-etc1-etc2.md) |
| **Chrono Trigger** | Cocos2d-x 3.14 | ABI do `nativeControllerButtonEvent`; refill de áudio fixo (anti-underrun) | [05](receitas/05-audio-opensles-sdl.md) · [06](receitas/06-controle-input-gptokeyb.md) |
| **Crazy Taxi** | NativeActivity SDL2 | teclado→keycode Android pra gptokeyb | [06](receitas/06-controle-input-gptokeyb.md) |
| **Resident Evil 4** | Unity Mono | FMOD AudioTrack fake; deadlock job-system (preservar post no sem_init) | [05](receitas/05-audio-opensles-sdl.md) · [troub.02](troubleshooting/02-deadlock-job-system.md) |
| **Streets of Rage 4** | MonoGame/.NET nativo | runtime .NET + MonoGame em GLES2; BYO-data via APK | [10](receitas/10-empacotando-portmaster.md) |
| **Terraria** | Unity IL2CPP | IL2CPP ES2 com player/mundo/áudio/controle | [01](receitas/01-iniciando-um-port.md) |
| **Dysmantle** | GameActivity | crash handler + BRK traps; ETC1 cache offline | [troub.01](troubleshooting/01-diagnostico-de-crash.md) · [08](receitas/08-texturas-etc1-etc2.md) |
| **GTA Vice City** | RenderWare (re3) | limite de instruções do shader (MAX_LIGHTS); Z-clipping 2D | [03](receitas/03-domando-a-mali450.md) |
| **Elderand** | Unity IL2CPP URP | zram multi-stream (gargalo de RAM); handshake de startup do gfxworker | [07](receitas/07-memoria-vram-e-teardown.md) · [troub.02](troubleshooting/02-deadlock-job-system.md) |

---

## 🚀 Dica de Ouro: "tem som mas a tela está preta"
90% das vezes é um destes:
*   **Driver hardcoded vazando** — nunca force `SDL_VIDEODRIVER` ([09](receitas/09-display-e-sdl-driver.md)).
*   **Shader não compilou** — cheque o log (`L0005`/`P0004`) ([03](receitas/03-domando-a-mali450.md)).
*   **Framebuffer sujo do run anterior** — faça o teardown EGL na saída ([07](receitas/07-memoria-vram-e-teardown.md)).
*   **Depth/Z-clipping** em 2D (Unity/GameMaker).

---
*Este ecossistema cresce a cada port. Descobriu um fix novo? Documente aqui — vira receita pro próximo jogo.*
