# sor4 — Streets of Rage 4 (port NextOS)

Port de **Streets of Rage 4** (APK Android v1.4.5, engine **MonoGame/.NET 9**) para device
Mali-450 (NextOS). Caminho **gerenciado**: runtime .NET 9 CoreCLR self-contained + MonoGame
buildado do source em **GLES2 nativo** (host próprio `sor4host` substitui a MainActivity).
NÃO é so-loader. Texturas ASTC→**ETC1**; áudio Wwise tocado por reimpl OpenAL.

## Game files (BYO — você fornece, do seu APK legítimo)
Você põe **o APK do SOR4 v1.4.5** na pasta do port; na 1ª execução o *progressor* extrai
tudo do APK (texturas, banks Wwise, `libWwise`, e o `SOR4.dll` do assembly-store XABA/LZ4),
patcha, converte pra ETC1 e apaga o APK. Ver `port/package/sor4/README.md`.

## Estado
Menu + gameplay rodando; áudio (música/SFX) validado. Ver `HANDOFF.md` e `HANDOFF-AUDIO.md`.

## Estrutura
- `build/` — trabalho no host (assemblies extraídos, runtime, host). NÃO versionado (grande).
- `port/`  — launcher `.sh`, shims, config p/ o device.
