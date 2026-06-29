# Sonic 4 Episode I — estudo inicial

Data: 2026-06-27

## Entradas estudadas

- APK legal do usuario: `sonic-4-episode-i-v1-5-0.apk`
- Cache legal do usuario: `Sonic-4-episode-I-v1-5-0-cache.zip`

Hashes locais:

```text
APK   410401ffc526f69eceb28a51f78183347f2f8add78ec5040cba3e40c4c4a9c7f
Cache acd6c9fa205cbd75479b46072c4419314b728b996b3efbf40894b5ca692df7b4
```

## Diferença para Sonic 4 Episode II

Episode II usa `libfox.so` e o driver JNI `foxJniLib_*`.

Episode I e outra engine:

- Android/Marmalade/s3e antigo.
- `.so` principal: `lib/armeabi/libs3e_android.so` (ARM 32-bit soft-float ABI).
- Extensoes secundarias:
  - `libs3eAPKExpansion.so`
  - `libs3eDialog.so`
  - `libs3eFlurry.so`
- Payload do jogo: `assets/Sonic4epI.s3e`.
- O `.s3e` e LZMA. Ao descompactar vira container `XE3U` com config Marmalade e codigo/dados do jogo.
- Config interna indica GLES1:
  - `SysGlesVersion = 1`
  - `VirtualRotate = -90`
  - `AndroidExtSo="libs3eDialog.so;libs3eFlurry.so;libs3eAPKExpansion.so"`

## Cache / OBB

Cache ZIP contem:

```text
com.sega.sonic4epi/main.6200011.com.sega.sonic4epi.obb
```

O port deve aceitar cache ZIP ou qualquer `.obb` direto, e instalar como:

```text
data/main.6200011.com.sega.sonic4epi.obb
```

## Bootstrap oficial do repo

O APK antigo usa `lib/armeabi/`, mas `tools/new-port-arm.sh` procura `lib/armeabi-v7a/`.
Para seguir a receita oficial sem alterar o APK original, montei um APK temporario so para bootstrap com as libs em `lib/armeabi-v7a/` e rodei:

```bash
ARM_SO_UTIL="$REPO/template-arm/so_util.c" \
  "$REPO/tools/new-port-arm.sh" \
  "$TMPDIR/sonic4ep1-bootstrap.apk" sonic4ep1_bootstrap
```

Resultado:

```text
.so principal: libs3e_android.so
secundarias: libs3eAPKExpansion.so libs3eDialog.so libs3eFlurry.so
entry: JNI_OnLoad
NEEDED: libz.so libdl.so liblog.so libc.so
151 imports UND
118 auto-resolvidos
33 UNKNOWN
```

O scaffold final `ports/sonic4ep1` foi sincronizado a partir desse bootstrap oficial.
Depois foi ajustado para Marmalade: a principal `libs3e_android.so` deve carregar
antes das extensoes, porque as extensoes importam `s3eEdk*` da principal.

## UNKNOWN iniciais

Os 33 UNKNOWN listados em `src/imports.gen.c` sao majoritariamente libc/bionic antigos que precisam ser cobertos em `src/imports.c` ou pelo fallback glibc:

```text
__google_potentially_blocking_region_begin/end
__sF
arc4random
bsd_signal
chmod
clock_getres
daylight
dlclose/dlerror/dlopen/dlsym
freeaddrinfo/getaddrinfo
fscanf/ftruncate
gethostname/getpeername
gmtime_r/localtime_r
isxdigit
longjmp/setjmp
nice
raise
remove
sigaction
statfs
tmpfile
tzname/tzset
uname
valloc
```

## Proximo passo tecnico

1. Completar `src/imports.c` com os UNKNOWN realmente faltantes.
2. Compilar o F1 do loader multi-modulo.
3. Copiar libs e assets para o Mali-450.
4. Rodar o F1: carregar `libs3e_android.so`, snapshot de simbolos, carregar extensoes, resolver imports e achar `JNI_OnLoad`.
5. So depois implementar a fase F2: fake JNI com `RegisterNatives`, capturar `initNative/runNative/glInit/glSwapBuffers/audio*` e iniciar o container `Sonic4epI.s3e`.

## F1 no Mali-450

Primeiro F1 no device, em `ports/sonic4ep1`:

```text
RC=0
libs3e_android.so carregou
libs3eAPKExpansion.so carregou
libs3eDialog.so carregou
libs3eFlurry.so carregou
```

Ajustes encontrados:

- linkar `-lz` para resolver `inflate*`;
- stubar `__google_potentially_blocking_region_begin/end`;
- salvar `JNI_OnLoad` logo apos carregar `libs3e_android.so`, antes de carregar extensoes, porque `so_util` guarda estado global do ultimo modulo carregado.

## Estado atual no Mali-450

Validado em 2026-06-27 no device `192.168.31.79`.

Funciona:

