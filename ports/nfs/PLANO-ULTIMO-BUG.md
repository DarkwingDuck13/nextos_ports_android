# PLANO — ÚLTIMO BUG do NFS MW: sprites/glyphs com tex=0 (spinner + fontes quebrando)

Device **192.168.31.164** (subnet .31, senha nextos). Port em `~/nextos_ports_android/ports/nfs/`.
Build `./build.sh`. Tudo funciona (áudio, gameplay, cores, latência) MENOS este bug de render.

## O SINTOMA (2 faces do mesmo bug, segundo o NextOS)
1. **Spinner do disclaimer**: a bolinha de loading (O) aparece como **pauzinho (I)** + decorações
   fantasma. Tela de logo/disclaimer logo após EA/Firemonkeys/MOST WANTED (que renderizam OK).
2. **Fontes do menu**: funcionam nas primeiras seções, mas **após ~4-5 seções de menu ficam "feias"**
   (garbled) e quebram TODAS dali em diante.
NextOS suspeita (e os dados batem) que é a MESMA raiz: "não é a fonte, é algo que faz a fonte ficar
assim".

## JÁ DESCARTADO (debugado, não chutado — não repetir)
- ❌ Pool de Paint exaurindo: **resolvido** com cache por (path,size) — só ~48 paints únicos (de 256).
- ❌ Fonte falhando ao abrir: **resolvido** com redirect `data/files/`→`data/Android/data/.../files/`
  (read_file + my_fopen). `NFS_FONTLOG`: **0 FALHOU, 0 InitFont falhou** agora.
- ❌ Ciclo de vida de textura por delete: o DELLOG mostrou **0 glDeleteTextures** no momento do bug.
- ❌ ABI softfp/hardfp: já corrigido (cores OK).
Ou seja: a fonte **CARREGA 100%** (paint válido, ttf lido). O bug é no **RENDER do glyph/sprite**.

## HIPÓTESE CENTRAL (a investigar)
Os sprites `.sba` (ícones, spinner, decorações) E os glyphs de fonte às vezes desenham com
**tex=0** (link sprite→textura resolve NULL na engine — PARTE 11). O `atlas_rebind` (egl_shim.c) é
um PALIATIVO que binda "o último atlas grande" (ou per-programa) nesses draws → quando binda o atlas
ERRADO, o sprite/glyph fica garbled. Após algumas seções, o estado do atlas muda e o glyph de fonte
pega o atlas errado → "fica feia". O spinner é o mesmo: binda atlas errado → pauzinho em vez de O.
**A fonte carrega, mas o DRAW dela vira tex=0 e o atlas-rebind estraga.**

## PLANO DE ATAQUE (próxima sessão, contexto fresco)
### Fase 1 — CONFIRMAR a hipótese (instrumentar o draw da fonte/spinner)
1. Rodar com `NFS_AUTOSHOT=1 NFS_DRAWLOG=1 NFS_TEXLOG=1`, navegar até o bug (4-5 seções) e capturar
   o frame quebrado (`auto.raw` → PIL). NextOS reproduz e avisa "bugou"; capturar na hora.
2. **Logar, NO MOMENTO DO BUG, o que os draws de glyph/sprite bindam**: `g_unit_tex[0]` (0 ou id?),
   `g_cur_prog`, e o que o `atlas_rebind` binda. O DRAWLOG atual capa em `g_drawn<90` (só os 1ºs
   draws) → **AUMENTAR/RESETAR o cap** ou logar contínuo num ring p/ pegar os draws do bug, não os
   do boot.
3. Confirmar: o glyph de fonte que quebrou está com tex=0? Se SIM → é o link sprite→textura +
   atlas-rebind. Se tem textura válida mas garbled → é a rasterização (font_shim) ou o upload.

### Fase 2 — ATACAR A RAIZ (link sprite→textura)
O fix REAL (não o paliativo atlas_rebind): achar POR QUE o sprite/glyph binda tex=0.
- A engine sobe o atlas como GL tex N (glGenTextures→glBindTexture(N)→glTexImage2D), mas o objeto
  Texture do .sba tem glid=0 (ou o sprite referencia outro objeto). RE: quem chama glGenTextures
  (thunk @0xb26534) e ONDE guarda o id; quem chama glBindTexture (@0xb2647c) e qual id usa.
  Os XREFs são PIC não-resolvidos pelo Ghidra → usar scanner movw/movt próprio OU auto-analysis.
- **Runtime mais tratável**: hookar `glBindTexture` (já é my_glBindTexture em egl_shim) e logar o
  `__builtin_return_address(0)` quando binda 0 vs N, p/ achar a função da engine que faz o bind do
  sprite. Depois RE essa função (com `~/re-tools` pyghidra; libapp em /tmp).
- Alternativa pragmática se a RE travar: melhorar o atlas-rebind p/ correlacionar o glyph de FONTE
  ao seu atlas de glyph (rastrear o ÚLTIMO atlas pequeno/quadrado por programa? distinguir glyph-
  atlas de UI-atlas) — mas isso é mais paliativo.

### Fase 3 — VALIDAR
Navegar 10+ seções de menu + abrir loja/sair/perfil sem quebrar fontes; spinner do disclaimer vira
uma rodinha (O). NextOS valida visualmente.

## FERRAMENTAS / ENVS
- `NFS_AUTOSHOT=1` (screenshot auto.raw — DEFAULT-OFF p/ não martelar o vfat; ligar só p/ debug).
- `NFS_FONTLOG` (criação/redirect de paint), `NFS_DELLOG` (deletes), `NFS_TEXLOG` (uploads/atlas
  candidate), `NFS_DRAWLOG` (draws: fbo/u0/prog/blend — AUMENTAR o cap g_drawn<90).
- `NFS_NOATLASHACK` (desliga o atlas-rebind — testar se as fontes ficam BLACK em vez de garbled =
  confirma que é o atlas-rebind estragando), `NFS_NOPROGATLAS` (desliga o per-programa).
- Captura: `cp auto.raw snap.raw` no device + scp + PIL `frombytes RGBA 1280x720 + FLIP_TOP_BOTTOM`.
- RE: `~/re-tools` (pyghidra; `export GHIDRA_INSTALL_DIR=~/re-tools/ghidra_12.1.2_PUBLIC
  JAVA_HOME=~/re-tools/jdk-21.0.11+10`), libfmod já em ~/nfs-stage; copiar libapp p/ /tmp.
- ⚠️ MASCARAR emustation (`systemctl mask emustation`) p/ não interferir; vfat fica read-only se
  martelado (auto-screenshot) → `mount -o remount,rw /storage/roms`.

## ESTADO ATUAL (commit 88ff04a)
Fontes CARREGAM 100% (cache + redirect). Falta o RENDER (glyph/sprite tex=0 → atlas errado).
Ver memória PARTE 11/19/20 e HANDOFF-PROXIMA-SESSAO.md.
