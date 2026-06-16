# HANDOFF — BUG DAS FONTES DO MENU (NFS Most Wanted, Mali-450)

Device **192.168.31.164** (subnet .31, senha nextos). Port em `~/nextos_ports_android/ports/nfs/`.
Build `./build.sh`. Deploy `scp nfs root@192.168.31.164:/storage/roms/nfs/nfs` (matar nfs antes;
`mount -o remount,rw /storage/roms` se o vfat ficar RO). Decoder local: `~/nfs-diag/decode.py`.

## 🎯 O BUG (a resolver)
No menu, ao navegar (abrir submenus/diálogos — ex.: o diálogo **EXIT GAME** ("sair"), a **loja**
online, **driver details**), depois de algumas aberturas **TODO o texto fica garbled** (glyphs
quebrados/trocados). Persiste mesmo voltando. Alguns textos ficam OK (ex.: o título "EXIT GAME"
grande renderiza certo) e outros garbled (o corpo do diálogo). NÃO é só online — "sair" (local)
também quebra. Splash/logos/menu inicial estão OK (outra missão, já resolvida, commit 05604a3).

## ✅ DESCOBERTA DECISIVA (o que muda a investigação)
O texto usa um **GLYPH CACHE / ATLAS**: cada caractere é rasterizado UMA vez numa textura-atlas em
posição empacotada; as palavras são compostas desenhando **um quad por glyph** que amostra a região
do glyph no atlas (confirmado: drawString vem 1 glyph por vez, ex. 'E' em (428,19), 'M' em (...)).
- **Página 1 do atlas = `tex=7` (512×512)**; quando enche, a engine cria **página 2 = `tex=28`**
  (vista como 512×512 e 1024×1024 em runs diferentes). A quebra COINCIDE com a criação da página 2.
- **DUMPEI o conteúdo REAL das duas páginas do atlas (NFS_ATLASDUMP) e AMBAS ESTÃO LIMPAS** —
  todos os glyphs corretos (ver `~/nfs-diag/g28_atlas7.png` e `g28_atlas28.png`). **O abm_buf que
  rasterizamos também está limpo** (`NFS_ABMDUMP`).
- **PORTANTO: a corrupção NÃO está no conteúdo do atlas nem na nossa rasterização. Está na
  COMPOSIÇÃO** — os quads de texto amostram a **página/posição ERRADA** do atlas quando a página 2
  existe. Hipótese nº1: os glyphs que foram pra página 2 (`tex=28`) são desenhados amostrando a
  página 1 (`tex=7`) → glyphs trocados → garbled. O título usa glyphs da página 1 (corretos); o
  corpo usa glyphs da página 2 → garbled.

## ❌ JÁ DESCARTADO (com dados, não chute — NÃO repetir)
- **tex=0 / atlas-rebind**: NÃO. `NFS_GLYPHLOG` mostra tex0/fr=0 no menu (glyphs têm textura real).
- **glDeleteTextures**: NÃO. `NFS_DELLOG` = 0 deletes no momento da quebra.
- **Pool de Paints (font_shim)**: NÃO. máx 48/256, sempre HIT, nunca "POOL CHEIO".
- **setSize com valor lixo**: NÃO (sizes válidos: 18..49.5).
- **Conteúdo do atlas corrompido**: NÃO (ambas as páginas limpas — ver acima).
- **Downscale de textura (NFS_TEXSCALE)**: NÃO está ligado em nenhum launcher (off).
- **ABM 512 vs 1024**: o scratch bitmap (abm_buf, getInfo) É 1024×1024. Reportar 512 **CRASHA no
  boot** (testado) — o atlas GL (512) é um objeto SEPARADO do scratch. NÃO mexer no ABM (=1024).

## 🔬 DADOS-CHAVE DA ENGINE (RE/instrumentação)
- A engine **NÃO usa glTexSubImage2D** p/ os glyphs (0 chamadas). Sobe o atlas INTEIRO via
  `glTexImage2D` (re-upload da página toda a cada glyph novo).
- **NUNCA seta GL_UNPACK_ROW_LENGTH** (só GL_UNPACK_ALIGNMENT). (Hookei glPixelStorei.)
- A engine sobe `tex=7` de um buffer próprio (ex. px=0xebd07008) e `tex=28` de outro (0xde0205d0);
  NÃO é o nosso abm_buf (0xe4807008) — ela LÊ nosso abm_buf (via AndroidBitmap_lockPixels, sempre
  retorna abm_buf, bmp sempre 0x457d0) e COMPÕE no atlas dela.
- A fn de bind de textura da engine: libapp off **0x56960c** (`r5=*r5; if(r5==0) bind 0; else bind
  r5[56]`). Draw-site dos quads de UI: off **0x569db8**. (libapp.so é ARM, NÃO Thumb; base no log
  `so_load: load base` — a do range que contém o ra; objdump armhf em
  `~/NextOS-Elite-Edition/build*Amlogic-old*/toolchain/bin/armv8a-emuelec-linux-gnueabihf-objdump`.)

