# HLM2 → R36S — plano de adaptação (próxima sessão)

Estado: **adiado pelo usuário** ("deixo pra depois") + R36S offline agora (cabo desconectado,
`169.254.170.2` sem ping). Mali-450/.79 está 100% (vídeo+gameplay+SFX+música+controle+save).

## Device R36S (do memory)
- IP cabeado `169.254.170.2`, user **root**, sem senha (chave id_ed25519). `ssh root@169.254.170.2`.
- **ArchR** (NÃO ROCKNIX), RK3326, **Mali-G31 Bifrost** (suporta ES3 real), 640x480.

## Pontos de adaptação (precedente = Sonic 4 EP2, MESMO R36S)
1. **Contexto GL adaptativo ES3→ES2 na MESMA janela.** O port hoje força EGL ES2 (certo p/ Mali-450
   Utgard). No Mali-G31/mesa-panfrost o ES2 pode dar `EGL_BAD_CONFIG`→tela preta. Padrão Sonic4
   (commit 3ff310b): tenta ES3 1º, cai p/ ES2; logar GL_RENDERER/VERSION/GLSL. Ver `egl_shim.c`.
2. **Áudio adaptativo:** varrer drivers SDL e escolher (ArchR pode usar ALSA, não pulse). Sonic4
   (7e263ad) varre drivers + força `SDL_AUDIODRIVER=alsa` onde necessário. NÃO hardcodar (regra #6).
   O poll-worker da música é independente de device → deve funcionar igual.
3. **Display 640x480:** o port já usa SDL_GetDesktopDisplayMode (adapta sozinho). Conferir no R36S.
4. **Launcher PortMaster:** paths do R36S/ArchR (control.txt) — o launcher atual já cobre os 4 paths.
   Sem gptokeyb (input nativo pela extensão), SELECT+START sai.
5. **glibc compat:** se o binário (toolchain NextOS aarch64) não rodar no ArchR, ver build_compat.sh
   (Docker bullseye glibc) — mesmo padrão do Castlevania SOTN universal.

## 1º passo quando o R36S voltar online
- `ssh root@169.254.170.2 'uname -a; glxinfo|grep -i renderer'` (confirmar Bifrost/ES3).
- Deploy do mesmo set (hlm2+libyoyo.so+assets/) + run → capturar GL_RENDERER + 1º frame.
- Se tela preta: ES3→ES2 adaptive. Se mudo: scan audio driver.
