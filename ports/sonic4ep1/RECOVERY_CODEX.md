# Sonic 4 Episode I - recuperacao Codex

Estado recuperado do port jogavel validado no aparelho em 2026-06-27/28.

## Binario bom

- Arquivo: `ports/sonic4ep1/sonic4ep1`
- SHA256: `d84c50c586f78dc80ca6bf225e3134389f89f670cd858bc1afb0970bf86e35b7`
- Origem da recuperacao local: `/home/nextos/sonic4ep1-recovery/device-192.168.31.79/sonic4ep1.new`

Esse binario foi o ultimo estado Codex conhecido com logos, audio, video,
menu, Splash Hill Act 1, controles, pause e retorno ao menu funcionando.

## Launcher

O caminho validado usa o runtime nativo do Marmalade/F3:

```sh
SONIC4EP1_RUN_NATIVE=1
SONIC4EP1_EXIT_AFTER_RUN=1
SONIC4EP1_MOVE_ACTION=6
SONIC4EP1_MENU_OVERLAY=1
SONIC4EP1_ARG1="$HERE"
SONIC4EP1_ARG2="$HERE/sonic4ep1.apk"
SONIC4EP1_ARG3="$HERE"
```

Nao usar no launcher normal:

- `SONIC4EP1_IGNORE_REAL_INPUT`: apenas para automacao.
- `SONIC4EP1_HOOK62D90`, `SONIC4EP1_NO_CAPFIX`,
  `SONIC4EP1_FAKE_FILEREG`, `SONIC4EP1_DEPLOY_DIR`: eram testes do caminho
  s3 posterior e nao pertencem ao binario jogavel `d84c50`.

## Falha que foi corrigida

O run quebrado deixou copias extras de `Sonic4epI.s3e` e `Sonic4epI.xe3u`
dentro da arvore do jogo. O runtime aborta com:

```text
Multiple executable files found. Please use ICF file setting [S3E] GameExecutable=x in app.icf to specify which to load
```

O `run.sh` atual move esses duplicados para a pasta irma
`sonic4ep1_codex_hidden` antes de iniciar o jogo. Ele nao apaga os arquivos.

## Validacao 2026-06-29

Teste no aparelho `root@192.168.31.79`:

- Processo `./sonic4ep1` permanece rodando.
- Video renderiza frames no framebuffer.
- Audio abre em 44100 Hz, 2 canais.
- Musicas detectadas no log: `assets/AUDIO/snd_sng_title.mp3` e
  `assets/AUDIO/snd_sng_menu.mp3`.
- Captura salva localmente em
  `/home/nextos/sonic4ep1-recovery/ep1_restore2.png`.
