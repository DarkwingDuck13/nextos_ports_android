# Legend of Mana — progresso sessão Claude 2026-07-06

Continuação do checkpoint Codex. Foco: jogo jogável completo no Mali-450 (.79).

## ESTADO ATUAL (build v11 em src/, binário deployado)

Fluxo já alcançado: logos → loading → **menu principal** (fundo + New Game/
Load Game/Options + fontes boas) → **B confirma** → **tela Select Language
renderiza PERFEITAMENTE** (日本語/English/Français/Deutsch/Español/한국어/
简体中文/繁體中文 + caixa de diálogo + cursor fada). Áudio SDL abre 44100Hz 2ch.

Print bom do Select Language: scratchpad `lom_v10_cs2.png`.

## DESCOBERTAS-CHAVE desta sessão

1. **Fontes boas = GL_ALPHA passthrough puro.** O experimento ALPHA→LUMINANCE
   e ALPHA→LUMINANCE_ALPHA DEIXOU AS FONTES FEIAS/QUADRADAS. REVERTIDO. O
   índice do CLUT está em **.r** (não .a) — patch `.r→.a` no shader CLUT
   deixou a tela TODA PRETA (regressão), também revertido/desativado.
   → Baseline de render = exatamente o do Codex, sem mexer em formato/shader.

2. **CONTROLE FUNCIONA.** Confirmado no device:
   - Menu principal: **dpad navega** (down moveu cursor, 26400 px de diff).
   - **B (AKEYCODE_BUTTON_B=97) = CONFIRMAR** (não A!). B no New Game avançou
     para o Select Language.
   - Injeção: dois caminhos criados em tools/ e no binário:
     * `tools/padinject.c` — injeta no event2 do pad físico (regra NextOS:
       nunca clonar, injetar no eventN real). Mapa Twin USB PS2 (0810:0001):
       a=b2 b=b1(0x121) x=b3 y=b0, dpad=HAT0X/Y. (chegou ao kernel mas o SDL
       do jogo pode não reler js0 — o caminho confiável foi o FIFO abaixo.)
     * **FIFO `/tmp/lomcmd` no próprio binário** (main.c pump_cmd_fifo):
       `echo "t <tecla>"` = tap; `d`/`u` = down/up; `T x y` = toque (coords
       1280x720); `q` = sair. Teclas: up down left right a b x y l1 r1 start
       select back. ESTE é o canal de automação que funciona.

3. **Select Language NÃO navega por dpad** — é menu touch-nativo (mobile).
   Por isso adicionei o comando `T x y` (toque) ao FIFO. Falta validar o toque
   no device (device travou antes de confirmar). REGRA #5: escolher **English**
   (cursor default = 日本語 japonês). English fica ~(660,195) na tela.

4. **Sem FBO/render-to-texture.** VRAMW=0, ATTACH=0, CopyTex=0 no run inteiro.
   Só 1 alloc RGBA 1024x512 (tex=3). O PSX/CLUT é resolvido inteiro no
   fragment shader (u_tex índice em .r + u_texClut paleta). Programa CLUT
   bilinear = program 27 (shaders 25 VS + 26 FS), variante point = shader 30.
   u_clutAddr lido = -1897220,0.249 (valor estranho, mas o menu/fontes/sprite
   da fada renderizam com ele — então NÃO é a causa dos pretos).

5. **loadClass pedidos = só net/gorry/cloud/CloudManager e
   gamecenter/GameCenterManager** (online, irrelevantes; stubs FakeClass OK).
   Nenhum asset falha ao abrir: assetpacks_001/002.gfs abrem, jogo escreve
   cache em m2lib/data_000_0102.bin. GgcGetStatusSignIn→0 em loop (benigno).

## HIPÓTESE ATUAL sobre os PRETOS (logo do título + personagens)

Como fontes, diálogos, sprites e o Select Language renderizam via o MESMO
caminho CLUT (.r), os "pretos" NÃO são o caminho CLUT geral. Suspeitas, em
ordem, a testar quando reentrar no fluxo:
- Título "Legend of Mana" e retratos podem ser **assets on-demand** de um pack
  específico (ONDEMAND_ASSET_PACK_NUM=2) que responde "instalado" mas cujo
  conteúdo real não está no path servido → textura vazia/preta. VERIFICAR:
  logar AAssetManager_open que retornam NULL / fopen que falham AO ENTRAR na
  tela dos personagens.
- Ou texturas grandes **GL_ALPHA 2048x2048** (41 uploads) usadas como u_tex de
  um programa que NÃO é o CLUT — se esse programa lê .r, GL_ALPHA dá 0. Achar
  qual programa desenha as tex 174-307 (2048²) e ver o canal que lê.
  → correlação draw↔programa↔textura já instrumentada (DRAW-scr/PSX/VRAMW).

## COMO RETOMAR (device travou — precisa power-cycle)

Device sagrado root@192.168.31.79 travou 2x nesta sessão (1x no experimento
LUMINANCE_ALPHA por RAM; 1x no teste de toque/captura). Ao voltar:

```sh
cd /home/felipe/nextos_ports_android/ports/legendofmana
./build.sh
SSHO="-F /dev/null -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8"
ssh $SSHO root@192.168.31.79 'killall legendofmana; sleep 2; mount -o remount,rw /storage/roms'
rsync -e "ssh $SSHO" -rlt legendofmana root@192.168.31.79:/storage/roms/ports/legendofmana/legendofmana
ssh -f $SSHO root@192.168.31.79 'cd /storage/roms/ports/legendofmana; ./legendofmana >/tmp/legendofmana-run.log 2>&1 &'
# esperar "DRAW-PSX" no /tmp/legendofmana-debug.log = menu pronto
# sequência p/ entrar no jogo em INGLÊS:
ssh $SSHO root@192.168.31.79 'echo "t b" > /tmp/lomcmd'         # New Game
# Select Language (touch): tocar English
ssh $SSHO root@192.168.31.79 'echo "T 660 195" > /tmp/lomcmd'   # English (ajustar y)
# captura: dd /dev/fb0 3686400 bytes -> ffmpeg bgra 1280x720
```

IMPORTANTE: SSH sofre asfixia — usar `nice -n 19` no dd e ConnectTimeout.
Não empilhar capturas rápidas. Matar+confirmar 0 instâncias antes de lançar
(regra: 2 jogos travam o device).
