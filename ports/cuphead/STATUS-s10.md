# Cuphead Mobile -> Mali-450 (Amlogic-old) — STATUS sessão 10 (2026-06-11)

## 🏆 TELA DE TÍTULO RENDERIZA! Boot COMPLETO.
Cuphead + Mugman + logo dourado + fundo radial vermelho + prompt "ENTER". Render estável.

## Como chegar ao título
1. Device .164: swap (2GB loop + 512MB) + `vm.overcommit_memory=1` + `systemctl mask emustation`.
2. Bundles em /storage/cuphead-sa/AssetBundles (299: atlas/font/persistentes do título + TODA a música).
3. `./go-title.sh` (retry automático: $PC=9 Cuphead.Init é flaky ~1/3).

## Os 2 muros da sessão
- **$PC=9 (Cuphead.Init -> Resources.Load("Core/CupheadCore"))**: crash na desserializacao do
  prefab (libunity), leitura de heap nao-inicializado (fault = fragmento de string, muda todo run).
  FLAKY: passa ~1/3 com a config completa (FORCEINTEG+TEXHALF=512+SAPATH+NOREFRESHDLC). Contornado
  por RETRY. Raiz nao resolvida (provavel job de worker nao roda sob integracao forcada).
- **$PC=17 (preload do titulo travado)**: cases 16/17 do start_cr = loops foreach AssetLoader.LoadAsset.
  Pedia `music_mus_intro_dontdealwithdevil_vocal_reverse` nao deployado -> "Unable to open archive" ->
  coroutine async nunca completa. FIX = deployar TODA a musica (132 bundles, 102MB).

## Próximo
controles (USB Gamepad em js0/event2; Rewired) -> som (FMOD retorna -1) -> gameplay (deploy de
bundles de fase sob demanda) -> empacotar.

Binário inalterado desde s9 (commit 04f2939). ZERO código novo na s10 — só config + deploy + retry.
