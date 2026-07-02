#!/bin/sh
# fluxo base validado (Codex): menu+audio real+controle, SEM meu SNDSAFE (que crashou por
# runtime_invoke do FieldSoundDispatch com singleton nulo). A KeyNotFound 136 é capturada pela
# própria engine (não-fatal). FBMIRROR corrige o "menu escuro na TV" (metade errada do fb0).
cd /storage/roms/ff9 || exit 1
export FF9_NOSNDSAFE=1
export FF9_FBMIRROR=1 FF9_FBBRIGHT=170
export FF9_ETBLOG=1 FF9_FIELDSTATE=1 FF9_NOLOGGER=1
export FF9_NOSKIPMOVIE=1
exec ./run.sh > /storage/roms/ff9/run_play.log 2>&1
