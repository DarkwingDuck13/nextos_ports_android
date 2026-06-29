# Sonic 4 EP2 - STATUS

## Sessao 2026-06-29 (FIX: pulo PAUSA no Special Stage)
- **Bug (reportado pelo luis, v3.3 nao corrigia):** no Special Stage o PULO (A) tambem pausa.
  Nos atos normais o pulo funciona. Causa-raiz por disasm do libfox.so:
  - `OUYAGetPauseKey()` (0x3bd904) = `mov r0,#0x8000; bx lr` -> retorna a mascara **0x8000**.
  - TODO checador de pausa (`gmMainStartDemoEndCheck`, `SsUserInputIsPause`, `CPauseMenu::play`,
    `GmPauseMenuGetResult`) faz: `OUYAGetPauseKey()|0x4000` -> testa `pad & 0xC000`.
  - 0x8000 e o MESMO bit do confirm de menu (`FOX_A_MENU=0x8020`, AoPadSomeoneStand). Logo,
    confirm-de-menu e pausa sao indistinguiveis: A com 0x8000 SEMPRE pausa um estado pausavel.
  - Por que so no special stage: `sonic_game_started` e heuristica de log. Atos normais carregam
    por `mapfar` e logam `--- GmGameDatLoadExit ---` -> started=1 -> A=FOX_A_GAME(0x20), sem pausa.
    Special stage carrega por **CSSLoadingTask/GmSpStage_Start** (player GmPlayerSpStage_*, dados
    SpStage01..08), caminho SEPARADO que NAO loga GmGameDatLoadExit -> started fica 0 (vindo de
    "Create World Map") -> A=FOX_A_MENU(0x8020) -> 0x8000 -> pad&0xC000 -> PAUSA no pulo.
- **Fix (imports.c, `sonic_update_gameplay_state_from_log`):** `strcasestr(msg,"spstage")` ->
  sonic_game_started=1. Casa os loads `--- Load Start <...SpStage0X...> ---` (mesmo logger foxLog
  ja confirmado em runtime) + "SPSTAGE BRANCH"/"SpStage Loading"; NAO casa a UI "Special Stage"
  (tem espaco). Acesso ao special stage em EP2 = Red Star Ring (hasRedStarRing/getEmeraldIndex).
- **Estado:** build native (208804, md5 1d8dab51) deployado em .79 dev (`/storage/roms/ports/sonic4`)
  E package (`/storage/roms/ports/sonic4ep2`). Regressao OK: boota 1280x720, chega no gameplay,
  60fps, 0 crash, ZERO falso-positivo "special stage:" no boot/menu/ato normal. Backups na .79:
  sonic4.bak_prespstage. **FALTA: confirmacao on-screen (entrar num special stage via RSR e pular)
  — nao automatizei (RSR + navegacao no world map). Compat glibc2.27 p/ release: rebuildar apos OK.**

## Sessao 2026-06-28 (R36S/ROCKNIX + perf + special-stage audio)
- **Special stage audio FIX (confirmado pelo NextOS):** cues SpStage01-08 (speed/spike/entrada)
  estavam unmapped (so SpStage04 existia) -> mudos; anel/BGM tocavam. Mapeados via AudioDataTbl
  do DEX (dexlib2) com a CAIXA correta do OBB (V maiusculo). +Env01. sfx_map.tsv (runtime). Commit 9ba8f16.
- **Performance:** GPU nao era o gargalo (titulo/gameplay carregado = 60fps); lag real = stalls de
  I/O (streaming do SD, multi-seg) + memoria. Levers aplicados: SONIC_LOWFX (bloom off, ~+15% GPU,
  sem perda visivel; commit 0e5260c) + swap 512MB + SD readahead 4096 (servico systemd persistente
  no ROCKNIX). Bench zone1: avg 27->35 fps (~+30%). Downscale interno NAO feito (mexe na nitidez).
- **R36S agora roda ROCKNIX** (Arch-R panfrost travava o boot; ROCKNIX painel4 via overlay mipi-panel.dtbo
  = tela ok com libmali; panfrost descartado nesse painel). Acesso: root@169.254.170.2 senha rocknix
  (regenera no boot). Build aarch64 (X5M 64-bit puro) = WIP (crash pos-init_array).
- Release: Sonic4EP2 PortMaster v3.2.zip (port.json v5, bin LOWFX e59671d6, audio fix). BYO-data.
- ⚠️ NUNCA rodar harness com `rm foxsave` (apaguei save do NextOS em bench; scripts removidos).

# Sonic 4 EP2 - STATUS (PortMaster/R36S/Mali-450, 2026-06-27)

