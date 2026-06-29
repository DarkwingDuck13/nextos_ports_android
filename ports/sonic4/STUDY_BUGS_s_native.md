# Sonic 4 EP2 — Estudo de bugs + fluxo NATIVO de input/áudio (2026-06-29)

Estudo pedido pelo NextOS: "estude tudo primeiro, veja o fluxo nativo, resolva nativamente".
Device de teste: R36S **ArchR** (RK3326, Mali-G31, ES3.2), root@169.254.170.2.
libfox.so analisada = md5 `d77489abf54523046ade35425e537782` (= a do device).

## BUGS REPORTADOS PELO TESTER (luis)
1. **Y = Left no level select.** No menu de seleção de fase, o botão Y age como Left.
2. **Score preso na tela (Oil Desert Act 3).** Depois do mini-game de apertar A pra score,
   os números do score ficam na tela o resto da fase.
3. **Volume 100% no Knulli/ROCKNIX.** Áudio sai sempre no volume cheio nesses CFW.

---

## FLUXO NATIVO DE INPUT (RE confirmada da libfox.so)

Estrutura do pad que o engine lê (de `GmPadUpdate` @0x3cda24):
- `[pad+0]` = máscara DIGITAL de botões (halfword)
- `[pad+2]` = analog LX (signed), `[pad+4]` = LY, `[pad+6]` = RX, `[pad+8]` = RY

**Verdade dos bits de menu** (lidos de `.data`, símbolos `g_gs_env_key_*` @0x866a4c–56):
- UP=0x0001  DOWN=0x0002  LEFT=0x0004  RIGHT=0x0008
- DECIDE=0x0020 (A)  CANCEL=0x0080 (B)
- → **batem 100% com o nosso FOX mask** (main.c:851-868). FOX_Y=0x0100 é bit separado.

`GmPadDirect` (0x3cde58): retorna `gmPaddirectFromPlayer0` (gm-level, que main.c escreve direto)
SE flag `[ptr+4]&0x800`; senão cai em `AoPadDirect` (0x2112f0). `OUYAGetPauseKey`=0x8000 (confirmado).

**Caminho de input do EP2 = ÚNICO:** main.c monta o FOX mask e faz
`SetPadData(mask,0,0,0,0,0)` + escreve `gmPaddirectFromPlayer0` / `gmPadAnalogLX/LY` direto.
- **Paddleboat NÃO existe na libfox** (readelf: 0 símbolos) → `pb_send_key` é no-op no EP2.
- push_key_event/AKEYCODE_* (android_shim) NÃO alimentam o engine do EP2 (sem Paddleboat;
  a Java foi substituída pelo nosso shim). Só o FOX mask conta.

### Bug #1 (Y=Left) — análise + TESTE EMPÍRICO no R36S
DESCARTADO: colisão de bit no gmPad (Y=0x100 ≠ Left=0x4), Paddleboat, gptk (y=v→FOX_Y certo).
HIPÓTESE de remap de keymap em runtime → **TESTADA E DESCARTADA**: build instrumentado
(SONIC_KEYDUMP, main.c) rodou no R36S ArchR e os bits ficaram ESTÁVEIS o tempo todo:
`[keydump] L=0004 R=0008 U=0001 D=0002 decide=0020 cancel=0080` por 1320 frames (~22s). **Sem remap.**
Mapping SDL do R36S também está CERTO: `r36s_Gamepad ... x:b2, y:b3, dpleft:b15` → Y(b3)→BUTTON_Y,
não dpleft. Logo **Y=Left NÃO reproduz no R36S** pela teoria FOX/menu.
CONCLUSÃO: é específico do **device/menu do luis** — provável (a) controller-mapping do device dele
com Y mal-bindado (gamecontrollerdb diferente), OU (b) o "level select" dele usa um caminho de input
que não tracei (gmPadRepeat?). PRECISA: qual device o luis usou + qual menu exato (Episode select?
World map? Extras stage select?) + idealmente o gamecontrollerdb do device dele.
(Infra de teste validada: launch compat glibc2.27 com timeout duro no R36S = boot→loop→música→kill
limpo, device não trava. Reusável p/ os próximos testes.)

### Bug agente #1 (ALTA): `sonic_game_started` PRESO em 1
imports.c:59-93. Único reset = log "Create World Map". Game-over→título, "Exit to Title" do pause
NÃO logam isso → flag fica 1 no título → A vira FOX_A_GAME(0x20) em vez de FOX_A_MENU(0x8020);
o título lê 0x8000 (AoPadSomeoneStand) → **A não passa do título depois de jogar 1 fase** (soft-lock).
### Bug agente #2 (MÉD): `strcasestr(msg,"spstage")` falso-positivo
Qualquer linha de log com "spstage" (ícone/disponibilidade de special no world map, results,
nome de asset SpStage0X) re-seta started=1 num MENU → A perde o bit 0x8000 de confirm.
### Bug agente #4 (BAIXA): aliasing de bits no FOX mask
FOX_PAUSE(0x4000)==FOX_L3 ; FOX_R3(0x8000) compartilha bit do confirm de título. Clicar L3/R3
na special stage injeta bit em 0xC000 → pausa indevida (mesma classe do fix de pulo-pausa).

