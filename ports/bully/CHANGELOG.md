# Bully: Anniversary Edition (NextOS Elite) — CHANGELOG

---

## v9.0 FINAL — 2026-06-14 (noite) — JANELA DE EXTRAÇÃO na tela + CLARITY HIGH automática

> Atualização grande do v9.0: novo sistema de instalação **BYO-DATA com janela e
> barra de % na tela** (estilo dos ports oficiais do PortMaster) e a **Clarity HIGH
> forçada em todos os devices** direto no binário. Validado no **X5M** (KMSDRM) e no
> **Mali-450** (fbdev). Os dois binários (`bully` + `bully.compat`) atualizados.

### 🪟 Janela de extração (BYO-DATA com imagem + progresso)
- Você coloca o seu **APK do Bully 1.4.311** na pasta `ports/bully/` e abre o port.
  Na 1ª vez abre uma **JANELA** (ferramenta `progressor`) mostrando a extração com
  **barra de porcentagem** (`Dados 3/5… [#######…..] 36%`). Ao terminar, libera o APK
  (~3 GB de disco) e o **jogo inicia sozinho**. Não precisa fazer mais nada.
- Funciona no **X5M (KMSDRM/Valhall)** e no **Mali-450 (fbdev/Utgard)**. (A "patcher
  UI" love2d do PortMaster não cria janela no KMSDRM do X5M — pede alpha que o
  scanout XRGB não tem; o `progressor` cria, por isso é o usado.)
- O `Bully.sh` ficou **limpo**: só chama a janela; toda a lógica de extração está no
  `tools/bully_extract.src`.

### 🔆 Clarity HIGH automática em TODOS os devices (no binário)
- 🔑 **A Clarity NÃO vinha do settings.ini** (a teoria antiga estava errada). A
  própria engine decide a Clarity por uma **verificação de hardware**: aparelho
  potente (X5M 4 GB) → **High**; aparelho de 1 GB (Mali-450) → **Low**. Essa
  verificação **ignora o settings.ini e não persiste** (mudar no menu volta pra Low
  ao reabrir).
- **Fix:** hookamos a função dessa verificação (`GetResolutionDefault`) no binário →
  **força High sempre**, em todos os devices. Quem quiser baixar p/ ganhar
  performance: `BULLY_CLARITY=low` (ou `med`) no ambiente.
- Por isso o **`settings.ini` saiu do pacote** (não controla a Clarity; o jogo cria o
  resto do default sozinho).

