# Crazy Taxi Classic (com.sega.CrazyTaxi) -> Mali-450 fbdev

so-loader. Fonte do loader: https://github.com/initdream/crazytaxi-aarch64
(feito em cima do nosso framework nextos_ports_android). É aarch64.

## Estado (2026-06-18) — JOGÁVEL
🟢🏆 Render (Mali-450 GLES2, 1280x720, 30fps) + SOM (pulse 44100/2ch) +
CONTROLE via gptokeyb funcionando (scanCodes 21/22/103/108 confirmados no log,
"Unpause brute-force complete!", menu de pause abriu com START). Aberto pela ES
na TV. Falta: empacotar PortMaster; jogar de verdade e ajustar .gptk se
algum botão estiver trocado.

## ⚠️ Device / como abrir (NÃO esquecer)
- Device = o mesmo do Terraria (Amlogic-old, Gxbb/S905, Mali-450 fbdev). IP MUDA
  por DHCP (.89→.88 em 18/06). Achar: ping-sweep na subrede local + `ls
  /storage/roms/ports/crazytaxi`. Host key muda no reboot → `ssh-keygen -R <ip>`.
- ABRIR SÓ ASSIM: `systemctl stop emustation; bash "/storage/roms/ports_scripts/
  Crazy Taxi Classic.sh"` em foreground PURO (do meu Bash tool: run_in_background).
  NUNCA >/dev/null, &, nohup, setsid, tee→vfat (destacam do VT → tela preta).
- Se TUDO preto (jogo e menu): `emustation` está MASKED → `systemctl unmask
  emustation && systemctl start emustation` (a ES precisa rodar p/ inicializar o
  display HDMI; só depois o jogo renderiza).
- fbgrab/dd fb0 por SSH NÃO reflete a tela neste device → validar OLHANDO A TV.

## Controle (gptokeyb)
main.c mapeia TECLADO→keycode Android (map_sdl_key_to_android) porque o gptokeyb
agarra o js0 e emite TECLADO (não SDL GameController). crazytaxi.gptk usa só
tokens válidos do /usr/bin/gptokeyb do device: a=space, b=leftctrl, x=leftshift,
y=leftalt, l1=rightshift, r1=rightctrl, start=enter, back=backspace, dpad=setas.
Autotest: CT_AUTOTEST=1 injeta SDL_KEYDOWN space (BUTTON_A) p/ validar via SSH.

## Como buildar
`./build.sh` — toolchain NextOS Amlogic-old aarch64
(aarch64-libreelec-linux-gnu-gcc), linka SDL2 + libMali (GLESv2/EGL fbdev) do
sysroot. Flags extra: -Wno-int-conversion (gcc novo trata como erro;
so_find_addr retorna uintptr_t atribuído a ponteiros de função).

## Layout no device
- /storage/roms/ports/crazytaxi/  (vfat, 84G livre)
  - crazytaxi (binario), libgl2jni.so, assets/ (301M), run.sh (teste SSH)
- /storage/roms/ports_scripts/Crazy Taxi Classic.sh  (launcher ES)

## Extração dos dados (do .apks/SAI bundle Crazy_Taxi_Classic_6.0)
- libgl2jni.so: split_config.arm64_v8a.apk  (lib/arm64-v8a/)
- assets/: merge de base.apk (assets/) + split_packs.apk (assets/data/... 260M)

## Lições / armadilhas
- ⚠️ NAO logar via tee no vfat: o engine loga MILHARES de linhas
  (AAssetManager_open de cada asset + spam f2fextension) e o tee no vfat
  lento TRAVA o boot (>45s sem chegar no título). run.sh/launcher mandam
  stdout pra /dev/null por padrão; CT_LOG=1 grava log.txt pra depurar.
- Video: SDL_VIDEODRIVER=mali (EGL fbdev). fb0 reflete o Mali -> fbgrab funciona.
- Audio: SDL_AUDIODRIVER=pulse (alsa falha: "Couldn't set audio channels").
  Aviso de symlink pulse (HOME em vfat) é benigno, conecta pelo socket do sistema.
- Pausa/unpause: o engine pausa no boot (PAUSE_INTRO_VIDEO/GAME_SERVICE etc).
  jni_shim.c spawna unpause_thread quando o engine chama playIntroVideo:
  faz brute-force activeGame(true, 1..255) p/ f2f e CT, limpando todos os
  reasons. O "spam PAUSE... failed not contain reason" É o brute-force (no-op
  nos reasons não setados), não re-pause por frame.
- useTouch=0 e isPremiumUser=1 são patchados em main.c.

## TODO
- [ ] validar input/gameplay com gamepad (entra menu, dirige)
- [ ] empacotar PortMaster zip (.sh topo + pasta) p/ BYO-data ou distribuir
- [ ] checar perf (30fps cap pq GPU < GLES 3.1) e áudio em jogo
