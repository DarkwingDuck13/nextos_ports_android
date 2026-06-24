# 🔊 Áudio: do OpenSLES/FMOD fake até o PulseAudio

O jogo Android toca som por uma de três vias: **OpenSL ES**, **FMOD** (via `AudioTrack` do Java) ou **OpenAL**. Nenhuma delas existe no device do jeito que o jogo espera. A estratégia é sempre a mesma: **fingir a API que o jogo chama e puxar o PCM por um callback do SDL2**, que entrega no backend de áudio do sistema.

## 1. A regra de ouro: NUNCA force o driver de áudio
Não setar `SDL_AUDIODRIVER` no launcher nem no código. O áudio vem **automático** do SDL2 do device (PulseAudio no Mali-450, PipeWire no X5M). Hardcodar quebra: o SDL estático de alguns jogos só conhece `android` e, se o env vazar, o `SDL_Init` do áudio falha. Se não tocar, ache a config do sistema — não force.

## 2. OpenSL ES → callback SDL (o caminho mais comum)
Nosso `opensles_shim.c` implementa o suficiente do OpenSL ES (`slCreateEngine`, `Realize`, `CreateAudioPlayer`, `RegisterCallback`, `Enqueue`). Em vez de mandar pro hardware, abrimos **um** `SDL_AudioDevice` e, no callback do SDL, puxamos os buffers que o jogo enfileirou (modelo *pull*).

## 3. FMOD via `AudioTrack` fake (ex.: RE4)
Alguns Unity tocam por FMOD, que no Android usa `org.fmod.FMODAudioDevice` (uma thread Java escrevendo num `AudioTrack`). Implementamos um **`FMODAudioDevice` fake** no JNI: quando o jogo "escreve no AudioTrack", a gente redireciona o PCM pro callback SDL → PulseAudio.

## 4. O bug do engasgo: buffer de refill raso (ex.: Chrono Trigger)
Sintoma: áudio gagueja/estala em loop. Causa: o limiar de refill estava amarrado ao tamanho do último enqueue (~3 KB), mas o callback do SDL consome ~12 KB por vez → **underrun** constante.
* **Fix:** limiar de refill **fixo e folgado** (ex.: 32 KB), com teto de chamadas por ciclo (ex.: 64). Opcional: thread de áudio com período curto (~2 ms) e um ganho fixo (~0.65) pra não saturar.

## 5. OpenAL no X5M (PipeWire)
No X5M o `openal-soft` precisa de backend PipeWire/Pulse compilado (senão fica mudo). E o PCM HDMI entra em **XRUN** no modeset do DRM master → backend ALSA preso. Fix em `alsoft.conf`: `drivers=pipewire,pulse,alsa` (PipeWire nativo é resiliente ao modeset).
* **Medir sem ouvir:** `cat /proc/asound/card0/pcm0p/sub0/status` — `XRUN` + `hw_ptr` parado = mudo; `RUNNING` + `hw_ptr` avançando = tem som.

---
*Resumo: finja a API (OpenSL/FMOD/OpenAL), puxe o PCM por callback SDL, deixe o driver no automático, e dê folga no buffer de refill.*
