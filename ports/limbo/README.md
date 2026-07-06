# limbo — NextOS Android port

Port gerado por `nextos_ports_android`. `.so`: `libLimbo.so`.

## Game files (BYO — você fornece, do seu APK legítimo)
- `libLimbo.so` (de lib/arm64-v8a/)
- assets / OBB conforme o jogo

## Estado
- [x] Boot NativeActivity standalone
- [x] EGL/SDL render em Mali, 1024x576 interno
- [x] Pacotes `limbo_android_boot.pkg` e `limbo_android_runtime.pkg`
- [x] Cena `data/levels/limbo.scene` carregando ate `GAME ON`
- [x] Tela inicial renderizada; screenshot em `/dev/shm/limbo_shot.raw`
- [x] Controle: `Select` alterna cursor touch; com cursor desligado os controles passam como gamepad normal
- [ ] Audio real Wwise/OpenSL; por enquanto bancos sao bypassados para nao travar o boot
- [x] Watchdog opcional no loader: `LIMBO_MAX_SECONDS=N` encerra com status 124 depois de N segundos, util para testes no device quando o jogo travar o sistema.

## Testado
- Device: `192.168.31.114`
- Destino: `/storage/roms/ports/limbo`
- APK base usado: `/home/felipe/Downloads/limbo-mod_1.26-an1.com.apk`

## Handoff
- Alvo unico autorizado: `192.168.31.114`. Nao trocar de device.
- Antes de nova mudanca: estudar o binario/caminho de audio e comparar somente com ports 100% jogaveis.
- Referencias boas para audio/shim: `chrono`, `sotn`, `sonicmania`, `sonic4`, `bully`, `secretofmana`, `codboz`, conforme engine/caminho usado.
- Proximo foco: audio real Wwise/OpenSL. Verificar se o runtime esta escolhendo AAudio ou OpenSL, e so entao trocar patch/hook. Evitar copiar solucao de port incompleto.
- Para testes remotos, sempre usar watchdog (`LIMBO_MAX_SECONDS`) e coletar logs antes de deixar loop longo rodando.