**FIX NATIVO p/ #1/#2 (o que o NextOS pediu):** trocar a heurística de string por leitura do
ESTADO REAL do engine. Sinais nativos candidatos: `g_gs_main_sys_info` (0x99f658), o estado
do state-machine de gameplay, `ss::CMain::s_main` (special) já usado. "Em gameplay de fase" =
existe stage/player ativo. Substituir `sonic_game_started` por essa leitura mata #1, #2 e a
fragilidade toda de uma vez.

---

## ÁUDIO — fluxo nativo + bug #3 (volume)
EP2 usa `AudioHelper` (Java) → nosso `sonic_audio.c` (SDL2). **opensles_shim.c NÃO é usado pelo
EP2** (engine não chama OpenSL) → os achados de concorrência do agente em opensles_shim
(conv_buf static, ring SPSC) são DEAD CODE pro EP2; não priorizar pro EP2.

### Bug #3 (volume 100% Knulli/ROCKNIX) — ROOT CAUSE CONFIRMADO
- ArchR (R36S): SDL abre **pulseaudio**, e o tracking de volume do sistema FUNCIONA
  (log: `sys volume -> 0.80/0.50/0.20/0.05/1.00/0.35` seguindo os botões). OK.
- Knulli/ROCKNIX: pulse/pipewire FALHAM (`pw.loop ... can't make support.system handle`,
  `ALSA: Couldn't open audio device`) → SDL cai no **card CRU "audiocodec"** (sonic_audio.c
  "Plano C"). Com card cru os botões de volume controlam o mixer do OS mas nosso stream
  escreve direto no hw → sempre cheio. O `sa_read_sys_volume` (sonic_audio.c:1006-) lê
  `/var/run/batocera-pending-volume` + `/userdata/system/batocera.conf` — **esses paths não
  existem no ROCKNIX/Knulli** → volume fica no default 0.80 (~"100%").
**FIX NATIVO:** adicionar em `sa_read_sys_volume` os paths/chaves de volume do ROCKNIX e do
Knulli (e/ou ler via `amixer get`/control do card). Não mexer no path pulse (ArchR já OK).

### Achados sonic_audio.c que VALEM pro EP2 (agente)
- #4 cache key: `CacheEntry.key[96]` vs lookup 128/256 → truncamento em 95 → cache miss p/
  chaves longas → re-decode. Latente.
- #5 voice steal: 32 vozes cheias → rouba slot 0 (corta som + órfã o handle). Glitch audível.

### Bug #2 (score preso Oil Desert Act3) — não reproduzido
Hipótese: camada de UI/efeito do mini-game (contador de score do "press A") não é limpa pelo
engine OU é agravada pelos nossos patches LOWFX (`SsDrawObjectShadow*`→0, bloom off) que
desligam draws. Precisa repro on-device pra confirmar. Lead: rodar a fase SEM LOWFX
(SONIC_LOWFX off) e ver se o score some — isola se é nosso patch ou o engine.
**Leads de símbolo (libfox) p/ quando houver repro:** o "apertar A pra score" é provável um
gimmick de SLOT/score — `GmGmkSlotInit`@0x392e28, `GmGmkStopperSlotInit`@0x39f67c,
`GmPlayerAddScore`@0x3dc7ac, `GmPlayerComboScore`@0x3dc7c0, `OFPostScore`@0x2112ac. A
persistência = o objeto de display de número do gimmick não é destruído/escondido ao acabar
(achar o Exit/Release do GmGmk* correspondente e ver se é chamado). Patches suspeitos que
poderiam pular um cleanup: `videoIsPlaying`→0 / `MediaPlayerisPlaying`→0 / `clMovie::isEnd`→1
(se o clear do score for disparado por uma transição/movie). Testar 1o com LOWFX off + sem
esses patches de movie, repro na fase.

---

## PRÓXIMOS PASSOS (ordem)
1. Build instrumentado: dump runtime de `g_gs_env_key_*` + mask por botão → fecha Y=Left.
2. Fix nativo `sonic_game_started` (ler estado real) → mata bugs agente #1/#2 + ajuda menus.
3. Fix volume Knulli/ROCKNIX em `sa_read_sys_volume` (paths desses CFW / amixer).
4. Repro Oil Desert Act3 c/ e sem LOWFX → score preso.
Device R36S ArchR online; ES rodando; lançar com cuidado (matar sonic antes, regra do device).
