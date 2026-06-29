#!/bin/bash
# Launcher do Sonic 4 Episode I (Mali-450 Amlogic .79, fbdev).
# Receita de deploy RECUPERADA 2026-06-28 (ver STATUS.md). Display/áudio
# AUTOMÁTICOS do sistema (regra #6: nunca forçar SDL_VIDEODRIVER/AUDIODRIVER).
cd "$(dirname "$0")" || exit 1
HERE="$(pwd)"

# 1) rom:// = assets/ do APK extraído (Sonic4epI.s3e + AUDIO/ + splash).
if [ ! -f romfs/assets/Sonic4epI.s3e ] && [ -f sonic4ep1.apk ]; then
  rm -rf romfs; mkdir -p romfs
  unzip -o -q sonic4ep1.apk 'assets/*' -d romfs
fi

# 2) UM ÚNICO .s3e no cwd (senão "Multiple config settings found").
rm -f sonic4epi.s3e romfs/assets/sonic4epi.s3e 2>/dev/null
[ -f Sonic4epI.xe3u ] && { mkdir -p stash; mv -f Sonic4epI.xe3u stash/ 2>/dev/null; }
# 3) sem s3e.icf/app.icf externos (o .s3e tem ICF embutido).
[ -f s3e.icf ] || [ -f app.icf ] && { mkdir -p icf_bak; mv -f s3e.icf app.icf romfs/assets/s3e.icf romfs/assets/app.icf icf_bak/ 2>/dev/null; }

export SONIC4EP1_RUN_NATIVE=1
export SONIC4EP1_DEPLOY_DIR="$HERE/romfs/assets"
export SONIC4EP1_ARENA=1        # OBRIGATÓRIO (isola heap do engine; senão corrompe glibc)
export SONIC4EP1_RESOLVE=1
# s2 2026-06-28: config que AVANÇA MAIS (open+read do .s3e funcionam):
export SONIC4EP1_HOOK62D90=1    # open interno do .s3e -> nosso fopen (mata "Can't open")
export SONIC4EP1_SURFOBJ=1      # surface object via hook do TLS getter 0x82d60
export LD_LIBRARY_PATH="/usr/lib32:$LD_LIBRARY_PATH"

exec ./sonic4ep1 "$@"
