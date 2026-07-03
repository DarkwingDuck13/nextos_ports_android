# PES 2012 Mobile — port Marmalade s3e → Mali-450 (.114)

APK: `~/Downloads/com.konami.pes2012_1.0.5-APK_Award.apk` (internacional, EN — **NÃO** a `jp.konami.pesam`).
OBB: `~/Downloads/com.konami.pes2012.zip` → `main.1000005.com.konami.pes2012.obb` (178MB, package.dz derbh).

## Engine (confirmado)
- `libpes2012.so` (527KB, ARM32 softfp, stripped) = loader Marmalade s3e. Exporta a API s3e inteira
  (s3eDevice*, s3eFile*, s3eDeviceExecPushNext = fibre/stack-switch, s3eDeviceYield, s3eCryptoVerifyRsa).
- `assets/PES2012.s3e` = **LZMA-alone → módulo XE3U 3.24MB** (magic `58 45 33 55`=XE3U, header LZMA `5d 00 00 01 00`, descomp 0x3171a4).
- Idioma: `assets/string/{en,fr,ge,it,sp}.bin` — **inglês**, sem japonês. Locale confirmado runtime = `en_US`.
- GLES1 fixed-function (perfil Mali-450 ótimo).

## Base
Copiado de `ports/sonic4ep1` (que TEM source funcional: main.c 2204 linhas + ep1_audio + so_util + softfp).
Divergência-chave vs Sonic: o PES **exporta os s3eFile\* por símbolo** → hookamos os EXPORTS
(`install_s3efile_exports`), muito mais robusto que os offsets internos do build Sonic.

## Estado (s1 2026-07-03) — BOOT + JNI + runNative LIMPOS, 0 crash
Fluxo validado no device (`SONIC4EP1_RUN_NATIVE=1`):
1. Módulo carrega (so_util ELF32), reloca, resolve. init_array OK. +324 símbolos.
2. **JNI_OnLoad OK**, 31 nativos capturados (initNative, setViewNative, runNative, generateAudio, onKeyEventNative...).
3. 15 s3eFile* exports hookados → nosso fopen. Símbolos-patch (getvm/devexit/debug) OK.
4. **initNative → setViewNative → setPixelsNative(1280x720) → runNative → retorna limpo.** Pega os paths + `getLocale`="en_US".
5. Loop de frames (`runOnOSTickNative`) roda, processo estável.

Mudanças no main.c vs Sonic:
- `SO_NAME`="libpes2012.so"; package "com.konami.pes2012"/1000005; removidos módulos extras Sonic.
- Patches de OFFSET Sonic (surface 0x8330c, caps 0x58d60, icf 0x13c44) gateados atrás de `PES_SONICPATCH` (OFF).
- `runNative` do PES tem **2 args String** (Sonic 3) — o call de 5 args funciona (extras em r0-r3 batem, stack ignorado).

## MURO ATUAL — o módulo do jogo nunca é carregado/entrado
`runNative` (file 0x2d92c) lê args → chama `0x2d39c` (scheduler cooperativo s3e) → **retorna cedo**.
`0x2d39c`: `bl 0x4e8b0` (se r0!=0 early-return r5=1) → `bl 0x245a4` (se !=0 → RUN path `0x2ef88`=exec-pump).
O RUN path só roda se o **main do jogo (fibre) estiver registrado**, mas o módulo **PES2012.s3e (XE3U)
nunca é carregado/descomprimido** → nada registrado → retorna.
- **Zero acessos a arquivo** durante runNative (nem com `app.icf`/`s3e.icf`/`PES2012.s3e` no cwd) → o load
  do exec está atrás do device-init/caps, que sai cedo no so-loader (mesma classe do wall Sonic).

Strings do loader confirmam a lógica alvo: `s3e.icf`/`app.icf`/`gameExecutable` → "The executable specified
in the ICF (%s) could not be found. Searching data folder for executable." → "Multiple executable files found."

## PRÓXIMOS PASSOS (sessão 2)
1. Achar por que `0x2d39c` sai cedo: desmontar `0x4e8b0`/`0x245a4`/`0x2ef88` (o exec-pump) e o device-init
   ANTES do runNative (o s3eMain real). Provável gate de caps/config vindo do icf que nunca carrega.
2. Achar o **module-loader** do .s3e (função que faz LZMA-decomp → XE3U → registra o app main). Grep no loader
   por quem chama a descompressão / lê "gameExecutable" / "Invalid .s3e file".
3. Uma vez no exec-load: nosso `s3eFileListDirectory`/`s3eFileOpen` já servem PES2012.s3e do cwd (data-folder search).
4. Equivalentes PES dos patches Sonic (caps, surface-flush, icf-inject) — re-achar offsets no libpes2012.so.
5. OBB: hook de path (o módulo acha por `%s/Android/obb/%s`) → redirecionar pro OBB real (`~/Downloads/...obb` no device).

## Como rodar (debug)
```sh
cd /storage/roms/ports/pes2012; export HOME=$PWD LD_LIBRARY_PATH=/usr/lib32:$PWD
SONIC4EP1_RUN_NATIVE=1 SONIC4EP1_FILELOG=1 SONIC4EP1_ARG1="$PWD" SONIC4EP1_ARG2="$PWD" ./pes2012
```
Método de debug decisivo = **gdb no device** (`handle SIGSEGV stop nopass; run; bt`).
text_base runtime = `runtime(s3eFileOpen) - 0x4753d`. runNative file off = 0x2d92c.
