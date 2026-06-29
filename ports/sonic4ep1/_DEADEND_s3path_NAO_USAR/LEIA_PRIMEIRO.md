# 🚫 BURACO PROIBIDO — caminho s3-path (NÃO USAR)

Caí nele 2x e custou um dia inteiro do NextOS. NUNCA mais.

## O que é o "buraco"
O caminho **s3-path** = binário `sonic4ep1.compat.gcc` + flags
`SONIC4EP1_HOOK62D90 / FAKE_FILEREG / NO_CAPFIX / TLSMAP / ROMDRIVE / APKEXP`.
Foi uma exploração que tenta carregar o módulo .s3e "na marra" com hooks.

## Por que é PROIBIDO
**NUNCA RENDERIZA o jogo em device NENHUM** (Mali nem R36S). Boota, entra no
loop de frames, mas a tela fica **PRETA pra sempre** (o File subsystem fake +
hooks impedem o game de ler `resources.dz` de verdade). Não é "quase lá", é beco
sem saída. Já testado exaustivamente (80 frames/58s = preto constante no R36S).

## O ÚNICO caminho que funciona
O **binário bom `d84c50`** (`ports/sonic4ep1/sonic4ep1`, dentro do
`Sonic4EP1 PortMaster v3.10.zip`) RENDERIZA PERFEITO no Mali — Splash Hill
jogável, áudio, controle. Usa flags REAIS: `RUN_NATIVE + OBB/OBB_DIR +
FORCE_FILEINIT_OK + S3EKEY + FIX_NAN_MATRIX + GLES/RES + AUTOTAP`. Receita =
`run.sh` (recovery). **Source do binário bom foi PERDIDO** (só temos o binário).

## R36S: adaptar o BINÁRIO BOM, não o s3-path
Único bloqueio do binário bom no R36S = barreira CP15 (`mcr p15` ilegal no
kernel RK3326, CP15BEN=0). A engine `mcr` (v3.10) PASSA do surface e só dá SIGILL
no barrier. Fix = emular CP15 (libcp15emul.so) OU achar barreira legal. NÃO
mexer no s3-path. Detalhe: a engine `dmb` (.r36s) passa o barrier mas crasha no
surface (comportamento à parte).
