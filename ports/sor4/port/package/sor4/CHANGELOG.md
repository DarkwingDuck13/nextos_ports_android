# Changelog — Streets of Rage 4

## 1.1
- **CO-OP LOCAL ate 4 jogadores!** O build mobile fundia todos os controles no Player 1 e
  nao chamava o handler de "entrar" (join). Destravado: cada controle que apertar A na selecao
  de personagem vira um player novo (P2/P3/P4). Validado com 2 (Axel + Blaze na fase, 2 vidas).
- **Fix de crash** no caminho dificuldade -> selecao de personagem (reportado em RG40XXH/MuOS):
  o Wwise sequestrava o sinal que o GC do .NET usa. Roteado pela nossa camada -> sem crash.
- **Barra de vida** corrigida (saia magenta com pontinhos no Mali-450 -> agora verde solida).
- **Performance/logs**: removido o spam de debug que rodava por textura/asset/evento de audio
  (log de sessao caiu de ~milhares de linhas p/ ~15; menos I/O no carregamento).

## 1.0
- Primeiro release. Port NATIVO (MonoGame/.NET 9 CoreCLR + GLES2), BYO-data via APK
  1.4.5; o progressor extrai/patcha/converte (ASTC->ETC1) na 1a execucao e apaga o APK.
- Audio Wwise via reimpl OpenAL: musica, vozes e SFX de combate. Musica troca limpa ao
  mudar de cena (roteamento por HIRC, sem sobreposicao); golpe com volume ajustado.
- Backend de video (fbdev/kmsdrm) e audio (pulse/pipewire/alsa) auto-detectados pelo
  device — o launcher nao forca nenhum. Binario unico glibc 2.30 (portavel entre devices).
- Controles via gamepad nativo (SDL); SELECT+START sai do jogo. `sor4.gptk` opcional.
- Pasta `licenses/` com as licencas de todos os componentes redistribuidos.
