# SOR4 — Habilitar 2/3/4 jogadores (co-op local) — HANDOFF

> Status: **PATCH ESCRITO + VERIFICADO NO IL. FALTA SÓ TESTAR NA TV COM 2 PADS.**
> Regra: só commit no master, SEM co-autor Agente. Não abrir/fechar jogo sem matar+confirmar.

## FEITO (2026-06-19, sessão de limpeza de logs)
- **Tool Cecil `coopsplit`** criada: `port/tools/coopsplit/` (Program.cs + csproj). Localiza o `blt`
  da Fase 1 (cujo próximo par é `ldc.i4.1`+`stloc V_4`, SEM offset hardcoded), valida que nenhuma
  instrução mantida salta p/ a região removida, e trunca o método com `ret`. Buildada → `bin/coopsplit.dll`.
- **Verificado no IL**: aplicada na SOR4.dll do device (806821f5) → método `update_game_pad_state_array`
  vai de **360→150 instruções**, terminando em `IL_01A3 blt IL_0007` + `IL_01A8 ret` (Fase 2 fusão e
  Fase 3 limpeza eliminadas). Cecil relê a DLL inteira sem erro.
- **Empacotada**: `port/package/tools/coopsplit.dll` (usa a Mono.Cecil.dll compartilhada, idêntica).
- **Fiada no pipeline (gate opt-in `SOR4_COOP=1`, default OFF até validar)**:
  - progressor BYO: `port/package/tools/sor4_extract.src` (após `video_exists`, antes do `rm SOR4.dll`).
  - dev: `port/tools/buildfix.sh` (no fim).
- **Artefato de teste PRONTO**: `/tmp/SOR4.coop.ready.dll` (md5 ff8ac5bb) = SOR4.dll do device + coopsplit.
  Backup da original do device: `/tmp/SOR4.device.now.dll` (md5 806821f5).

## DEPLOY + TESTE (quando o NextOS voltar com 2 controles)
> O install do device tem `.setup_done` → NÃO re-roda o progressor. Então testa-se trocando a DLL direto.
1. **Matar + confirmar 0** (regra): `pkill -9 -x sor4host` + checar por `/proc/*/exe` = 0.
2. **Backup + deploy** no device `/storage/roms/ports/sor4/host_pkg/`:
   - `cp SOR4.dll SOR4.dll.precoop.bak` ; `scp /tmp/SOR4.coop.ready.dll → host_pkg/SOR4.dll`
   - (opcional, p/ futuros installs) `scp port/package/tools/coopsplit.dll → tools/coopsplit.dll`
3. **Plugar 2 pads ANTES de abrir** (mesmo modelo de preferência). Lançar em foreground:
   `systemctl stop emustation; bash /storage/roms/ports_scripts/StreetsOfRage4.sh`.
4. P1: Story/Arcade → tela de seleção de personagem. P2 aperta confirmar no 2º pad → deve virar Player 2.
5. **Se P2 NÃO entrar**: provável ressalva — rodar com `SOR4_MGLOG=1` p/ reativar os logs `[PAD]` e ver
   se o SDL enumera os 2 pads como PlayerIndex 0 E 1 (se ambos caem em 0, o problema é na camada SDL,
   ANTES do nosso patch). Reverter: `cp SOR4.dll.precoop.bak SOR4.dll`.
6. **Se funcionar**: virar o default do gate p/ ON (`${SOR4_COOP:-1}`) no extract.src + rebuildar o zip.

## O PROBLEMA (reportado pelo NextOS)
Plugando 2 controles, **os 2 movem o MESMO personagem** e o Player 2 nunca entra.

## CAUSA-RAIZ (confirmada no IL — /tmp/sor4_il.txt, 18MB, gerado por dumpil)
O co-op de 4 EXISTE no binário, mas o build Android funde tudo no P1 de propósito:

- `maxPlayerCount = 4` → IL:9807
- Join do jogador: `CharacterSelectionScreen::handle_input_handle_first_button_presses` → IL:93806
  - loop controllers 0..4; se aperta **SELECT (ActionEnum 5)** + `can_assign_controller_to_new_local_player()` + controle não-atribuído → `assign_controller_to_new_local_player(c)`.
- Leitura física CORRETA e separada: `platform_strict::get_game_pad_state(i,...)` → IL:366307
  = `GamePad.GetState((PlayerIndex)i, DeadZone.None)`. Pad 0→PlayerIndex 0, pad 1→1, etc.

