# 📦 Empacotando no padrão PortMaster (BYO-data)

O port só vira "jogo na lista" quando empacotado certo. Padrão PortMaster: um `.sh` no topo + uma pasta de dados. O instalador separa os dois.

## 1. Layout no device (NextOS / EmulationStation)
* **Launcher `.sh`** → `/storage/roms/ports_scripts/` (com "s"!). Pôr o `.sh` em `ports/` = **não aparece** no ES.
* **Dados do port** → `/storage/roms/ports/<nome>/` (binário + libs + assets).
* `es_systems.cfg` aponta `path=ports_scripts`; `$directory=roms`; `/roms` é symlink pra `/storage/roms`.
* Depois de copiar o `.sh`: `systemctl restart emustation`.

## 2. BYO-data: o repo NÃO distribui jogo
O repositório traz **só o código/loader**. O usuário fornece o `.so` + assets de um APK que ele **possui legalmente**. Nunca commitar `.so`/binário/assets/dados de jogo (ver `.gitignore`). Um progressor de "APK na pasta" pode extrair e preparar os dados na primeira execução (padrão validado no SOR4: extrai do APK → patcha → comprime ETC1 → apaga o APK).

## 3. Abrir o jogo (foreground, bash puro)
Para o ES exibir na TV, o launcher roda o jogo em **foreground**:
```bash
systemctl stop emustation
bash "/storage/roms/ports_scripts/<Nome>.sh"
```
**Proibido:** `nohup`, `&`, `setsid`, `</dev/null`, `tee` em vfat — qualquer um destaca do VT e dá tela preta no Amlogic-old.

## 4. Controle no launcher
Suba o `gptokeyb` apontando pro `.gptk` do port (ver [controle](06-controle-input-gptokeyb.md)). Não force `SDL_VIDEODRIVER`/`SDL_AUDIODRIVER` (ver [display](09-display-e-sdl-driver.md) e [áudio](05-audio-opensles-sdl.md)).

## 5. Idioma: sempre inglês
Todo port em **inglês**. Se o jogo defaultar pra outro idioma (japonês é o caso comum), force inglês via language type / dados-en / locale. Nunca entregar texto não-inglês.

## 6. Antes de lançar: mate e confirme 0 instâncias
Mate o jogo anterior por `/proc/*/exe` e **confirme 0 instâncias** antes de subir outro — 2 jogos juntos travam o device (ver [memória](07-memoria-vram-e-teardown.md)).

---
*Resumo: `.sh` em ports_scripts, dados em ports/<nome>, BYO-data (nunca commitar jogo), foreground bash puro, gptokeyb, inglês, e mate+confirme antes de lançar.*
