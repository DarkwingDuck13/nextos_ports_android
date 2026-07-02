#!/bin/sh
# diagnóstico do pipeline EBG (por que o fundo do campo fica preto): LoadEBG/GenAtlas/
# CreateMaterials/CreateScene + spriteCount/atlas. Sem SNDSAFE (crashava). FBMIRROR liga TV.
cd /storage/roms/ff9 || exit 1
export FF9_NOSNDSAFE=1
export FF9_FBMIRROR=1
export FF9_EBGDIAG=1 FF9_ETBLOG=1 FF9_FIELDSTATE=1 FF9_NOLOGGER=1
export FF9_NOSKIPMOVIE=1
exec ./run.sh > /storage/roms/ff9/run_ebg.log 2>&1