### O SABOTADOR: `CommonLib.platform::update_game_pad_state_array` → IL:341532
Roda 3 fases por frame:
1. **Fase 1** (IL:341542–341716): preenche slot[0..3] via get_game_pad_state(i) — slots distintos, OK.
2. **Fase 2 = FUSÃO** (IL:341718–341900): loop v=1..3 → `slot[0].botão |= slot[v].botão` (todos botões) + pega maior stick. → tudo dos pads 1/2/3 vai pro slot 0.
3. **Fase 3 = LIMPEZA** (IL:341902–341932): loop v=1..3 → zera slot[v] (`isConnected=false`, reset_buttons, stick=0).

**Efeito:** todo input cai no Player 1, e `is_controller_connected(1/2/3)` (IL:70946, lê
`currentGamePadStateArray[v].isConnected`) vira `false` → o join nunca é detectado.
É o modelo single-player mobile (qualquer pad BT dirige o herói único).

## O FIX (a fazer)
**1 patch Cecil: truncar `update_game_pad_state_array` após a Fase 1.**
- Inserir `ret` em **IL_01a8** (linha ~341718), descartando Fase 2 (fusão) e Fase 3 (limpeza).
- Resultado: slot[0..3] = pads físicos distintos → `is_controller_connected(1)`=true → P2 aperta
  confirmar na seleção de personagem e entra (idem 3/4).

### Como implementar (encaixa no pipeline existente)
- Ferramentas Cecil ficam em `port/tools/` (ex.: `noopm`, `skipvideo`, `verstub`, `skipcall`).
  Aqui NÃO é no-opar o método inteiro — é **truncar** o corpo. Criar tool nova (ex.: `truncm`/`split2p`)
  ou estender uma existente: localizar o método por nome
  `CommonLib.platform::update_game_pad_state_array`, achar a instrução que inicia a Fase 2
  (o `ldc.i4.1 / stloc V_4` logo após o `blt` da Fase 1) e substituir tudo dali em diante por `ret`.
  Via Cecil: limpar instruções a partir desse ponto e emitir `Instruction.Create(OpCodes.Ret)`,
  recalcular offsets (Cecil faz no write). Cuidado com o `.locals`/maxstack (Cecil reescreve).
- Adicionar o passo no `port/tools/buildfix.sh` E no progressor BYO `port/package/tools/.../extract.src`
  (a cadeia de `sor4host --run-dll` que patcha o SOR4.dll). Gate opcional por env (ex.: `SOR4_COOP=1`)
  pra poder ligar/desligar.

## VALIDAR (precisa do NextOS na TV — não dá por ssh, pad é SDL nativo)
1. 2 controles físicos plugados ANTES de abrir (de preferência mesmo modelo → mesmo
   SDL_GAMECONTROLLERCONFIG do launcher cobre os dois).
2. P1: Story/Arcade → entra na tela de seleção de personagem.
3. P2: aperta confirmar (botão A/SELECT) no 2º pad → deve aparecer como Player 2 e escolher personagem.
4. Todos confirmam → começa com 2 (ou 3/4) na fase.

## RESSALVAS A CHECAR (não bloqueiam, mas podem aparecer rodando)
- Nota antiga do HANDOFF "menu só lê slot 0" é do **TÍTULO** (navMode `simultaneousPlayers`); a
  seleção de personagem usa `simultaneousControllers` (varre 5 índices). Provável OK.
- Confirmar que o MonoGame/SDL enumera os 2 pads como PlayerIndex 0 e 1 (ver `/tmp/sdlprobe` /
  logs `[PAD]`). Se ambos caírem em PlayerIndex 0, o problema é antes (camada SDL), não esse patch.
- Possíveis outros gates mobile-single-player na UI da seleção (ex.: esconder slots 2–4). Não vi
  nenhum no caminho do join, mas só o teste na TV confirma.

## ARQUIVOS-CHAVE
- IL dump: `/tmp/sor4_il.txt` (regerar com `port/tools/dumpil` se sumir).
- Pipeline de patch: `port/tools/buildfix.sh` + progressor BYO em `port/package/tools/`.
- Memória: ver entrada 2026-06-19 em
  `~/.agente/projects/-home-nextos/memory/project_sor4_streets_of_rage4_mali450.md`.
