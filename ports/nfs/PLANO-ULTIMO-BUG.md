# NFS MW — TELA do DISCLAIMER (splash MOST WANTED): RESOLVIDA 2026-06-15 (pin no atlas de boot)

Device **192.168.31.164** (subnet .31, senha nextos). Port em `~/nextos_ports_android/ports/nfs/`.
Build `./build.sh`. Deploy `scp nfs root@192.168.31.164:/storage/roms/nfs/nfs` (matar nfs antes).

## ✅ RESULTADO (commit desta sessão)
A 4ª tela (splash "© 2018 Electronic Arts Inc.") tinha a **parte de cima toda fora de ordem**
(caixa preta + formas-fantasma). Agora renderiza **COMPLETA e correta**: logo NEED FOR SPEED MOST
WANTED + a arte real (Porsche 911 branco + fileira de carros pretos + skyline) + o texto legal + o
**spinner de loading em anel "O"**. As **logos iniciais** (EA/Firemonkeys/MOST WANTED) continuam
perfeitas. Validado por screenshot (glReadPixels→PIL), frames 210 (logo) e 360/420 (splash).

## 🔬 CAUSA-RAIZ (RE + medição, não chute)
1. **Por que tex=0:** a fn de bind da engine (libapp.so off `0x56960c`) faz `r5=*r5` (objeto de
   textura do sprite) e liga **glBindTexture(target, 0)** quando `r5==null`. Logos E splash são
   draws com objeto NULL → tex=0 (link sprite→.sba resolve null no port = PARTE 11). Achado com
   **NFS_BINDLOG** (return-address gateado por frame g_disc_frame).
2. **Por que o splash dava lixo mas as logos não** (medido com NFS_TEXLOG mostrando o frame de cada
   upload de atlas): o **atlas de boot tex=1** (510x1003) sobe no **frame 0** e contém TODAS as
   logos + a arte do splash. As logos desenham nos frames ~40-273 quando tex=1 é o ÚNICO atlas →
   o "último atlas global" = tex=1 = CERTO. No **frame ~281** sobem os atlases do MENU (tex 10-19);
   o splash desenha no frame ~300 e o "último atlas global" virou tex=19 (menu) = ERRADO → topo
   fora de ordem.

## 🔑 O FIX (egl_shim.c atlas_rebind)
Para draws UI com tex=0, em vez do "último atlas global" passou a **fixar no PRIMEIRO atlas grande**
(`g_first_atlas_tex` = atlas de boot tex=1, gravado no 1º upload is_atlas). Logos E splash usam
tex=1 → ambos corretos. O menu usa texturas reais (tex≠0) e nem passa por aqui.
- `g_first_atlas_tex` setado em my_glTexImage2D no 1º atlas (w≥256,h≥256,w≠h,RGBA).
- Envs diagnóstico: `NFS_LASTATLAS=1` = comportamento antigo (último global, p/ reproduzir o bug);
  `NFS_FORCEWHITE=1` = textura 1x1 branca (limpa mas sem a arte); `NFS_NOATLASHACK=1` = desliga.
  `NFS_BINDLOG=1` = loga bind/draw tex=0 por frame; `NFS_TEXLOG=1` = loga uploads de atlas c/ frame.

## ⚠️ PONTO DE ATENÇÃO p/ outras telas
O pin no atlas de BOOT vale enquanto o conteúdo dos draws tex=0 estiver no tex=1 (boot+splash).
Se em alguma tela posterior um draw tex=0 precisar de OUTRO atlas, o pin erraria — mas o menu/jogo
usa texturas reais (tex≠0) e não depende disto (validado: menu/mapa renderiza ótimo). Se aparecer
regressão em alguma tela nova, reavaliar (talvez per-programa ou per-screen).

## ➡️ PRÓXIMO (se quiser): fontes do menu que quebram após ~4-5 seções
Mesma raiz (objeto de textura do glyph vira null → tex=0). Hoje, com o pin, glyph quebrado pegaria
o atlas de boot (provavelmente errado p/ glyph). Investigar com NFS_BINDLOG já no menu (gatear por
frame mais alto). Fix real = re-bindar/re-upload a textura do glyph na evicção.

## FERRAMENTAS
- Screenshot: `NFS_AUTOSHOT=1 NFS_SEQSHOT=1` → `seq_NNNN.raw` (a cada 30 frames) em /storage/roms/nfs.
  Decodificar local: `~/nfs-diag/decode.py seq_*.raw` (RGBA 1280x720 + flip). Logo≈frame 210, splash≈360.
- RE: objdump ARM `~/NextOS-Elite-Edition/build*Amlogic-old*/toolchain/bin/armv8a-emuelec-linux-gnueabihf-objdump`
  (libapp.so é ARM, NÃO Thumb). libapp base = `so_load: load base` (a do range que contém o ra).
- ⚠️ MASCARAR emustation; vfat /storage/roms vira RO se martelado → `mount -o remount,rw /storage/roms`.
  /tmp é tmpfs 416MB — NÃO acumular .raw lá (puxar de /storage/roms/nfs direto).
