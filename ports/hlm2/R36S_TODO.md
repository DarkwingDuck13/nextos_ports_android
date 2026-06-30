# HLM2 → R36S — plano de adaptação

🟡 **EM ANDAMENTO (2026-06-30, R36S ArkOS 192.168.31.150, rg351mp, Mali-G31 Bifrost real driver,
glibc 2.30):** rodando via `hlm2.compat` (build_compat.sh, Docker buster). Boot→disclaimer
(cores cheias, sem bug de alpha)→logos→NOTICE→gameplay, controle nativo OK (Xbox 360 mapping
via GO-Super Gamepad), GL ES3.2 real detectado (driver Mali Bifrost genuíno). **MAS: crash
SIGSEGV 100% reprodutível na PRIMEIRA musica tocada** (não é "deep gameplay" — acontece logo
na tela do NOTICE/skip-violence, é literalmente a 1ª chamada de `scrPlaySong`). Sessão parada
pelo usuário pra retomar com modelo mais forte (Sonnet não tava dando conta do RE).

## ❌ HIPÓTESE DESCARTADA: pthread_attr bionic→glibc
Cheguei a "fixar" `b_pthread_create` em `pthread_bridge.c` (ignorar `attr` bionic ao repassar
pro host) — **MAS esse código é MORTO**, nunca usado: `imports.gen.c:94` resolve o símbolo
`pthread_create` pra `shims.c::pthread_create_fake`, que JÁ ignorava `attr` e JÁ setava o
canario bionic via trampolim, desde a sessão anterior. O fix foi commitado (e8da949) mas não
mudou o comportamento — **crash idêntico antes/depois**, byte a byte mesmo backtrace. Também
testado `HM_NOMUSFIX=1` (desativa o poll-worker das DecodingThread) — **crash idêntico**, ou
seja **o bug NÃO está no nosso hook de música** (poll-worker), está em outro lugar.

## 🔎 ONDE REALMENTE ESTÁ (confirmado por log + disassembly, NÃO consertado ainda)
- Sequência no log: `[mus] >>> scrPlaySong argc=2` → `[asset] open wadtemp/music/detection.ogg`
  (fopen OK, retorna ponteiro válido) → `[shim] stack_chk_fail caller=libyoyo+0x147ceac
  (no-abort)` → `=== CRASH === SIGSEGV Fault addr: 0x218, PC libyoyo.so+0x147cee0`.
- `nm -D` local em `libyoyo.so`: o símbolo mais próximo ANTES de `0x147cee0` é **`ov_read`**
  (em `0x147c9c0`, ou seja crash é **0x520 bytes dentro de `ov_read`**, a função padrão do
  libvorbisfile). `ov_open`=0x147ac48, `ov_open_callbacks`=0x147a860 — todos próximos,
  confirma que é a libvorbisfile ESTATICAMENTE linkada no libyoyo.so.
- `x0=0` no crash, fault addr=0x218 → leitura de campo a offset 0x218 de um ponteiro NULO
  (classe `OggVorbis_File` ou similar, ainda não-inicializada/zerada sendo usada por `ov_read`).
- **Os hooks de diagnóstico verbose (`HM_MUSLOG`) pra `VorbisFileStream::Open`,
  `DecodingTask::Setup/CheckSetup`, `VorbisFileStream::C1` (ctor) NUNCA dispararam** — ou os
  símbolos mangled não bateram (`so_find_addr_safe` falhou silenciosamente) ou esse caminho
  (1ª música) é SÍNCRONO e bypassa inteiramente o DecodingTask/Setup pipeline (ex.: o engine
  pode chamar `ov_open`+`ov_read` direto, sincronamente, pra ler metadata/duração ANTES de
  enfileirar o streaming async — teoria não confirmada).
- Existem símbolos relevantes no `libyoyo.so` (ver `nm -D`): `_ZN12VorbisStream4ReadEP14DecodingBuffer`,
  `_ZN16VorbisFileStream4OpenEv`, `_ZN16VorbisFileStream12PeekMetadataE...`,
  `_ZN12VorbisStream12ReadMetadataEP14OggVorbis_FileP13AssetMetadataPS_` — `ReadMetadata` é
  forte candidato a ser o caminho síncrono que chama `ov_open`/`ov_read` direto.

