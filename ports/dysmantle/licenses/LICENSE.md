# Licencas — DYSMANTLE (port)

## Codigo do port (loader / shims)
O so-loader (so_util, egl/opensles/jni/android shims, fixpak/etc2_decode) deriva do
framework nextos_ports_android, baseado nos ports so-loader de mtojek (Apache-2.0).
Codigo do porter (felc18-blip) liberado sob a mesma Apache-2.0.

## Jogo
"DYSMANTLE" e (c) 10tons Ltd. ESTE PACOTE NAO CONTEM NENHUM DADO DO JOGO. O usuario
precisa fornecer sua propria copia legal do APK (DYSMANTLE 1.4.1.12); o launcher extrai
os dados dele na 1a execucao (BYO-data).

## Janela de extracao (tools/)
A ferramenta de extracao com janela/progresso `tools/progressor` e o helper
`tools/common.src` vem do port "TMNT: Shredder's Revenge" do PortMaster (metodo de
extracao da comunidade PortMaster, reaproveitado). Creditos aos autores do PortMaster /
do port do TMNT.

A fonte `tools/FiraCode-Regular.ttf` e (c) The Fira Code Project Authors, sob a SIL Open
Font License 1.1 (OFL) — https://github.com/tonsky/FiraCode (uso e redistribuicao
permitidos pela OFL; a fonte nao e vendida isoladamente).

## Bibliotecas
SDL2, EGL/GLESv2, libz/libturbojpeg etc. sao as do proprio CFW/device (nao bundladas).
A `libc++_shared.so` (runtime NDK) acompanha o pacote.