## Estado atual
O port passa pelo fluxo principal no device `.79`:

- logo/titulo/menu aparecem;
- Start entra no jogo;
- gameplay renderiza com fundo, HUD, Sonic/Tails e cenario;
- BGM funciona no titulo, menu e primeira fase;
- SFX comuns funcionam pelo caminho nativo `AudioHelper` -> SDL audio;
- audio aprovado pela validacao NextOS: menu, gameplay, pulo, mola e sons comuns fluem sem engasgo;
- performance do gameplay corrigida: depois de remover o sleep fixo do loop, gameplay fica em 60 FPS estavel;
- Start/Pause dentro do gameplay abre corretamente e foi aprovado pela validacao NextOS;
- `SELECT+START` foi corrigido no `main.c`, testado no device e aprovado pela validacao NextOS;
- save/continue voltou para o caminho nativo: com save bom, Start nao reinicia Act 1 direto e segue para o
  fluxo de mapa/continue esperado;
- save real validado: foi concluida a primeira fase, o jogo foi fechado/reaberto e apareceu `Continue`.
- fix aplicado para transicoes longas/boss: `sonic_game_started` agora tambem reconhece
  `GmGameDatLoadExit`, `Gimmick set camera scale` e `GmPlySeq`, evitando que A/B sejam tratados
  como entrada de menu depois de carregar fase por continue/boss;
- log normal foi reduzido: `ALOG`, `[frame]`, `[PERF]` e `[MEMCPY-NULL]` ficam desativados por
  padrao e voltam apenas com flags de diagnostico (`SONIC_VERBOSE_LOG`, `SONIC_ALOG`,
  `SONIC_FRAMELOG`, `SONIC_PERFLOG`, `SONIC_MEMCPYLOG`).

Device principal Mali-450 usado: `192.168.31.79`.
Device R36S usado: `169.254.170.2`.
Diretorio de desenvolvimento/testes antigos no device: `/storage/roms/ports/sonic4`.
Diretorio final do pacote no device: `/storage/roms/ports/sonic4ep2`.
Launcher final do pacote: `Sonic4EP2.sh` na raiz do zip, para extrair direto em `roms/ports`.

## Pacote PortMaster v1
Pacote pronto no desktop local:

```sh
Sonic4EP2 PortMaster v1.zip
```

Conteudo do zip:

- `Sonic4EP2.sh`;
- `sonic4ep2/sonic4` binario armhf compat;
- `sonic4ep2/sonic4.gptk`;
- `sonic4ep2/port.json`;
- `sonic4ep2/gameinfo.xml`;
- `sonic4ep2/cover.png` (cover nova do Sonic/Tails);
- `sonic4ep2/screenshot.png`;
- `sonic4ep2/splash.png`;
- `sonic4ep2/sfx_map.tsv`;
- `sonic4ep2/tools/extract-sonic4-data.sh`;
- `sonic4ep2/tools/sonic4ep2_extract.src`;
- `sonic4ep2/tools/progressor`;
- `sonic4ep2/README.md`;
- `sonic4ep2/LICENSE.md`;
- `sonic4ep2/Sonic4ep2.f2f`.

O zip nao inclui `runtime/`, APK, OBB ou arquivos comerciais do jogo.
Zip final validado: md5 `29081438be1b3d3896ee9498c4af7407`, sha256
`68e5ecabb44ab1104a5d25a56c82fb2eabb2e6374d8ee8cc3ff46d5772bcf982`, tamanho `2.4M`.
O zip foi validado sem entradas `ports/` ou `ports_scripts/`.

Build compat:

```sh
cd ports/sonic4
SR=<toolchain-sysroot>
sudo -n docker run --rm --platform linux/amd64 -v "$PWD":/repo -v "$SR":/sysroot:ro debian:buster bash /repo/build_compat_gcc.sh
cp -f sonic4.compat.gcc package/ports/sonic4ep2/sonic4
cd package/ports
zip -r "<output-dir>/Sonic4EP2 PortMaster v1.zip" Sonic4EP2.sh sonic4ep2
```

Resultado do build Docker:

- `sonic4.compat.gcc` / `package/ports/sonic4ep2/sonic4`;
- tamanho: `165196` bytes;
- sha256 validado no pacote: `02d137428af9c63f2941ae0b94a97a3e8b14d5bdea4bb351ba0b6a94cf41f3ea`;
- maior simbolo glibc: `GLIBC_2.27`, abaixo do alvo glibc 2.30;
- sem dependencia `GLIBCXX`;
- dependencias dinamicas esperadas no device: `libSDL2-2.0.so.0`, `libmpg123.so.0`,
  `libvorbisfile.so.3`, `libGLESv2.so`, `libdl.so.2`, `libm.so.6`, `libpthread.so.0`,
  `libc.so.6`.

