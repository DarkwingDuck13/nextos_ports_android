# Licencas — Streets of Rage 4 (port)

Este pacote NAO contem nenhum dado do jogo. O usuario fornece a sua propria copia
legal do APK (Streets of Rage 4 1.4.5) e o launcher extrai os dados dele na 1a
execucao (BYO-data). Abaixo, a procedencia e licenca de cada componente.

## Jogo (NAO incluso — BYO-data)
"Streets of Rage 4" (c) DotEmu / Guard Crush Games / Lizardcube / SEGA. Todas as
marcas, texturas, audio (.wem/.bnk), fontes e o codigo do jogo (`SOR4.dll`) sao
propriedade dos respectivos donos e NAO sao redistribuidos aqui — vem do APK do
proprio usuario e o APK e apagado apos a extracao.

## Codigo do port (felc18-blip) — Apache-2.0
O host .NET (`sor4host`), os patches do MonoGame (GLES2), o reimpl de audio
(`libWwise.so` = wrapper + OpenAL/opusfile), o decoder/encoder de textura
(`libsor4astc.so`, ASTC->RGBA/ETC1, codigo proprio baseado no spec ASTC aberto da
Khronos) e as ferramentas Cecil de patch sao do porter (felc18-blip), liberados sob
Apache-2.0, derivados do framework nextos_ports_android.

## Wwise (NAO incluso — BYO, proprietario)
`libWwise.real.so` e o Audiokinetic Wwise runtime, (c) Audiokinetic Inc.,
PROPRIETARIO. Nao e redistribuido: e extraido do APK do usuario. A nossa
`libWwise.so` apenas o carrega para a logica e toca o som via OpenAL/opusfile.

## Runtime .NET e BCL bundlados — MIT
O runtime .NET 9 CoreCLR e a biblioteca base (`System.*.dll`, `Microsoft.*.dll`,
`lib*System*.so`, `libcoreclr*` etc.) sao (c) .NET Foundation and Contributors, sob
a licenca MIT. https://github.com/dotnet/runtime/blob/main/LICENSE.TXT
(texto completo em `MIT.txt`).

## MonoGame.Framework.dll — Ms-PL + MIT
(c) The MonoGame Team. Licenca Microsoft Public License (Ms-PL); partes derivadas do
XNA/Microsoft sob MIT. https://github.com/MonoGame/MonoGame/blob/develop/LICENSE.txt
(texto completo em `MonoGame-Ms-PL.txt`).

## Mono.Cecil.dll — MIT
(c) Jb Evain e contribuidores. MIT. https://github.com/jbevain/cecil

## SharpFont.Core.dll — MIT
(c) Robert Rouhani. MIT. https://github.com/Robmaister/SharpFont
(faz P/Invoke na FreeType do sistema do device — nao bundlada).

## SDL2 (`libSDL2-2.0.so*`) — zlib
(c) Sam Lantinga. Licenca zlib. https://www.libsdl.org/
(texto completo em `SDL2-zlib.txt`.) Em alguns devices o launcher usa a libSDL2 do
proprio CFW; nesse caso a libSDL2 e a do sistema.

## Audio do sistema (NAO bundlado — carregado via dlopen do device)
- OpenAL Soft (`libopenal.so.1`) — LGPL-2.1. https://openal-soft.org/
- opusfile / libopus / libogg (`libopusfile.so.0`) — BSD-3-Clause, (c) Xiph.Org.
  https://opus-codec.org/
Sao as bibliotecas do proprio CFW/device; o port nao as redistribui.

## LZ4 (build/conversao — `texconv`) — BSD-2-Clause
Codec LZ4 (lz4net) (c) Yann Collet / Milosz Krajewski, usado so na extracao do
assembly-store do APK. https://github.com/lz4/lz4

## Ferramentas de extracao (`tools/`)
- `tools/progressor` + `tools/common.src`: metodo de janela/progresso da comunidade
  PortMaster (reaproveitado do port "TMNT: Shredder's Revenge"). Creditos aos autores
  do PortMaster.
- `tools/FiraCode-Regular.ttf`: (c) The Fira Code Project Authors, SIL Open Font
  License 1.1 (OFL). https://github.com/tonsky/FiraCode
- gptokeyb (opcional, `SOR4_USE_GPTK=1`): GPL-2.0, fornecido pelo CFW (nao bundlado).
