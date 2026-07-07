# AGENTS.md — guia para agentes (Codex/Claude) neste repositório

Este arquivo é a **memória compartilhada** para qualquer agente de IA trabalhando
no `nextos_ports_android`. Leia antes de editar/commitar.

## O que é este repo

Framework para **rodar jogos Android (o `.so` nativo do APK) em Linux ARM64** (e,
em breve, **x86_64/PC**) via **so-loader**: carrega o ELF do Android e resolve os
imports com implementações nativas (libc/GLES/pthread) + shims que fingem ser
Android (JNI, OpenSL ES→SDL2, EGL→SDL2, bionic→glibc). Alvo principal: **Mali-450
(Utgard)** e handhelds (R36S, X5M, muOS, ROCKNIX). **Não** é emulação: código ARM
roda no ARM do device.

## 🚫 Regras inegociáveis

1. **BYO-data — NUNCA commitar dado de jogo.** Nada de `.so`/`.apk`/`.obb`/`.dat`/
   `.assets`/`sharedassets*`/`global-metadata.dat`/binários de loader compilados/
   `.dex`/`.db`/fontes de app/`shared_prefs`/dumps de RE/`.tar` de device. O repo
   tem **só código/loader/docs**. O `.gitignore` cobre a maioria — se `git add -An`
   listar qualquer binário/asset, PARE e ajuste o ignore. (Copyright + é público.)
2. **Commit direto no `master`, commits pequenos e por port.** Um commit por port/
   assunto. Nada de branch a não ser que peçam.
3. **Sem co-autoria de IA nos commits.** Nada de "Co-Authored-By"/"Generated with".
   Autor = a config git do dono do repo.
4. **Sem dados pessoais em texto público** (commit/README/docs). O projeto se chama
   **NextOS**.
5. **Créditos obrigatórios** (licenças de terceiros): mtojek (Apache-2.0, base),
   initdream (Crazy Taxi), Producdevity (CoD BOZ, MIT). Mantenha `NOTICE`.
6. **Binário LIMPO, `.sh` LIMPO, SELECT+START que VOLTA pro ES.** Todo port
   entregue tem que sair de verdade. NUNCA deixe watchdog/heartbeat, selftest de
   input, framedump/dump de framebuffer, ou spam de `[GLDRAW]/[GLSTATE]` ligado
   por padrão no binário final; e NUNCA um launcher que para/mascara o
   emustation. Ver a seção "Armadilha: launcher e saída" abaixo — quebrar isso
   trava o device e obriga correção manual.

## Estrutura

- `template/` + `template-arm/` — base por-jogo (copiada a cada port).
- `tools/new-port.sh` — bootstrap de um port a partir de um APK/.so.
- `facilitando_o_trabalho/` — base de conhecimento:
  - `kit_essencial/core/` — peças reutilizáveis (`so_util`, `egl_shim`,
    `pthread_bridge`, **`nx_jni`** = JNI por tabela).
  - `receitas/` — 16 receitas (ver abaixo).
  - `troubleshooting/` — crash, deadlock.
- `ports/<jogo>/` — cada port.

## Como portar (fluxo atual)

1. `tools/new-port.sh <apk|so> <nome>` → esqueleto compilável + `imports.gen.c`.
2. Resolver símbolos `UNKNOWN` em `imports.gen.c` (stub o que não bloqueia o boot).
3. **JNI por tabela** (`nx_jni`, receita 14) em vez de `switch` manual.
4. Ajustar render (EGL→SDL2, GLES1 vs GLES2), áudio (OpenSL→SDL2), input (gptokeyb).
5. Empacotar estilo PortMaster (control.txt + gptokeyb) — só pra lançar.

## Direções ativas do framework (roadmap)

- **`nx_jni` — JNI por tabela** (`kit_essencial/core/nx_jni.{h,c}`, receita 14):
  dispatch dirigido por dados. Adotar em ports novos; migrar antigos aos poucos.
- **Loaders genéricos por engine** (receita 15): 1 binário roda N jogos de uma
  engine (GameMaker/Cocos2d-x) via `game.cfg`. Unity fica por-port (metadata único).
- **Alvo PC/x86_64 e multiarch** (receita 16): `so_util` precisa de `R_X86_64_*` +
  `hook_x86_64` + TLS `%fs`; `new-port.sh` deve extrair `lib/x86_64`. Ganho: debugar
  no desktop (gdb/asan) antes do device.
- **Config data-driven** (`game.cfg`): tirar do binário os `*_TEX_*`/`gfxargs`/keymap.

## Receitas (índice)

01 iniciando · 02 pthread/ABI · 03 Mali-450 · 04 fake JNI · 05 áudio · 06 input/
gptokeyb · 07 VRAM/teardown · 08 texturas ETC1/ETC2 · 09 display/SDL · 10 empacotar
· 11 ponteiros/hooks · 12 Unity bootstrap/render/GC · 13 Unity guia-mestre ·
**14 JNI por tabela (nx_jni)** · **15 loaders genéricos por engine** ·
**16 alvo PC/x86_64 e multiarch**.

## Notas de plataforma

- **Unity IL2CPP** é o muro mais difícil (metadata/usages). A cena Vita/Switch marca
  Unity como "não-portável" — aqui já há vários rodando; é trunfo nosso, não copie
  deles nesse ponto.
- **ABI**: cuidado com structs bionic dependentes de ponteiro/arch (`sigaction`,
  `dirent`, `pthread_*`). Layout errado = crash silencioso (lição do cvgos: bionic
  LP32 tem handler ANTES da mask; layout ARM64 num armv7 pôs a mask como handler).