## ➡️ PRÓXIMO PASSO (o que falta)
Confirmar a hipótese nº1 e achar POR QUE a página 2 amostra errado:
1. **Logar, por draw de glyph (draw pequeno texturizado), QUAL textura está ligada (tex0)** e
   correlacionar com texto fino vs garbled. Se o corpo (página 2) desenha com **tex0=7** (página 1)
   em vez de **tex=28**, confirma: a engine bindou a página errada. (Já tenho `g_small_n/g_small_tex0`
   contadores e logs de tex0 em drawString/clear/lock; falta logar tex0 no DRAW dos quads de texto.)
2. Se for página errada bindada: investigar se o NOSSO hook (my_glBindTexture / a ordem de
   glActiveTexture / o estado de unidade de textura) está confundindo a seleção de página da engine.
   Ou se a engine seleciona a página por um id que no port resolve errado.
3. Alternativa de RE: a função de seleção/bind da página do glyph na engine (perto de 0x56960c /
   o draw 0x569db8) — ver como ela escolhe entre tex=7 e tex=28.
4. Possível fix paliativo se a RE travar: **impedir a criação da página 2** (manter tudo na página
   1) — ex. interceptar e aumentar a 1ª textura de glyph atlas, ou reduzir a pressão do cache. Mas
   o correto é a página 2 ser amostrada certo.

## 🔁 REPRODUÇÃO AUTÔNOMA (funciona — sem precisar do NextOS)
Navegação via injetor **moga.txt** (main.c, roda todo frame): `echo <keycode> > /storage/roms/nfs/moga.txt`.
Keycodes: 96=A(confirma/abre) 4=BACK 105=R2 103=R1 22=DPAD_RIGHT 21=LEFT 20=DOWN 19=UP 108=START.
- Lançar: `bash glive.sh` (em /storage/roms/nfs; tem AUTOSHOT→/tmp/auto.raw). Avançar o splash com
  alguns `echo 96>moga.txt`. Menu ≈ frame 1100+.
- Para forçar a página 2 (tex=28) e a quebra: navegação PROFUNDA e variada (entrar em abas com A,
  ciclar com 22/20 dentro, voltar com 4, próxima aba 105) — acumula glyphs únicos até encher a
  página 1. A sequência "for t in 1..5: A; 6×RIGHT; 3×DOWN; BACK; R2" criou tex=28 (GOT28 t=2).
- O diálogo **EXIT GAME** garbled foi reproduzido com `for i in 1..10: A; BACK; R2` (abre cada aba).

## 🧪 INSTRUMENTAÇÃO DISPONÍVEL (envs, tudo gated default-OFF)
- `NFS_GLYPHLOG` — por frame: nº de draws UI pequenos e quantos tex=0.
- `NFS_STRLOG` — drawString (tex0, size, x, y, string) + clear(tex0) + abm_lock(bmp,tex0,pix).
- `NFS_UPLOADLOG` — glTexImage2D (frame, tex, WxH, px) + glTexSubImage2D + glPixelStorei.
- `NFS_ATLASDUMP` — dumpa o buffer REAL de cada upload de atlas (≥256, RGBA) → /tmp/atlas_<tex>_<wxh>.raw.
- `NFS_ABMDUMP` — dumpa o nosso abm_buf (scratch) → /tmp/abm.raw (todo drawString).
- `NFS_DELLOG`, `NFS_FONTDBG` (pool de Paints), `NFS_BINDLOG` (bind/draw tex=0 no disclaimer).
- `NFS_AUTONAV=<período>` — auto-injeta sequência R2/R1/RIGHT (main.c) — atract demo às vezes
  sobrescreve; preferir moga.txt manual.
- ⚠️ Capturas vão pro **/tmp** (tmpfs, NÃO martelar o vfat). /tmp tem 416MB; LIMPAR entre runs
  (`rm /tmp/atlas_*.raw /tmp/*.raw /tmp/live.log`). NÃO usar NFS_SEQSHOT longo (enche /tmp).

## 📦 ESTADO DO CÓDIGO (git)
- HEAD relevante do NFS: commit **05604a3** (splash/disclaimer RESOLVIDO — pin no atlas de boot).
- **Mudanças NÃO-COMMITADAS** em ports/nfs/src/ = APENAS instrumentação gated por env (egl_shim,
  font_shim, imports, jni_shim, main) + hook glPixelStorei. **NÃO quebram boot** (todas default-off).
  O ABM está de volta em **1024** (o 512/abm_dim que quebrava o boot foi revertido). Pode commitar a
  instrumentação ("NFS MW: instrumentação do glyph cache de fonte") OU manter no working tree.
- Launchers no device: `grun.sh` (jogar, com som, sem debug), `glive.sh` (debug, AUTOSHOT+logs).
- ⚠️ MASCARAR emustation (`systemctl mask emustation`) ao testar; restaurar (`unmask`+`start`) ao fim.

## RESUMO DE 1 LINHA
Fontes do menu quebram ao abrir submenus = a engine cria uma 2ª página de glyph-atlas (tex=28) e os
quads passam a amostrar a página/posição ERRADA (ambas as páginas do atlas estão LIMPAS; a falha é
na COMPOSIÇÃO/seleção de página, não no conteúdo). Próximo: logar a textura ligada no DRAW dos quads
de texto p/ confirmar que a página 2 é desenhada amostrando a página 1, e achar onde a engine (ou o
nosso hook de bind) seleciona a página errada.
