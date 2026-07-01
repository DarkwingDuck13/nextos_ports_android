# Bully2 handoff - 2026-06-30

## Estado atual

- Zip v11 local final: `/home/nextos/Área de trabalho/Bully v11.zip`
- SHA256 do zip v11: `5475249ceb387893f3e5d3099522f595037738a7b62c349bb0d8387b69541711`
- O zip esta no formato PortMaster esperado: `Bully.sh`, pasta `bully/`, binario, assets do port, ferramentas de extracao e `bully/gamedata/README.txt`.
- O zip nao inclui APK, OBB, `main.11...obb` ou `patch.11...obb`.
- O zip inclui o nosso patcher de menu/runtime:
  - `bully/tools/extract-bully-data.sh`
  - `bully/tools/ensure-bully-menu-patch.sh`
  - `bully/tools/patch-bully-menu.py`

## Decisao sobre o patch

Existem dois "patches" diferentes:

- `patch.11.com.rockstargames.bully.obb` e dado oficial/proprietario do jogo Android. Ele deve vir da copia legal do usuario e ficar em `bully/gamedata/`. Ele nao deve ir no zip publico.
- O patch do port e nosso script/binario que gera `assets/bully2_patch.zip` depois da extracao legal. Esse patcher ja vai dentro do zip em `bully/tools/`.

O extractor procura primeiro em `bully/gamedata/`, aceita qualquer `.apk` e qualquer `.obb`, e extrai `libGame.so`, `libc++_shared.so`, `assets/data_0..4.zip` e os `.idx`. Depois chama `ensure-bully-menu-patch.sh` para gerar o patch de menu Textures/Light.

## Ark/R36S de teste

- IP atual informado pelo usuario: `192.168.31.104`
- Hostname confirmado: `rg351mp`
- Usuario SSH: `ark`
- Caminho do port no Ark: `/roms/ports/bully`
- Zip instalado no Ark: `/home/ark/Bully v11.zip`
- Hash do zip no Ark foi validado igual ao local.

O envio dos dados grandes foi interrompido a pedido do usuario. No Ark ficou apenas um APK parcial em:

`/roms/ports/bully/gamedata/Bully_1.4.311-60FPS-apkaward.com-Mod.apk`

Tamanho visto no momento da parada: `736987432` bytes. Isso nao e um APK completo e nao deve ser usado para validar o jogo. Se voltar a enviar, usar `rsync --append-verify` para continuar ou apagar o parcial e reenviar do zero.

## Proximo passo sugerido

1. Testar o fluxo real do primeiro boot com o usuario colocando sua copia legal em `bully/gamedata/`.
2. Confirmar que o progressor aparece, extrai tudo e gera `assets/bully2_patch.zip`.
3. Se quiser simular com os dados locais privados, enviar apenas APK/OBB para `bully/gamedata/`; nao colocar esses dados no zip final.
4. Depois da extracao, validar que o jogo abre e que as opcoes `Textures` e `Light` continuam persistentes no menu.

## Observacoes de transferencia

O Wi-Fi do Ark estava lento e oscilando entre aproximadamente 400 e 800 kB/s via SSH/rsync. O `emulationstation.service` consumia muita CPU/RAM durante a copia; parar temporariamente o ES melhorou a memoria livre, mas nao resolveu totalmente a taxa. O power-save de `wlan0` foi desligado durante o teste.

Antes de encerrar, o `emulationstation.service` foi iniciado novamente no Ark.