- **`so_util`** é o coração: relocs, GOT, `init_array`, hooks. Multiarch mexe aqui.

## Referências externas (cena so-loader)

- TheFloW (criador, Vita) · Rinnegatamante (`so_util`, `yoyoloader_vita`, vitaGL) ·
  v-atamanenko (`FalsoJNI`, `soloader-boilerplate`) · NaGaa95 (Switch) · mtojek
  (Linux ARM64, nossa base). Úteis como referência de destraves por jogo — os
  stubs de JNI e receitas costumam transferir.

## ⚠️ Armadilha: npot_fix herdado do Dysmantle (Mickey/porta pretos no COI)

O `imports.c` do Dysmantle traz `my_glTexParameteri` com "npot_fix" DEFAULT ON que
força `WRAP_S/T=CLAMP_TO_EDGE` e min-filter mipmap→`LINEAR` em TODA textura. Isso é
CORRETO para o Dysmantle (texturas NPOT), mas ERRADO como default herdado: qualquer
jogo cujos materiais usam UV espelhado/repetido (personagens skinned, planos
`*_mirror_*`, adornos) passa a amostrar só os texels da BORDA do atlas → objeto com
forma perfeita e albedo coerente porém errado/escuro ("personagem preto", "porta
preta"), com luz e uniforms perfeitos. Diagnóstico que fecha a questão em ~3 runs
(ver `ports/castleofillusion/src/imports.c`, probes `COI_CHARFS`):
1. `COI_CHARFS=light` (fragment = luz total) → personagem branco = luz OK;
2. `COI_TEXDUMP=1` + decodificar o payload ETC1 no host → atlas OK = conteúdo OK;
3. sobra estado do sampler → testar `DYSMANTLE_NPOT_OFF=1` (no COI o default já foi
   invertido: religar só com `DYSMANTLE_NPOT_FIX=1`).
REGRA ao scaffoldar de Dysmantle: npot_fix começa OFF; só ligue se o jogo tiver
textura NPOT real com artefato comprovado.

## ⚠️ Armadilha: launcher e saída (SELECT+START travava o device)

Sintoma real (BADLAND + Magic Rampage, ports feitos por Codex, tiveram que ser
corrigidos à mão): apertar SELECT+START pra fechar TRAVAVA o jogo e não voltava
pro EmulationStation; em alguns casos o ES não subia mais nem depois de reboot.
Duas causas, as duas proibidas:

1. **Launcher parando/mascarando o frontend.** O `.sh` fazia
   `systemctl stop emustation` e às vezes `systemctl mask --runtime`. O `mask`
   **sobrevive ao fim do script** → `systemctl start emustation` passa a dar
   "Unit is masked" e o ES nunca mais volta. **NUNCA** mexa no emustation no
   launcher. O contrato do ES é: ele lança o `.sh` de `ports_scripts/` em
   FOREGROUND e volta sozinho quando o script termina.

2. **Shutdown "gracioso" que pendura.** As threads do `.so` carregado (FMOD,
   workers do Cocos/engine) **nunca terminam**; chamar `nativeOnStop`/
   `GS_destroy`/`SDL_Quit`/`atexit` no caminho de saída pendura segurando GPU/
   áudio e a tela fica congelada. Saída correta: dispara uma thread-deadline
   (`sleep 2; _exit(0)`), faz o mínimo (pausar áudio, soltar GL) e chama
   `_exit(0)` — o processo morre garantido mesmo se o engine travar.

**Padrão de launcher aprovado** (igual ninjago/LEGO — copie este esqueleto):
```sh
#!/bin/bash
# resolve controlfolder (PortMaster) ... source control.txt; get_controls
GAMEDIR="/storage/roms/ports/<nome>"; cd "$GAMEDIR"
export HOME="$GAMEDIR"
export LD_LIBRARY_PATH="/usr/lib:$GAMEDIR:$LD_LIBRARY_PATH"
for s in /var/run/pulse/native /run/pulse/native; do
  [ -S "$s" ] && { export PULSE_SERVER="unix:$s"; break; }; done
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
./<binario> "$GAMEDIR" >"$GAMEDIR/debug.log" 2>&1
```
Regras do `.sh`: **nada** de `systemctl`, `pkill emustation`, watchdog,
heartbeat, `nohup`/`&`/`setsid`, nem `SDL_VIDEODRIVER`/`SDL_AUDIODRIVER`
forçado (vêm do sistema). Foreground puro, log em `debug.log`.

**Loop principal e saída no binário** (o combo SELECT+START = BACK+START):
```c
if (gc_btn(SDL_CONTROLLER_BUTTON_BACK) && gc_btn(SDL_CONTROLLER_BUTTON_START))
  running = 0;
...
// fim do loop:
pthread_t d; if (pthread_create(&d,0,exit_deadline_thread,0)==0) pthread_detach(d);
/* pausa áudio + solta GL se quiser */ _exit(0);
```

**Antes de entregar um port, remova do binário final:** watchdog/heartbeat,
`*_INPUT_SELFTEST`, `dump_framebuffer`/`log_frame_pixels`, e deixe o trace
GL/assets **silencioso por padrão** (atrás de um env tipo `<JOGO>_VERBOSE=1`).
Debug fica atrás de env, nunca ligado no default.

**Como validar a saída sem apertar botão:** `kill -TERM <pid>` no binário
percorre o MESMO caminho do SELECT+START (vira `SDL_QUIT`). Rode o jogo por
ssh, confirme render na tela (`dd if=/dev/fb0`), mande SIGTERM, e cheque que o
processo morreu e o launcher retornou 0. Só então empacote.
