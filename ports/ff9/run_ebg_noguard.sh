#!/bin/sh
# testa o EBG REAL (sem FIELDGUARD): deixa CreateSceneCombined/CreateSeparateSprites construírem
# os meshes do background de verdade. EBGDIAG loga created/comb por overlay.
cd /storage/roms/ff9 || exit 1
export FF9_NOSNDSAFE=1 FF9_FBMIRROR=1
export FF9_NOFIELDGUARD=1
export FF9_EBGDIAG=1 FF9_FIELDSTATE=1 FF9_NOLOGGER=1 FF9_NOSKIPMOVIE=1
exec ./run.sh > /storage/roms/ff9/run_ebg_noguard.log 2>&1
