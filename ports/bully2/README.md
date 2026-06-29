# Bully2

Novo port do **Bully: Anniversary Edition** com abordagem original-first.

Escopo inicial:

- alvo primeiro: Mali-450/fbdev;
- loader nativo para `lib/arm64-v8a/libGame.so` Android;
- ciclo Android/JNI reproduzido no Linux;
- `data_0.zip` a `data_4.zip` e `.idx` originais do APK;
- sem cache ETC, sem conversao de textura, sem half-res, sem reduzir streaming;
- `NvAPK` nativo do `libGame.so` por padrao (`BULLY2_NVAPK_MODE=native`);
- despejo nativo por `GameNative.implOnLowMemory` + `CGame::TidyUpTextureMemory`
  por padrao (`BULLY2_EVICT=onlow`, `BULLY2_LOWMEM_TIDYTEX=1`);
- watchdog do launcher ativo por padrao para encerrar o teste antes do device
  travar (`BULLY2_WATCHDOG_MIN_AVAIL_MB=160`,
  `BULLY2_WATCHDOG_MAX_SWAP_MB=64`);
- controle por eventos JNI nativos do jogo.

BYO data: coloque um APK completo do Bully v1.4.311 em `ports/bully2` na primeira execucao. O launcher extrai `libGame.so`, `libc++_shared.so` e `assets/data_*.zip(+.idx)`.

Este port pode usar o `ports/bully` antigo como referencia de fatos, mas a implementacao aqui deve continuar limpa e separada.

Debug pesado fica desligado por padrao. Use `BULLY2_STREAMLOG=1`,
`BULLY2_GLMEMLOG=1` ou `BULLY2_GLLOG=1` apenas para diagnostico curto.
