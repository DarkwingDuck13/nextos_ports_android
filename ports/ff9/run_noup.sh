#!/bin/sh
# força path NÃO-upscaled do EBG (useUpscaleFM=0): GenerateAtlasFromBinary empacota sprites do
# binário -> spriteCount>0 -> geometria do background -> renderiza (SD, mas visível vs preto).
cd /storage/roms/ff9 || exit 1
export FF9_NOSNDSAFE=1 FF9_FBMIRROR=1
export FF9_EBGDIAG=1 FF9_EBG_NOUPFM=1
export FF9_FIELDSTATE=1 FF9_NOLOGGER=1 FF9_NOSKIPMOVIE=1
exec ./run.sh > /storage/roms/ff9/run_noup.log 2>&1
