# 🧠 Memória, VRAM e o Teardown que evita a tela preta

Os devices Mali têm pouca RAM (832 MB no Amlogic-old; ~480 MB úteis + zram no RK3326) e pouca VRAM. Jogo de Android assume 2–4 GB. Aqui está como manter o jogo vivo — e como **sair** sem travar o device.

## 1. VRAM: reduza no upload, não no jogo
* **Cap de textura:** alguns engines criam render targets gigantes. Detecte a resolução nativa e respeite `GL_MAX_TEXTURE_SIZE` real (não hardcode 1024).
* **Meia-resolução de textura:** rebaixar todas as texturas pra 1/2 (offline, num cache ETC1/ETC2) derruba a VRAM drasticamente (ex.: Bully 129 MB → 27 MB) com perda visual pequena. Ver receita de [texturas](08-texturas-etc1-etc2.md).
* **Limpe mips** e ignore o que não é essencial.

## 2. RAM: zram multi-stream (a lição do Elderand)
Quando o gargalo é RAM/swap (boot trava aleatório, device "weda"), o culpado costuma ser swap single-stream saturando:
* `echo 4 > /sys/block/zram0/max_comp_streams` (multi-stream) + `lz4` + tamanho generoso + `swapon -p100`.
* `swappiness` moderado (~60), e **swapoff do swap em SD** (swap no cartão trava infinito).
* ⚠️ **NUNCA `swapoff` com swap CHEIO** → deadlock em D-state, só reboot resolve.
* Re-aplicar a cada boot se o classifier bloquear o `custom_start.sh`.

## 3. O "mate por /proc/*/exe", não por nome
`pkill -x`/`pkill -f` por caminho **não casa** (o engine vira `{Main}`, o launch é `./jogo`) e deixa **zumbi** segurando 120 MB de RAM + VRAM = falso-OOM. **Sempre** mate varrendo `/proc/*/exe`. E confirme **0 instâncias** antes de lançar outro jogo (2 jogos juntos travam o device).

## 4. O Teardown do Mali (resolve "tela preta + reboot" entre runs)
No fbdev Mali, sair com `_exit()` direto deixa o framebuffer num estado ruim → próxima execução vem preta, às vezes reboota. **Antes de sair**, faça o teardown:
```c
glFinish();
eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
eglTerminate(dpy);
SDL_QuitSubSystem(SDL_INIT_VIDEO);
```
Isso estabiliza dezenas de ciclos sem reboot. Em casos de watchdog (timeout de boot), um `egl_shim_emergency_teardown()` no handler evita que o `exit_group` trave o fbdev.

---
*Resumo: corte VRAM no upload (cap + meia-res + mips), zram multi-stream pra RAM, mate por /proc/*/exe, e SEMPRE faça o teardown EGL antes de sair.*
