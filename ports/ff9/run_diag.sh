#!/bin/sh
# diag do evento de abertura: skip-movie (iteração rápida), evento NATURAL (sem forcecontrol),
# LOGTHROW (exceções reais: KeyNotFound 136 etc.) + ETBLOG (script pede diálogo?) + FIELDSTATE.
cd /storage/roms/ff9 || exit 1
export FF9_FORCECONTROL=0
export FF9_LOGTHROW=1 FF9_ETBLOG=1 FF9_FIELDSTATE=1 FF9_NOLOGGER=1 FF9_LOGFMT=1
export FF9_GPLOG=1 FF9_BUBBLELOG=1
exec ./run.sh > /storage/roms/ff9/run_diag.log 2>&1
