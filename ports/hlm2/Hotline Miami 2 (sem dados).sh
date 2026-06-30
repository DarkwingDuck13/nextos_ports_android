#!/bin/bash
# PORTMASTER: hlm2.zip, Hotline Miami 2.sh
# Hotline Miami 2: Wrong Number -> NextOS Mali-450 GLES2 (so-loader Android, GameMaker/YYC)
# VERSAO "SEM DADOS": extrai os dados do jogo do hlm2.apk na PRIMEIRA inicializacao.
#   -> Coloque o seu hlm2.apk (Hotline Miami 2 v0.8.1, ARM64) em ports/hlm2/ e rode.
# INPUT NATIVO via a extensao do jogo (getControllerValue le o estado SDL do controle direto).
# SEM gptokeyb. Saida = SELECT+START (nativo no binario).

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/hlm2"
gamedir="$GAMEDIR"

cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

$ESUDO chmod 666 /dev/tty0 2>/dev/null
printf "\033c" > /dev/tty0

# ---- PRIMEIRA INICIALIZACAO: extrai libyoyo.so + assets/ do hlm2.apk ----
if [ ! -f "$GAMEDIR/.data_ok" ]; then
  APK="$GAMEDIR/hlm2.apk"
  if [ ! -f "$APK" ]; then
    echo "Coloque hlm2.apk em ports/hlm2/ e rode de novo." > /dev/tty0
    echo "(Hotline Miami 2: Wrong Number v0.8.1, APK ARM64)" > /dev/tty0
    sleep 6
    exit 1
  fi
  echo "Extraindo dados do jogo (primeira vez, ~1-2 min)..." > /dev/tty0
  cd "$GAMEDIR"
  # libyoyo.so (codigo do jogo) de lib/arm64-v8a/
  unzip -oq "$APK" "lib/arm64-v8a/libyoyo.so" && mv -f lib/arm64-v8a/libyoyo.so libyoyo.so && rm -rf lib
  # assets/ (game.droid + musica wad + localizacao + meta)
  unzip -oq "$APK" "assets/*"
  if [ -f "$GAMEDIR/libyoyo.so" ] && [ -f "$GAMEDIR/assets/game.droid" ]; then
    touch "$GAMEDIR/.data_ok"
    echo "Dados extraidos com sucesso." > /dev/tty0
  else
    echo "ERRO na extracao (hlm2.apk corrompido ou unzip ausente)." > /dev/tty0
    sleep 6
    exit 1
  fi
fi

echo "Loading Hotline Miami 2... Please Wait." > /dev/tty0

chmod +x "$GAMEDIR/hlm2"
"$GAMEDIR/hlm2"

# o binario faz teardown EGL ao sair (SELECT+START/SIGTERM); kill -TERM garante limpeza
# (senao trava o Mali fbdev entre runs).
$ESUDO kill -TERM "$(pidof hlm2)" 2>/dev/null
sleep 1
$ESUDO systemctl restart oga_events 2>/dev/null &
printf "\033c" > /dev/tty0
exit 0