Starter limpo:

- mata qualquer `sonic4` antigo pelo alvo real de `/proc/*/exe` antes de iniciar;
- fica como `Sonic4EP2.sh` na raiz do zip, padrao PortMaster classico igual Bully;
- usa `GAMEDIR="/$directory/ports/sonic4ep2"`, sem hardcode de `/storage/roms` no launcher;
- extrai `lib/armeabi-v7a/libfox.so` do APK na primeira execucao;
- instala `data/main.22.com.sega.sonic4episode2.obb` a partir do cache ZIP ou OBB direto;
- usa `progressor`/`tools/sonic4ep2_extract.src` para janela de primeira extracao quando disponivel;
- usa `SONIC_DATADIR="$GAMEDIR"`;
- usa `SONIC_AUTOSTART=0`;
- usa `SONIC_NOFAKESOUND=1`;
- usa `SONIC_SWAPINT=1`;
- no R36S/Wayland detecta resolucao real via `wlr-randr` ou `/sys/class/drm` e exporta `SONIC_RES`;
- no Mali-450/fbdev nao exporta `SONIC_RES`, mantendo o caminho SDL/fbdev ja validado;
- nao usa autoplay, `AUTORIGHT`, `AUTOJUMP`, `INPUTLOG` ou flags de teste.

Validacao R36S (`169.254.170.2`):

- device usa `essway.service` ativo, `es-de.service` inativo;
- painel real confirmado: `/sys/class/drm/card0-DSI-1/modes = 640x480`;
- launcher novo detectou `SONIC_RES=640x480`;
- log confirmou `setScreenSize(640 x 480)` e `fox: init(w=640 h=480)`;
- teste pelo ES confirmou tela correta e audio OK;
- pacote final copiado para `/storage/roms/ports/Sonic4EP2_PortMaster_v1.zip`;
- zip final extraido em `/storage/roms/ports`, criando `/storage/roms/ports/Sonic4EP2.sh`
  e `/storage/roms/ports/sonic4ep2/`;
- estado deixado para primeira execucao: `lib/` e `data/` removidos, `foxsave_0.dat`
  removido, APK e OBB na raiz de `sonic4ep2/`;
- backup preservado em `/storage/roms/ports/sonic4ep2/_backup_input/`;
- nenhum processo `sonic4` rodando ao final.

Validacao Mali-450 (`192.168.31.79`):

- instalacao anterior preservada como `/storage/roms/ports/sonic4.devbackup_20260626_183504`;
- zip final copiado para `/storage/roms/Sonic4EP2_PortMaster_v1.zip`;
- teste fbdev com `emustation.service` parado temporariamente e religado ao final;
- log confirmou que no Mali-450 nao houve override R36S: `setScreenSize(1280 x 720)` e
  `fox: init(w=1280 h=720)`;
- jogo entrou no loop principal, renderizou frames e ficou em torno de 60 FPS;
- `emustation.service` voltou `active`; `es-de.service` permaneceu `inactive`;
- nenhum processo `sonic4` rodando ao final.

Validacao limpa anterior no `.79`:

- APK copiado para `/storage/roms/ports/sonic4ep2/sonic-the-hedgehog-4-episode-ii-2.0.0.apk`;
- OBB direto copiado para `/storage/roms/ports/sonic4ep2/main.22.com.sega.sonic4episode2.obb`;
- backup de inputs preservado em `/storage/roms/ports/sonic4ep2/_backup_input/`;
- primeiro launch extraiu `libfox.so` e OBB automaticamente;
- `libfox.so` extraida com md5 `d77489abf54523046ade35425e537782`;
- OBB extraida com md5 `3b8ad5c461014bd94cf227982d49c664`;
- primeiro boot do layout final chegou em `[frame 0]`, criou `foxsave_0.dat` novo e estabilizou em 60 FPS;
- segundo boot nao reextraiu dados, carregou `foxsave_0.dat` com sucesso e estabilizou em 60 FPS;
- apos encerrar, varredura de `/proc` retornou `no_sonic_running`.
- `progressor` corrigido e smoke-testado depois da extracao: saiu `child exited with code 0`, sem apagar APK/OBB restaurados;

Regra operacional do device:

- o frontend correto para devolver a tela ao usuario e `emustation.service`;
- nao iniciar, parar, reiniciar ou alterar `es-de.service` neste fluxo.

## Audio implementado
O caminho real do Sonic 4 EP2 nao e OpenSL neste port. A `libfox.so` chama a ponte Java
`com/mineloader/fox/AudioHelper`:

