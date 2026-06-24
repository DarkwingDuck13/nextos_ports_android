# 🖥️ Display: deixe o vídeo no automático (e a armadilha do SDL estático)

A regra mais importante e mais contra-intuitiva do display nesses devices: **não hardcode driver de vídeo**. O `wayland` é só uma camada sobre o `kmsdrm`; o SDL2 do device resolve o backend sozinho.

## 1. NUNCA force `SDL_VIDEODRIVER`
Não set `SDL_VIDEODRIVER` no launcher nem no código. O display é 100% automático. Se não aparecer imagem, o problema está noutro lugar (EGL/shader/teardown) — ache a config do sistema, não force o driver.

## 2. A armadilha do SDL ESTÁTICO dentro do jogo (lição do Castlevania SOTN)
Cenário traiçoeiro: o jogo tem um **SDL2 estático embutido** que só conhece o backend `android`. O `control.txt`/launcher seta `SDL_VIDEODRIVER=wayland` + `SDL_AUDIODRIVER=pulseaudio` — **certo para o SDL2 do device** (o egl_shim), mas esse env **vaza** pro SDL estático do jogo → `SDL_Init` falha com *"failed to initialize SDL subsystem"* → NULL deref → SIGSEGV. Pior: rodava sob gdb e quebrava via launcher.
* **Fix:** depois de criar a janela com o `egl_shim_create_window`, faça `setenv("SDL_VIDEODRIVER","android")` e `setenv("SDL_AUDIODRIVER","android")`. O SDL2 **do device** já criou o contexto e mantém o backend dele; o SDL **estático do jogo** passa a usar `android` (que ele conhece) e inicializa. Display segue automático.

## 3. Validação visual: olhe a TV
Capturar `/dev/fb0` por SSH **não reflete** a tela de verdade pós-cold-boot (vem preta mesmo quando a TV mostra imagem). Validação real = **olhar a TV** ou usar `fbgrab` de outra sessão. Para abrir o jogo por SSH e ver na TV, rode o `.sh` em **foreground com bash puro** — nunca `nohup`/`&`/`setsid`/`tee` em vfat (destaca do VT → trava no tee → tela preta).

## 4. "Tela preta" tem 3 suspeitos rápidos
1. **EmulationStation masked** não sobe pós-reboot → display não inicializa → tudo preto. (`unmask` + `start`.)
2. **Driver hardcoded** vazando (ver acima).
3. **EGL/shader/teardown** — contexto não criado, shader não compilou, ou framebuffer sujo do run anterior (ver [teardown](07-memoria-vram-e-teardown.md)).

---
*Resumo: display é automático — nunca force SDL_VIDEODRIVER. Se o jogo tem SDL estático, set `android` DEPOIS de criar a janela. Valide na TV, não no fb0 por SSH.*
