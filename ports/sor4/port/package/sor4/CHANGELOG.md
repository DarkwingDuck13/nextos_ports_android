# Changelog — Streets of Rage 4

## 1.0
- Primeiro release. Port NATIVO (MonoGame/.NET 9 CoreCLR + GLES2), BYO-data via APK
  1.4.5; o progressor extrai/patcha/converte (ASTC->ETC1) na 1a execucao e apaga o APK.
- Audio Wwise via reimpl OpenAL: musica, vozes e SFX de combate. Musica troca limpa ao
  mudar de cena (roteamento por HIRC, sem sobreposicao); golpe com volume ajustado.
- Backend de video (fbdev/kmsdrm) e audio (pulse/pipewire/alsa) auto-detectados pelo
  device — o launcher nao forca nenhum. Binario unico glibc 2.30 (portavel entre devices).
- Controles via gamepad nativo (SDL); SELECT+START sai do jogo. `sor4.gptk` opcional.
- Pasta `licenses/` com as licencas de todos os componentes redistribuidos.
