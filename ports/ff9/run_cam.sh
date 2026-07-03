#!/bin/sh
# rodada CAMDIAG: menu -> New Game (via tergp) -> campo, com raio-X de câmeras/EBG.
# skip-movie default (iteração rápida). Log: run_cam.log
cd /storage/roms/ff9 || exit 1
export FF9_CAMDIAG=1 FF9_EBGDIAG=1 FF9_NOSKIPMOVIE=1
# sobrevive ao log-crash nativo do il2cpp no boot (loteria: %s lixo no logger 0x10a6200)
export FF9_SKIPLOGCRASH=1
# menu/jogo visível na TV: espelha metade de cima -> metade de baixo do fb0 double-buffer
export FF9_FBMIRROR=1
export FF9_FIELDSTATE=1 FF9_NOLOGGER=1
exec ./run.sh > /storage/roms/ff9/run_cam.log 2>&1