- `MusicSetDataSource(id, key)`;
- `MusicStart(id)`, `MusicStop(id)`, `MusicPause(id)`, `MusicVolume(id, volume)`;
- `PlaySound(key, volume, arg)`;
- `PauseSound(handle)`, `ResumeSound(handle)`, `StopSound(handle)`, `SetVolume(handle, volume)`;
- `spReset/mpReset`;
- `asyncBuildSpData(...)`, `asyncBuildBgmData(...)`;
- `isDoneBuildSp`, `isDoneBuildBgm`, `GetMusicState`.

O shim JNI encaminha essas chamadas para `src/sonic_audio.c`.

`src/sonic_audio.c`:

- abre audio SDL2 em 44100 Hz stereo S16;
- usa os leitores nativos do proprio `libfox.so` (`tsReadFile`) para buscar arquivos dentro do LPK/OBB;
- decodifica MP3 com `libmpg123`;
- decodifica OGG com `libvorbisfile`;
- mistura BGM e SFX em callback SDL;
- cacheia ate 256 buffers decodificados;
- trata SFX observados como one-shot, porque o terceiro argumento de `PlaySound` veio como `2/3/-1`
  mesmo para sons que nao podem loopar;
- replica o comportamento nativo do `AudioHelper`: `MusicSetDataSource`/`MusicStart` da mesma faixa
  ativa nao reiniciam a posicao, e o loop vem da tabela nativa `AudioDataTbl` (`loopflag`,
  `loopStart`, `loopEnd`) em vez de tratar toda musica como loop total;
- trata jingles de evento como camada sobre o BGM, mas preserva os loops nativos de invencibilidade
  e Super Sonic; jingles curtos (`emerald`, `1up`, `clear`, etc.) ficam one-shot;
- corrige o mapa nativo de bosses: `boss2=FinalA1`, `boss3=MetalSonic`, `boss5=StardustSPDWY`,
  `boss6=DEmk2`;
- aplica headroom/soft limiter no mixer e reaproveita SFX mecanicos ja ativos para ficar mais proximo
  do comportamento do `SoundPool` Android e evitar clipping em mecanismos repetitivos;
- aceita `SONIC_SFX_OVERRIDE`, por exemplo `Jump=S4EP2FX_024_S1C2_44.OGG`, para testar trocas sem
  recompilar;
- carrega `sfx_map.tsv` em runtime, com 644 entradas normalizadas pelo manifesto real do OBB, e usa
  o banco atual vindo de `asyncBuildSpData` (`ep2zone1`, `ep2zone2`, etc.).

## Fonte oficial do mapa SFX
O mapa certo veio do DEX do APK, nao de chute pelo nome dos arquivos:

- APK: `sonic-the-hedgehog-4-episode-ii-2.0.0.apk`;
- DEX extraido: `/tmp/sonic4-classes2.dex`;
- classe: `com/mineloader/fox/AudioDataTbl`;
- dump local: `/tmp/sonic4-dex-sfx-map.tsv`;
- resultado: 772 linhas, 288 cues unicos, 10 cues conflitantes por banco/zona.

Correcoes importantes ja aplicadas em `g_sfx_map`:

- `Ok` -> `S4EP2FX_001_SHSY08_22.OGG`;
- `Pause` -> `S4EP2FX_004_SHSY10_22.OGG`;
- `Jump` -> `S4EP2FX_009_SK62_44.OGG`;
- `Enemy` -> `S4EP2FX_017a_S2_3441_44.OGG`;
- `Spring` -> `S4EP2FX_067_SKB1_44.OGG`;
- `Ring1L` -> `S4EP2FX_112a_S2_2235_44L.OGG`;
- `Ring1R` -> `S4EP2FX_113a_S2_2235_44R.OGG`;
- `LockedOn` -> `S4EP2FX_144a_COLORS_OBJ_LOCKON_22.OGG`.

Detalhe maior em `SFX_MAP.md`.

## Validacao feita
Build local:

```sh
cd ports/sonic4
bash build.sh
```

Deploy feito para `.79` com `scp`.

Regra obrigatoria de launch:

- usar `run.sh` para abrir o jogo no device;
- usar `runsonic.sh N` para teste com tempo/screenshot;
- usar `stop.sh` quando precisar apenas fechar o Sonic sem abrir outro;
- ambos matam qualquer processo existente cujo `/proc/*/exe` seja
  `/storage/roms/ports/sonic4/sonic4*` antes de iniciar outro, incluindo processo antigo com binario
  deletado;
