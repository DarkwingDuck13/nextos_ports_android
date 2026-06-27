# Sonic 4 EP2 - STATUS (audio/performance/pause, 2026-06-26)

## Estado atual
O port passa pelo fluxo principal no device `.79`:

- logo/titulo/menu aparecem;
- Start entra no jogo;
- gameplay renderiza com fundo, HUD, Sonic/Tails e cenario;
- BGM funciona no titulo, menu e primeira fase;
- SFX comuns funcionam pelo caminho nativo `AudioHelper` -> SDL audio;
- audio aprovado pelo NextOS: menu, gameplay, pulo, mola e sons comuns fluem sem engasgo;
- performance do gameplay corrigida: depois de remover o sleep fixo do loop, gameplay fica em 60 FPS estavel;
- Start/Pause dentro do gameplay abre corretamente e foi aprovado pelo NextOS;
- `SELECT+START` foi corrigido no `main.c`, testado no device e aprovado pelo NextOS;
- save/continue voltou para o caminho nativo: com save bom, Start nao reinicia Act 1 direto e segue para o
  fluxo de mapa/continue esperado;
- save real validado: NextOS passou a primeira fase, o jogo foi fechado/reaberto e apareceu `Continue`.

Device usado: `192.168.31.79`.
Diretorio no device: `/storage/roms/ports/sonic4`.
Binario atualizado no device: `/storage/roms/ports/sonic4/sonic4`.

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
- aceita `SONIC_SFX_OVERRIDE`, por exemplo `Jump=S4EP2FX_024_S1C2_44.OGG`, para testar trocas sem
  recompilar;
- carrega `sfx_map.tsv` em runtime, com 644 entradas normalizadas pelo manifesto real do OBB, e usa
  o banco atual vindo de `asyncBuildSpData` (`ep2zone1`, `ep2zone2`, etc.).

## Fonte oficial do mapa SFX
O mapa certo veio do DEX do APK, nao de chute pelo nome dos arquivos:

- APK: `/home/nextos/Downloads/sonic-the-hedgehog-4-episode-ii-2.0.0.apk`;
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
cd /home/nextos/nextos_ports_android/ports/sonic4
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

Teste aprovado pelo NextOS:

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

Depois o NextOS passa uma fase inteira. Em seguida fechar pelo guard (`sh ./stop.sh`) e reabrir com
`sh ./run.sh`: Start deve entrar no mapa/continue, nao reiniciar a primeira fase do zero.

## Pendencias atuais
Ordem recomendada:

1. Montar pacote PortMaster/NextOS final com starter limpo e dados extraidos do APK/OBB.
2. NextOS testar controles reais por mais tempo no run limpo atual.
3. Validar rotas mais longas de gameplay para item box, inimigo/matar bicho, damage,
   spin/dash/homing, gimmicks de zona e coop/Tails. Audio base ja esta aprovado.
4. Depois do teste manual, revisar qualquer visual/audio restante que aparecer fora da primeira rota.