### 🧱 Binário compat (R36S/ArkOS) com GCC Debian
- `bully.compat` (glibc-velha) agora é **GCC Debian 10 → GLIBC_2.17, ~110 KB** (não o
  zig gordo de 1.1 MB, que era o que sumia com as fontes no R36S — o "texto
  invisível" era o COMPAT, não o MSAA). Ambos os binários já trazem o hook da Clarity.

### 📺 Vídeo
- **Auto-detectado** (sem `SDL_VIDEODRIVER`): X5M sobe **KMSDRM**, Mali-450 sobe
  **mali/fbdev** sozinhos.

### 🙏 Créditos da janela de extração
- A ferramenta `progressor` + fonte `FiraCode-Regular.ttf` + `common.src` vêm do port
  **TMNT: Shredder's Revenge** do PortMaster (método de extração reaproveitado).
  Licenças em `licenses/`.

> ⚠️ As seções v9.0 anteriores abaixo citam "MSAA" como causa do texto invisível e
> "settings.ini RS_High" como controle da Clarity — **ambas foram revisadas**: o texto
> invisível era o binário compat (zig→GCC) e a Clarity é a verificação de hardware
> (resolvida pelo hook acima).

---

## v9.0 — 2026-06-14 — TEXTO INVISÍVEL resolvido (MSAA) + compat anti-freeze + multi-device

> Versão única que cobre **todos os devices** (fbdev Mali-450, KMSDRM Mali novo/X5M,
> ArkOS/R36S glibc-velha). Junta os fixes do v8.3 (áudio/escola Mali-450) e resolve
> o **texto invisível** que os testers relatavam no R36S e a **trava de ~30 min** no
> binário compat. Validado: Mali-450 (fbdev, texto OK, sem swap) + X5M (KMSDRM
> auto-detect, áudio limpo).

### O texto invisível ERA o MSAA (você estava certo)
- **Causa-raiz:** o MSAA 4x automático em painel ≤600px (R36S 480p), combinado com
  `Clarity=RS_High` (render target em resolução cheia), fazia o Mali-G31/GLES3.2 do
  R36S **perder o texto**. Prova cruzada decisiva: **Mali-450** (RS_High, 720p, SEM
  MSAA) = texto **OK**; **R36S** (RS_High, 480p, COM MSAA) = texto **some**. O único
  diferenciador era o MSAA.
- **Fix:** MSAA auto **REMOVIDO** (default `BULLY_MSAA=0`). Sem MSAA, o R36S fica
  igual ao Mali-450 (texto OK) **e mantemos `RS_High` (nitidez) em todos** — não
  precisei sacrificar a nitidez de nenhum device. Quem quiser AA força `BULLY_MSAA=4`.
- Confirmação de que NÃO era diferença de "commits" perdida: o diff v7→v8.1 mostra
  que o MSAA já existia nas DUAS (não era a diferença de launcher); o que mudou e
  expôs o bug foi o `RS_High` passar a ser de fato aplicado (enum MAIÚSCULO).

### bully.compat rebuildado (R36S/ArkOS finalmente com anti-freeze)
- O `bully.compat` (glibc 2.17, usado por ArkOS/dArkOS/R36S) estava SEM o despejo por
  `MemAvailable` — tinha só o despejo antigo por teto fixo (que faz churn). Era
  **justamente** o device que travava aos ~30 min e o binário dele não tinha o fix.
- **Recompilado com zig** (do mesmo `src/`): agora os DOIS binários têm o despejo por
  **pressão real de RAM** (`MemAvailable` < piso) → o R36S ganha o anti-OOM/anti-freeze.

### Multi-device / launcher
- **`controlfolder` robusto:** itera os candidatos e escolhe o que REALMENTE tem
  `control.txt` (não só o 1º diretório que existe). O X5M só tem `control.txt` em
  `/storage/.config/PortMaster` → o bloco simples (gtavc) caía em `/roms/ports/...`
  sem control.txt → `$directory` vazio → `//ports/bully` → não abria. **Corrigido.**
- **Auto-detect de vídeo validado:** sem `SDL_VIDEODRIVER`/`pm_platform_helper`, o
  X5M sobe **KMSDRM** (Mali-G310 GLES3.2) e o Mali-450 sobe **mali/fbdev** sozinhos.
- **`settings.ini` vem pronto no zip, SEM seds/migração** (Clarity `RS_High`
  MAIÚSCULO = o único formato que o parser do jogo aplica). O launcher só recria por
  segurança se o arquivo for apagado.
- **`alsoft.conf` (non-mmap) re-incluído** p/ devices ALSA-puro (R36S/ArkOS):
  evita o "Broken pipe" sob carga. Devices com PulseAudio (Mali-450) seguem no
  **pulse direto** (`ALSOFT_DRIVERS=pulse`); o X5M (ALSA) já era limpo (neutro).

### Pendência conhecida (não-bloqueante)
- **Música mp3 no X5M:** o bundle `libmpg123` foi removido no v8.1 (quebrava o LAUNCH
  em glibc-velha). Sem ele o X5M não toca o rádio (sfx/vozes via OpenAL funcionam). Um
  re-bundle SEGURO (lib glibc 2.17 via zig) fica p/ v9.1.

---

## v8.3 — 2026-06-14 — FIX do áudio estourado + trava da escola no Mali-450 (Amlogic-old)

> Resolve o "áudio estourado" e a "tela preta/trava na escola" que apareceram nas
> versões v6→v8 no Mali-450/Utgard (Amlogic-old, A53 1GB), mantendo TODOS os fixes
> multi-device do v8 (binário dual glibc, alsoft.conf, Clarity). Validado por ouvido
> no device: som ok, mapa sem travadas, escola entra de boa.

### Causa-raiz (duas, independentes)
1. **Áudio estourado:** o **despejo de RAM** (anti-OOM do v8) disparava por um teto
   FIXO de textura (200MB) que o working-set normal já ultrapassava — disparava a
   cada ~2s MESMO com RAM livre. Cada `implOnLowMemory` é uma varredura síncrona na
   thread de render → trava → starva a mixagem do OpenAL → underrun ("Broken pipe")
   contínuo = estouro. (E o `glFinish` por render-to-texture agrava em movimento.)
2. **Trava da escola:** no Mali-450 a escola CABE via `BULLY_TEX_LIGHT/HALF` (como
   no v4, que não tinha despejo e rodava liso). O despejo do v8, ao disparar na
   escola (pico), fazia varredura/churn → travava o device.

### Config final do Mali-450/Utgard (validada por ouvido: "audio ok + jimmy ok")
O launcher detecta o Utgard e liga 3 coisas que, juntas, deixam o áudio LIMPO e
CONSISTENTE em movimento + a escola entrando + a roupa do Jimmy firme:
1. **Despejo OFF** (`BULLY_TEX_BUDGET_MB=0`/`BULLY_LOWMEM_MB=0`) — a escola cabe via
   TEX_LIGHT/HALF (= v4); o despejo só travava.
2. **glFinish OFF** (`BULLY_RTT_FINISH_MINDRAWS=999999` → glFlush sempre) — o glFinish
   por render-to-texture travava a GPU Utgard a cada composição → estouro ao mover.
   Com glFlush: áudio limpo E a roupa do Jimmy continua firme (glFinish não era
   essencial p/ ela neste device).
3. **Buffer de áudio grande** (`alsoft_pulse.conf`: periods=4 period_size=2048,
   fix-rate) — absorve a variação de CPU residual do streaming → áudio CONSISTENTE
   (sem isso variava run-a-run). Custo ~100KB RAM + ~150ms latência (imperceptível).
- Launcher reescrito no **padrão PortMaster** (base reVC/gtavc): header if/elif,
  `CUR_TTY`, e **NÃO força `SDL_VIDEODRIVER`** (o SDL auto-detecta mali/kmsdrm) nem
  chama `pm_platform_helper` — o jogo abre via auto-detect (validado).
- `settings.ini` (Clarity RS_High) agora vem **pronto no zip** (em vez de seed).

### Correções (mecanismo)
- **Despejo desligado no Mali-450/Utgard** (detecção: `mali-utgard`/`/sys/module/mali`/
  SoC Amlogic Gx*). A escola cabe via TEX_LIGHT/HALF (= v4). Demais devices de 1GB
  (R36S Mali-G31 etc.) **mantêm o despejo** (lá previne o OOM de ~30min).
- **Despejo (nos devices que usam) agora dispara por PRESSÃO REAL de RAM**
  (`MemAvailable` < piso) em vez de teto fixo de textura → não dispara à toa →
  áudio limpo + anti-OOM. Env: `BULLY_LOWMEM_MB` (piso), `BULLY_TEX_BUDGET_MB`
  (fallback em kernel sem MemAvailable; 0 = desliga).
- **glFinish por render-to-texture** ganhou throttle opcional
  (`BULLY_RTT_FINISH_MINDRAWS`): glFinish só nos RTT pesados (a roupa do Jimmy,
  ~289+ draws), glFlush nos leves → menos stall de GPU em movimento. Default 0
  (= sempre glFinish, comportamento seguro do v8).

### Nota
- `bully.compat` (glibc 2.17, devices antigos) segue o build do v8.1 nesta release;
  os fixes acima estão no `./bully` (NextOS/glibc ≥2.38, usado no Amlogic-old). O
  despejo-off no Utgard vale pros dois (é do launcher).

---

## v8 — 2026-06-13 — FIX do vazamento de memória (OOM / "trava depois de ~30min")

> Resolve o problema relatado por vários testers de R36S/1GB: o jogo roda mas a
> RAM enche e ele crasha/congela depois de ~20-30 min (clássico: trava no portão
> da escola). zram/swap só adiavam. Agora é resolvido na raiz.

### Causa
A engine do Bully faz **streaming de textura** do mundo (carrega conforme você
anda). No Android, ela **despeja** as texturas antigas quando o SO manda
`onLowMemory`. O nosso port nunca enviava esse sinal → a engine achava que tinha
RAM infinita e **nunca chamava `glDeleteTextures`** → as texturas acumulavam até
OOM. Medido na instrumentação: o jogo criava 2300+ texturas e deletava **7**;
~390 MB de textura viva e só subindo.

### Fix
O binário agora monitora a memória de textura viva e, quando passa de um teto,
**dispara o `implOnLowMemory` da própria engine** — que roda o despejo de
streaming dela (seguro, ela sabe o que não está em uso) e chama `glDeleteTextures`.
Medido depois do fix: deletes saltaram de 7 → 748, e a memória **para de crescer**
(fica oscilando numa banda em vez de ir ao infinito).

O teto é automático por RAM (no `Bully.sh`, ajustável em `BULLY_TEX_BUDGET_MB`):
- **~1GB** (R36S, TSP, RG35XX): 200 MB + encolhe texturas (`TEX_HALF`) também no
  kmsdrm → working set menor.
- **~2GB**: 320 MB.
- **3GB+** (X5M): 448 MB (quase nunca dispara → sem churn/stutter).

ANTI-CHURN: se o despejo não reduz a memória (engine já no piso de working-set
da cena), o cooldown recua de ~2s p/ ~30s — evita pedir despejo repetido que
viraria stutter. (Validado no Mali-450: de 56 disparos/126s p/ 6 disparos/120s.)

Validado no X5M (eviction dispara, del 7→748) E no Mali-450/833MB (eviction
funciona no Utgard sem crash, memória estável ~187MB parado, anti-churn ok).
Aplicado nos DOIS binários (`bully` e `bully.compat`).

### Áudio: anti "broken pipe" (R36S/ArkOS) + libs de fallback
- **`alsoft.conf` non-mmap**: em devices só-ALSA (sem PulseAudio, ex: R36S/ArkOS)
  o OpenAL caía no caminho ALSA *mmap* que faz underrun ("broken pipe") e travava
  o áudio em sessões longas. Agora o launcher aponta `ALSOFT_CONF` p/ um config
  non-mmap + buffers maiores — **só quando não há PulseAudio e NÃO no X5M**
  (cujo áudio já funciona; não é tocado).
- **`audiolibs/` (libopenal/libmpg123) REMOVIDO (v8.1)**: o bundle era glibc
  2.43/2.38; em device de glibc velha o preload pegava a NOSSA cópia e falhava
  (`GLIBC_2.38 not found`) → **o jogo não abria** (regressão reportada no v8:
  Nicolas/mik "abria no v7, não no v8"). Sem o bundle, o preload dessas libs
  falha de boa (como no v7) e o jogo abre. (A música extra do X5M foi sacrificada
  pra não quebrar o launch dos outros; se quiser de volta, recompilar libmpg123
  contra glibc baixa via zig, como o bully.compat.)

### Clarity (resolution) preso em Low — FIX DO TOKEN (VALIDADO no Mali-450)
Causa: o jogo LÊ o nosso `settings.ini` (confirmado: `fopen ./settings.ini OK`)
mas só aplica os enums em MAIÚSCULO (`RS_High`, `SS_Off`...). O seed antigo
escrevia minúsculo (`rs_high`) → o parser ignorava → caía no default (Low). O
binário só contém os tokens maiúsculos; minúsculo NUNCA casava. Agora o launcher
semeia em maiúsculo + migra `settings.ini` antigos (sed). VALIDADO no Mali-450
(.164): com `RS_High` a cena renderiza nítida (lapVar ~305) vs `RS_Low` borrado
(~34) — o token maiúsculo É respeitado e o perfil NÃO clampa em hardware fraco.
(No X5M o setting é ignorado — o perfil já dá alta resolução, por isso "1080p
fica ótimo".)

### Ainda pendente (próximas versões)
- Tela preta no RGCubeXX/Knulli (sem `/dev/dri` → backend `mali` não apresenta).
- Qualidade fina em 640x480 (separado; o Clarity acima já ajuda muito).

---

## v7 — 2026-06-12 — DOIS binarios (NextOS + compat GLIBC_2.17) = roda em qualquer device; MSAA auto 480p; SEM swap

> Codigo do jogo IDENTICO ao v6. A v7 resolve de vez a compatibilidade
> multi-CFW com a abordagem certa: DOIS binarios nativos, sem glibc bundlada,
> sem patchelf, sem swap.

### Por que DOIS binarios (e por que isso encerra a saga de glibc)
As versoes v5/v6 enviavam UM binario linkado contra glibc 2.43 (a do NextOS) +
uma glibc bundlada em `runtime/`. Isso gerou uma cadeia de erros em outros CFWs
(`tunable_is_initialized`, `libgcc_s.so.1 not found`, `__libc_pthread_init`,
`libdl.so.2`...), todos sintomas da MESMA causa: misturar a nossa glibc nova
com o sistema do device. A solucao correta (a mesma de todo port PortMaster) e
compilar contra uma glibc VELHA. Agora o pacote traz os dois:
- **`bully`** — build NextOS, precisa GLIBC >= 2.38. Cobre NextOS (2.43), muOS,
  Knulli, ROCKNIX, S905X5M — todos os CFWs modernos. E o build canonico.
- **`bully.compat`** — MESMO codigo compilado em Debian buster, precisa so de
  **GLIBC_2.17** (de 2012) -> roda em QUALQUER device, ArkOS/dArkOS (2.27-2.30)
  inclusive. Linka libdl/libpthread no estilo classico, que existe tanto na
  glibc velha (lib real) quanto na nova (stub de compat).
O launcher escolhe sozinho: glibc do device >= 2.38 usa `bully`, senao usa
`bully.compat` (que roda em tudo). NENHUM dos dois usa runtime bundlado nem
patchelf — rodam 100% nativos, o ld.so do proprio CFW resolve SDL2/EGL/libgcc_s
e o libmali casado com o kernel. Adios `runtime/`.

### SEM swap / SEM zram
Removido por completo (decisao do projeto desde o inicio; a v7-test tinha
reintroduzido por engano). O launcher NAO mexe mais em memoria do device —
nada de zram, nada de swapfile, zero desgaste de SD. Em device de RAM baixa,
use Clarity=Low e mantenha shadows off.

### MSAA 4x AUTOMATICO em painel pequeno (fix do "grainy/pixelated")
Relatos de imagem serrilhada/pixelada em painel 480p (RG34XX-SP, RG35XX H,
R36S). O MSAA 4x existe desde a v5 e resolve isso, mas vinha DESLIGADO na v6.
Agora: painel com altura <= 600px -> MSAA 4x liga sozinho (barato em GPU
tile-based; fallback automatico p/ 0x se a GPU recusar). Paineis 720p/1080p sem
MSAA. Override no Bully.sh (`BULLY_MSAA=4` forca / `BULLY_MSAA=0` desliga).
Pra nitidez, confira tambem Settings > Clarity = HIGH.

### Fix do "stack smashing detected" (H700/Mali-G31 — Knulli/muOS)
Em alguns devices o jogo abortava com `*** stack smashing detected ***` logo
ao subir a thread GameMain (depois do gate Rockstar, quase no gameplay). Causa:
o libGame (bionic) le a stack-guard de `tpidr_el0+0x28`; sob a glibc desses
devices esse slot do TCB e instavel em threads novas -> a canary "muda" no meio
da funcao -> abort. (Em outras glibc, ex. NextOS/X5M, o slot calhava estavel,
por isso so quebrava em alguns aparelhos.) FIX: o binario `bully` reserva um
TLS pad fixo que estabiliza esse slot (mesma solucao ja validada no Dysmantle)
+ `__stack_chk_fail` neutralizado como insurance. Aplicado nos DOIS binarios
(`bully` e `bully.compat`) -- ambos confirmados SEM regressao (passam GameMain
e renderizam o gameplay no Mali-450 e no Mali-G310). O `bully.compat` foi
RECOMPILADO do nosso source com `zig cc -target aarch64-linux-gnu.2.17` (ver
build_compat.sh) -> GLIBC_2.17 + o TLS pad, roda em qualquer device de glibc
antiga (ArkOS/dArkOS) sem o stack smashing.

### Creditos
A ideia de compilar contra glibc velha (e o binario `bully.compat` GLIBC_2.17)
veio da comunidade, recompilando a partir do source publicado
(`github.com/felc18-blip/nextos_ports_android`). Obrigado!

### Notas
- 1o boot demora (tela preta ate ~5min): extracao dos ~3GB de assets do APK p/
  o SD. So acontece uma vez.
- Tela preta COM audio em alguns KMSDRM (ex: RGCubeXX): e backend de display
  (page-flip), nao glibc — em investigacao; mande as linhas `[sdl]`/`[gl]` do
  log.txt.

---

## v6 — 2026-06-12 — Controles PS2 via gptokeyb + troca de itens + binario novo

### Resumo
Os CONTROLES sairam do binario e foram pro padrao PortMaster (gptokeyb +
`bully.gptk`): layout PS2 completo, remapeavel SEM recompilar, padronizado
em qualquer CFW. E a troca de ITENS — que nao existia em botao nenhum do
build mobile — foi destravada via toque sintetico no HUD.

### Controles (layout PS2, editavel no bully.gptk)
| Botao | Funcao |
|-------|--------|
| Cruz / Circulo / Quadrado / Triangulo | correr / pular / bater / agarrar-interagir |
| L1 | mira/lock-on (abas p/ esquerda nos menus) |
| R1 | disparar arma (abas p/ direita nos menus) |
| L2 / R2 | trocar item anterior/proximo (toque sintetico no slot do HUD) |
| L3 / R3 | olhar p/ tras / agachar |
| dpad | zoom mapa (cima/baixo) + tarefas (esq/dir) |
| SELECT+START | sair do jogo |

### Por dentro (engenharia reversa do libGame 1.4.311)
1. **Enum real de eventos** confirmado no binario (__type_GamepadButton):
   o build mobile ignora shoulders/triggers (16-19) e religa as acoes extras
   no cluster 6/7/12-15 (6=mira 7=tiro 12=olhar-tras 13=agachar). O jogo le
   SO eventos (GetGamepadButtons/Axis nunca sao chamados no gameplay).
2. **Troca de item nao tem botao** no build touch — solucao: o binario injeta
   um TOQUE (AND_TouchEvent) no slot de arma do HUD quando L2/R2 sao
   apertados, com coordenadas relativas a resolucao (vale 480p/720p/1080p).
3. **bully.gptk** = mapeamento fisico->acao por device. ATENCAO: comentario
   `#` SOMENTE em linha propria (o parser do gptokeyb invalida a linha se o
   comentario vier apos o valor). Pads que reportam shoulder<->gatilho
   trocados: basta cruzar as linhas l1/l2 (e r1/r2) no .gptk.
4. Sem gptokeyb no CFW -> fallback automatico pro controle nativo da v5.

### Outras mudancas
- **Clarity (resolution) vem em HIGH por padrao**: em instalacao nova o jogo
  criava o settings.ini com rs_low; o launcher semeia um settings.ini padrao
  com `resolution=rs_high` na 1a execucao (depois a escolha do usuario manda).
- **MSAA 4x agora e OPCIONAL e vem DESLIGADO** (linha comentada no Bully.sh;
  descomente `export BULLY_MSAA=4` p/ ligar em paineis 480p).
- **Instancia unica (SO X5M/Valhall, gateado por device-tree)**: mata
  instancia anterior presa antes de subir — evita "jogo sem tela + som
  duplicado" (briga de DRM master). Nos demais devices nem executa.
- **Saves do Android sao compativeis**: copie `BullyFile*`+`FileInfo*` de
  `Android/data/com.rockstargames.bully/files/` pra pasta `bully/` e eles
  aparecem no Load Game.
- Ferramentas de diagnostico embutidas (so agem se voce pedir):
  `echo N > /dev/shm/bully_btn` dispara evento de botao; `echo "x y" >
  /dev/shm/bully_tap` injeta toque; `touch /dev/shm/bully_shot` salva
  screenshot RGBA em /dev/shm/bully_shot.raw.
- X5M/Valhall (KMSDRM Amlogic vendor): o launcher espera o PCM HDMI fechar
  antes de abrir o jogo (modeset com PCM aberto congela o audio do sistema
  nesse kernel). Gateado por device-tree; nos demais devices nem executa.

---

## v5 — 2026-06-12 — Roda em qualquer CFW: glibc bundlada + fallback de vídeo + MSAA 4x

### Resumo
Versão multi-device de verdade, motivada por testes de terceiros (Trimui Smart Pro,
CFWs Debian-based tipo ArkOS). Três muros, três fixes — Mali-450 (NextOS) intocado.

### Fixes
1. **glibc 2.43 bundlada** (`runtime/`: ld-linux-aarch64.so.1 + libc.so.6 + libm.so.6).
   Devices com glibc < 2.34 recusavam o binário (`GLIBC_2.34 not found`). O binário
   agora usa interpretador RELATIVO (`runtime/ld-linux-aarch64.so.1`, via patchelf) —
   por isso o launcher PRECISA do `cd "$GAMEDIR"` antes de executar. O `runtime/` vem
   primeiro no LD_LIBRARY_PATH; SDL2/EGL/GPU continuam sendo os do device (glibc nova
   executa libs compiladas contra glibc velha numa boa, o contrário é que não).
2. **Fallback de driver de vídeo** (`egl_shim.c`): se o SDL_VIDEODRIVER pedido não
   existir no SDL2 do device (ex.: Trimui stock sem backend kmsdrm), solta a variável
   e re-inicializa deixando o SDL escolher o backend que existe. Confirmado em campo:
   no Trimui Smart Pro, remover o SDL_VIDEODRIVER fez a imagem aparecer — agora o
   binário faz isso sozinho.
3. **MSAA 4x nos devices KMSDRM** (`BULLY_MSAA=4` no launcher + `egl_shim.c`): pede
   multisample na criação da janela, com fallback automático pra 0x se o driver
   recusar. Faz diferença visível em painéis 480p (serrilhado some); quase de graça
   em GPU tile-based. **Mali-450/fbdev: a variável nem é setada — caminho idêntico
   ao v4** (sem custo, sem risco). Ajustável: `BULLY_MSAA=0|2|4` no Bully.sh.
4. **Launcher**: multiarch dirs de Debian/Ubuntu (`/usr/lib/aarch64-linux-gnu`,
   `/lib/aarch64-linux-gnu`) no fim do LD_LIBRARY_PATH p/ achar SDL2/EGL nesses CFWs.
5. **SELECT+START confiável em TODO device** (`jni_shim.c`): o hotkey de sair já
   existia no binário, mas só no `GetGamepadButtons` (caminho de POLLING, que o
   jogo quase nunca usa — ele lê input por EVENTOS). Por isso v3/v4 funcionava em
   uns devices e não em outros (dependia do gptokeyb de cada CFW). Agora o check
   roda no `pump_gamepad()` (todo frame, no loop principal) — sai em qualquer
   device, com qualquer PortMaster, mesmo sem gptokeyb. O gptokeyb continua sendo
   lançado como backup (inofensivo).

---

## v4 — 2026-06-11 — Qualidade restaurada no KMSDRM (highp + trilinear) + launcher no padrão PortMaster

### Resumo
A v3 trouxe o suporte multi-backend (rodar em KMSDRM). A **v4 RESTAURA a QUALIDADE GRÁFICA**
que tinha sido cortada pro Mali-450 — mas só nos devices KMSDRM capazes (Mali-G310/R36S); o
Mali-450 (fbdev) mantém os cortes que ele precisa. Tudo no MESMO binário, adaptativo.
Rodando **60fps liso** no S905X5M (Mali-G310, 1080p).

### Qualidade restaurada (só no KMSDRM, via `bully_is_kmsdrm()`)
1. **Shaders em highp** (`imports.c`) — antes forçava mediump (limite do Mali-450 Utgard). No
   G310/R36S mantém highp → cores e iluminação com precisão total (sem banding).
2. **Filtragem trilinear / mipmap** (`imports.c`) — antes forçava bilinear (GL_LINEAR, sem
   mipmap, que o Utgard não tinha). No KMSDRM deixa o trilinear do jogo passar + gera a cadeia
   de mipmap (`glGenerateMipmap`) → texturas nítidas e estáveis ao longe (sem tremido/serrilhado).
   Ajuda ainda mais em telas 640x480 (R36S).
3. (já na v3) detail maps completos (sem TEX_LIGHT), textura full (sem TEX_HALF), resolução nativa.

### Launcher no padrão PortMaster (`Bully.sh`)
- Detecção de controlfolder cobrindo também `/storage/.config/PortMaster` (NextOS).
- Template padrão: `control.txt` + mod, `get_controls`, `$GPTOKEYB`, `pm_platform_helper`, `pm_finish`.
- Hotkey de SAIR **SELECT+START** via gptokeyb (padrão PortMaster), com fallback pro gptokeyb do
  sistema quando a instalação do PortMaster é parcial.

### Compatibilidade
- **Amlogic-old** (Mali-450 fbdev 720p): cortes do Utgard mantidos, 100% intacto.
- **S905X5M** (Mali-G310 KMSDRM 1080p): highp + trilinear + full, 60fps liso.
- **R36S** (KMSDRM 640x480): highp + trilinear + full.

---

## v3 — 2026-06-11 — Suporte multi-backend de vídeo (KMSDRM + Wayland) + fullscreen nativo automático

### Resumo
A v2 só rodava em devices **fbdev** (Mali-450 Utgard / Amlogic-old, kernel antigo).
A **v3 roda também em devices com DRM/KMS modernos** (Mali Bifrost/Valhall, kernel mainline) —
ex.: **S905X5M (Mali-G310)**, **R36S**, e similares — **sem quebrar o suporte fbdev**.

O port agora **se adapta sozinho** ao device. Tudo abaixo é automático (com fallback); o
usuário não precisa configurar nada.

---

### As 4 mudanças (cada uma com seu FALLBACK explicado)

#### 1) Launcher adaptativo — escolhe o driver de vídeo certo (`Bully.sh`)
O launcher detecta o tipo de display do device e escolhe o backend do SDL2:

```sh
if [ -e /dev/dri/card0 ]; then
  export SDL_VIDEODRIVER=kmsdrm        # device com DRM/KMS (Mali novo, kernel mainline)
else
  export SDL_VIDEODRIVER=mali          # device fbdev (Amlogic-old Mali-450, kernel 3.14)
  export BULLY_TEX_LIGHT=1             # otimização de textura SÓ pro Mali-450 (pula mapas _n/_s)
  export BULLY_TEX_HALF=1             # otimização de textura SÓ pro Mali-450 (mipmaps + tex÷2)
fi
```

- **Fallback / lógica:** existe `/dev/dri/card0` → é DRM/KMS → usa `kmsdrm`. Senão → é fbdev →
  usa o driver `mali` (EGL fbdev) **e liga as otimizações de textura do Mali-450** (que são
  necessárias só na GPU fraca; em GPUs modernas seriam desperdício de qualidade, então ficam
  desligadas no caminho kmsdrm).

#### 2) Criação da janela GBM/EGL robusta — fallback de formato de cor (`egl_shim.c`)
No KMSDRM, o plano de scanout primário é **XRGB8888 (sem canal alpha)**. Se a janela pedir
ARGB (com alpha), o SDL2 não acha um formato GBM compatível e falha com
`Can't window GBM/EGL surfaces on window creation`.

- **Fallback / lógica:** o port tenta criar a janela com `alpha=8` (ARGB, ideal pro fbdev mali);
  **se falhar, tenta de novo com `alpha=0` (XRGB, que o KMSDRM aceita)**. Assim funciona nos dois
  sem o usuário saber de nada. Log: `CreateWindow OK com alpha=0 (KMSDRM/XRGB)`.

#### 3) Apresentação correta no KMSDRM — o "page-flip" que faltava (`imports.c`)
**Esse era o motivo da tela ficar preta no KMSDRM mesmo o jogo renderizando.**
No fbdev, `eglSwapBuffers` cru já joga a imagem direto no framebuffer (visível). No KMSDRM,
quem faz o **page-flip** (mandar o buffer pro scanout da tela) é o `SDL_GL_SwapWindow` —
o `eglSwapBuffers` cru sozinho NÃO apresenta nada → tela preta.

- **Fallback / lógica:** o port detecta o backend (`bully_is_kmsdrm()` =
  `SDL_GetCurrentVideoDriver() != "mali"`) e:
  - **KMSDRM/Wayland:** roteia o `eglSwapBuffers` do jogo pro `SDL_GL_SwapWindow` (faz o page-flip).
  - **fbdev (mali):** mantém o `eglSwapBuffers` cru original (**Amlogic-old 100% intacto**).

#### 4) Resolução nativa automática — fullscreen em qualquer device (`jni_shim.c`)
Antes o jogo reportava **1280x720 fixo (hardcoded)**. Num device 1080p, ele desenhava só num
cantinho 1280x720 → não preenchia a tela.

- **Fallback / lógica:** agora o jogo reporta a **resolução REAL nativa do device**
  (`SDL_GetDesktopDisplayMode`) — cada device preenche a tela com a sua resolução:
  - **R36S** → 640x480
  - **Amlogic-old (Mali-450)** → 720p
  - **S905X5M** → 1080p

  Log: `implOnSurfaceChanged 1920x1080 (real)`.

---

### Compatibilidade
| Device | GPU | Display | Resolução | Status v3 |
|--------|-----|---------|-----------|-----------|
| Amlogic-old | Mali-450 (Utgard) | fbdev | 720p | ✅ inalterado (continua perfeito) |
| S905X5M | Mali-G310 (Valhall) | KMSDRM | 1080p | 🆕 NOVO |
| R36S | (KMSDRM) | KMSDRM | 640x480 | 🆕 NOVO |

> **Binário aarch64 ÚNICO** roda nos três — ele usa o `SDL2`/`EGL` instalado em cada device.
> Nada de versão separada por device.

---

## v2 — 2026-06-08 — Primeiro release (Mali-450 / Amlogic-old) — INÉDITO MUNDIAL

- Bully: Anniversary Edition (v1.4.311) rodando no **Mali-450 (Amlogic-old)** via so-loader —
  primeiro port **aarch64/Linux/PortMaster** do mundo (antes só existia Vita 32-bit e Switch).
- Cadeia: so-loader AArch64 → libGame.so (Android) → JNI estático → SDL2-mali EGL GLES2 fbdev.
- Mundo + escola jogáveis, controle + áudio OK, com fixes específicos do Utgard (texturas,
  shaders highp→mediump, alpha-test da roupa do Jimmy, limite de textura da GPU).
- BYO-data: o usuário fornece o APK do Bully (v1.4.311); o launcher extrai os dados no 1º boot.