## 🎯 PRÓXIMOS PASSOS (pra quem pegar a sessão)
1. Confirmar por que os hooks `HM_MUSLOG` não dispararam (testar `so_find_addr_safe` isolado
   pra cada símbolo mangled, comparar com `nm -D` real do `libyoyo.so` — pode ter erro de
   mangling no código).
2. Hookar especificamente `_ZN12VorbisStream12ReadMetadataE...` e `VorbisFileStream::PeekMetadata/OnLoadPeek`
   (suspeitos do caminho síncrono) pra ver se são chamados ANTES do crash e o que retornam.
3. Considerar dump de memória ao redor do `this`/struct usado por `ov_read` no momento do
   crash (registers x1-x8 do crash dump já ficam logados, comparar com layout de
   `OggVorbis_File` do libvorbisfile pra achar QUAL campo a 0x218 é).
4. Mali-450 NUNCA bateu nesse bug rodando o MESMO `libyoyo.so`+`hlm2.apk` — então é algo
   especifico do ambiente R36S (glibc 2.30 vs toolchain LibreELEC, ou diferença de
   timing/arquitetura CPU RK3326 vs Amlogic). Vale comparar binário `hlm2.compat` (usado aqui)
   vs `hlm2` nativo — ambos tem o mesmo bug? (não testado ainda).

Captura de tela neste device: `/dev/fb0` é só o CONSOLE de texto (fbcon), NÃO o scanout real —
o jogo renderiza via DRM/KMS direto (sem compositor rodando). Screenshot real:
`sudo ffmpeg -f kmsgrab -i - -vf "hwdownload,format=bgr0" -frames:v 1 out.png`.

---

Estado anterior (histórico): adiado + R36S cabeado offline (`169.254.170.2`, ArchR, cabo
desconectado). Mali-450/.79 está 100% (vídeo+gameplay+SFX+música+controle+save).

## Device R36S (do memory)
- IP cabeado `169.254.170.2`, user **root**, sem senha (chave id_ed25519). `ssh root@169.254.170.2`.
- **ArchR** (NÃO ROCKNIX), RK3326, **Mali-G31 Bifrost** (suporta ES3 real), 640x480.

## Pontos de adaptação (precedente = Sonic 4 EP2, MESMO R36S)
1. **Contexto GL adaptativo ES3→ES2 na MESMA janela.** O port hoje força EGL ES2 (certo p/ Mali-450
   Utgard). No Mali-G31/mesa-panfrost o ES2 pode dar `EGL_BAD_CONFIG`→tela preta. Padrão Sonic4
   (commit 3ff310b): tenta ES3 1º, cai p/ ES2; logar GL_RENDERER/VERSION/GLSL. Ver `egl_shim.c`.
2. **Áudio adaptativo:** varrer drivers SDL e escolher (ArchR pode usar ALSA, não pulse). Sonic4
   (7e263ad) varre drivers + força `SDL_AUDIODRIVER=alsa` onde necessário. NÃO hardcodar (regra #6).
   O poll-worker da música é independente de device → deve funcionar igual.
3. **Display 640x480:** o port já usa SDL_GetDesktopDisplayMode (adapta sozinho). Conferir no R36S.
4. **Launcher PortMaster:** paths do R36S/ArchR (control.txt) — o launcher atual já cobre os 4 paths.
   Sem gptokeyb (input nativo pela extensão), SELECT+START sai.
5. **glibc compat:** se o binário (toolchain NextOS aarch64) não rodar no ArchR, ver build_compat.sh
   (Docker bullseye glibc) — mesmo padrão do Castlevania SOTN universal.

## 1º passo quando o R36S voltar online
- `ssh root@169.254.170.2 'uname -a; glxinfo|grep -i renderer'` (confirmar Bifrost/ES3).
- Deploy do mesmo set (hlm2+libyoyo.so+assets/) + run → capturar GL_RENDERER + 1º frame.
- Se tela preta: ES3→ES2 adaptive. Se mudo: scan audio driver.