- logos iniciais aparecem;
- tela `Tap the screen` entra no menu;
- fundo, logo e botoes do menu aparecem;
- audio do titulo/menu/gameplay toca;
- gameplay entra e renderiza Splash Hill Zone Act 1;
- pulo/gameplay por controle funcionam.
- menu inicial, `Help & Options`, `Settings` e pause navegam pelo controle
  fisico.

Fix importante de input:

- `LoaderKeyboard.onKeyEvent(action, keyCode, event)` chama o native como
  `onKeyEventNative(action, unicodeChar, keyCode)`. A ordem antiga herdada de
  outro port estava errada e impedia o foco nativo do menu.
- D-pad/analogico agora navegam o menu pelo foco nativo do Marmalade, com
  seta/overlay ligada por padrao porque o highlight nativo e pouco visivel.
- O confirm do menu nao aceita `AKEYCODE_BUTTON_A`/`DPAD_CENTER` por key event.
  Para selecionar `New Game`, o jogo espera o touch legado do Marmalade:
  `onMotionEvent(pointer, action, x, y)` com `action=4/5`.
- `A`/`Start` no menu usam a selecao atual e enviam esse touch legado nas
  coordenadas do botao selecionado.
- `Help & Options` tem estado separado em grade:
  `How to Play`, `Controls`, `Settings`, `Credits`, `Privacy Policy`,
  `Terms of Service` e `Back`. Cima/baixo/esquerda/direita movem a seta nessa
  grade, `A` toca no item selecionado e `B` volta pelo `Back`.
- `Settings` tem estado proprio: esquerda/direita alternam Music/SFX e tipos
  de controle, `B` volta para `Help & Options` sem perder a navegacao.
- `Credits`, `Privacy Policy` e `Terms of Service` ficam navegaveis pela seta,
  mas o `A` do controle nao abre essas telas. Motivo: `Credits` entra numa tela
  de rolagem sem retorno confiavel por touch/back no so-loader; bloquear pelo
  controle evita prender o jogador. Toque fisico direto ainda e decisao do jogo.
- No gameplay, `Start` toca o botao nativo `Pause` da tela. O menu de pause
  tem seta em `Retry`, `Return to Main Menu` e `Back`; `B`/`Start` forcam
  `Back` para voltar ao gameplay.
- Movimento no gameplay:
  - `A` continua como touch do botao direito de pulo.
  - D-pad/analogicos agora tambem seguram o joystick touch esquerdo:
    `down` no centro do controle virtual e `move` para a direcao.
  - Em gameplay, as direcoes tambem reenviam o `onKeyEventNative` na ordem
    antiga, restrito a fase, para preservar o caminho de controle que fazia o
    Sonic andar antes sem afetar menus/pause.
  - Pulo enquanto anda: como a JNI antiga nao trata o nosso touch como
    multitouch real, quando `A` e pressionado com o joystick esquerdo segurado
    o port solta o touch de movimento por um instante, toca o botao direito de
    pulo e depois deixa o movimento voltar. Tambem envia `BUTTON_A` e
    `DPAD_CENTER` no caminho legacy so durante gameplay.
- Pause `Return to Main Menu`:
  - `Return to Main Menu` abre um estado proprio de confirmacao `Yes/No`.
  - A seta azul/preta tambem aparece nesse modal.
  - Esquerda/direita/cima/baixo alternam `Yes` e `No`; `A` confirma; `B` e
    `Start` forcam `No`.

Testes feitos:

```text
Start -> menu -> A em New Game:
  OK, entra em Splash Hill Zone Act 1 e toca snd_sng_z1a1.mp3.

Start -> menu -> Down -> Up -> A:
  OK, selecao vai 0 -> 1 -> 0 e entra em Splash Hill Zone Act 1.

Menu -> Help & Options:
  OK no controle fisico. Down/Up/Left/Right movem pela grade, A abre Settings
  e Back volta para o menu inicial.

Help & Options -> Settings -> B -> Options -> Credits:
  OK. Settings continua navegavel apos voltar. Credits e ignorado pelo A do
  controle e a seta continua funcional na grade.

Help & Options -> How to Play / Controls -> B:
  OK. Ambos abrem e voltam para a grade.

Gameplay -> Start -> Pause -> Left/Right -> B:
  OK. Start abre o pause por touch nativo no botao Pause, a seta move entre as
  opcoes e B volta para a fase.

Gameplay -> segurar Right:
  OK. Teste automatico segurando direita atravessou o inicio da fase; captura
  local: `/home/nextos/ep1_move_hold_long.png`.

Gameplay -> Right + A:
  Corrigido. Pulo em movimento usa key legacy + touch de pulo com liberacao
  temporaria do touch de movimento.

Pause -> Return to Main Menu:
  Corrigido. Modal `Yes/No` agora tem estado proprio de selecao.
```

Observacao de teste:

- `SONIC4EP1_IGNORE_REAL_INPUT=1` existe apenas para automacao por script,
  impedindo que o controle fisico interfira nos testes. Nao deve ser usado no
  launcher normal.