- nao iniciar manualmente com `nohup ./sonic4 ... &` sem rodar a mesma varredura primeiro.
- `SELECT+START` fecha o jogo no binario, igual Bully/Dysmantle/Katana Zero; no Sonic o check tambem
  roda no `main.c`, porque esse loop consome os eventos SDL antes do `android_shim`.

Teste bom de gameplay/SFX:

```sh
cd /storage/roms/ports/sonic4
SONIC_EXTRA='SONIC_NOFAKESOUND=1 SONIC_AUDIOLOG=1 SONIC_AUTORIGHT_AFTER=1150 SONIC_AUTOJUMP_AT=1240 SONIC_INPUTLOG=1' sh ./runsonic.sh 150
```

Artefatos desse teste:

- log: `/tmp/sonic4-input-a-game1.log`;
- screenshot: `/tmp/sonic4-input-a-game1.png`;
- screenshot mostra gameplay com fundo, HUD, Sonic/Tails, nao o menu de pause falso antigo;
- nenhum `unmapped sfx` no trecho validado;
- contagem observada: `Spring` 70, `Ring1L` 5, `Ring1R` 4, `Ok` 2, `Jump` 2, `LockedOn` 1.

Teste aprovado pela validacao NextOS:

- log: `/tmp/sonic4-audio-final-ok.log`;
- audio sem engasgos;
- som da mola repetindo e correto, porque a rota automatica fica quicando em cima da mola;
- som considerado finalizado.

Performance:

- problema raiz: `main.c` tinha `usleep(16000)` fixo apos o present;
- como o swap ja espera vsync, isso criava double pacing e deixava menu 3D/gameplay em camera lenta;
- agora o sleep default e `0`; se precisar testar, usar `SONIC_FRAME_SLEEP_US=N`;
- log bom: `/tmp/sonic4-perf-perfect.log`;
- gameplay apos carregar estabiliza em `[PERF] fps=60.0 avg=16.7ms`.

Start/Pause:

- tentativa antiga por input bruto `FOX_START` nao abria o menu;
- o caminho atual chama a sequencia nativa `GmPauseMenuLoadStart` -> `GmPauseMenuBuildStart` ->
  `GmPauseMenuStart`;
- NextOS confirmou que Start/Pause ficou perfeito;
- o run de teste manual atual foi iniciado no `.79` sem `AUTOPAUSE`, sem `AUTORIGHT`, sem `AUTOJUMP`
  e sem `SONIC_INPUTLOG`, mantendo apenas o `AUTOSTART` inicial do launcher para entrar no jogo.

Save/continue:

- nao forcar mais `_Z18GsUserIsSaveEnablem`; esse flag precisa refletir o load real do save;
- o setup nativo carrega `foxsave_0.dat`, mas no port o caminho observado nao reconstruia sempre os dados
  globais de progresso antes do menu;
- `main.c` agora espera `AoStorageLoadIsFinished()` + `AoStorageLoadIsSuccessed()` e chama
  `DmBuildSysDataFromBackup()` uma vez, seguido de `gs::backup::utility::UpdateStageUnlockState()`;
- validado no device com o save bom: log mostrou `stage 0 clear=1`, `stage 1 unlocked=1` e Start parou de
  cair direto em `G_ZONE1`/Act 1 novo;
- save bom preservado no device como `foxsave_0.dat.codex_keep_20260626_173044`; nao restaurar o backup
  `before_native_load_20260626_175036`, porque ele representava estado quase novo.
- validacao final em jogo real: apos concluir a primeira fase, `foxsave_0.dat` ficou com md5
  `81be5f6252b533ad0cc1024e0b7074e5`; depois de `stop.sh` + `run.sh`, o menu mostrou `Continue`.

Protocolo recomendado para validar save limpo:

```sh
cd /storage/roms/ports/sonic4
cp -p foxsave_0.dat foxsave_0.dat.before_clean_save_test_$(date +%Y%m%d_%H%M%S) 2>/dev/null || true
rm -f foxsave_0.dat
sh ./run.sh
```

Depois passar uma fase inteira. Em seguida fechar pelo guard (`sh ./stop.sh`) e reabrir com
`sh ./run.sh`: Start deve entrar no mapa/continue, nao reiniciar a primeira fase do zero.

## Pendencias atuais
Ordem recomendada:

1. Testar controles reais por mais tempo no pacote v1 limpo.
2. Validar rotas mais longas de gameplay para item box, inimigo/matar bicho, damage,
   spin/dash/homing, gimmicks de zona e coop/Tails. Audio base ja esta aprovado.
3. Depois do teste manual, revisar qualquer visual/audio restante que aparecer fora da primeira rota.
