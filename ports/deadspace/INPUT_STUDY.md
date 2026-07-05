# Dead Space - estudo de input nativo

Data: 2026-07-05
Device alvo unico: `192.168.31.114`

## Resumo

O port e possivel e ja passou do maior bloqueio: imagem, assets, ETC1, audio e entrada no
gameplay foram validados. O problema atual nao parece ser "Dead Space nao aceita controle".
O problema e que a camada de input atual mistura tres rotas:

- keycodes Android via `KeyboardAndroid_NativeOnKeyDown/Up`;
- touch virtual no modulo `TouchScreen`;
- eventos SDL recebidos por callback/evento.

Essa mistura funcionou o bastante para entrar no jogo, mas explica os bugs atuais:

- menu vertical falha/intermitente;
- botoes A/B/X/Y nao desbloqueiam tudo;
- analogico direito move a camera rapido e volta para frente;
- D-pad e sticks podem competir com a mesma rota de touch.

O caminho correto e aproximar o loader do hardware que o APK original anuncia: teclado fisico
/ Xperia Play + touchscreen + touchpad. O binario do jogo tem codigo para TouchPad/Xperia,
mas nosso JNI hoje provavelmente informa que nao existe TouchPad.

## Referencias estudadas

### LEGO Star Wars TCS

Referencia forte: port funcional usado como base de engenharia de controle.

Achados uteis:

- O port nao depende de eventos de botao SDL isolados.
- A cada frame chama `SDL_GameControllerUpdate()`.
- Le todos os botoes com `SDL_GameControllerGetButton()`.
- Le eixos com `SDL_GameControllerGetAxis()`.
- Mantem estado anterior e envia key down/up so quando muda.
- Comentario do proprio port: polling por frame e mais confiavel em builds SDL/Trimui onde
  eventos de controle podem falhar.

O que nao da para copiar direto:

- LEGO Star Wars exporta `nativeUpdateGamepadAxisValues(...)`.
- Dead Space nao exporta API equivalente de gamepad/eixos.

Uso correto para Dead Space:

- Copiar o padrao de polling por frame.
- Nao copiar a API de eixo nativa, porque ela nao existe no Dead Space.

### Syberia

Referencia util de loader Android nativo.

Achados uteis:

- Usa fake NativeActivity/AInputQueue para engine Android/NDK.
- Faz SDL GameController virar eventos Android de baixo nivel.

O que nao da para copiar direto:

- Dead Space atual nao esta rodando pelo caminho `AInputQueue`.
- Dead Space esta chamando exports JNI Java-style da engine EA Blast.

Uso correto para Dead Space:

- Usar como referencia de filosofia: alimentar a engine pelo caminho Android esperado.
- Nao migrar cegamente para `android_shim.c`, porque esse arquivo existe no port mas nao esta
  no caminho principal atual.

### LEGO Batman

Referencia apenas negativa/parcial.

Motivo:

- O proprio status marca gameplay, audio e outros pontos como incompletos.
- AGENTS.md exige usar apenas ports 100% funcionais como base de decisao para novo port.

Uso correto para Dead Space:

- Nao usar como base.
- No maximo usar como aviso sobre perigo de achar que menu funcionando significa gameplay
  completo funcionando.

## Evidencia no APK/binario Dead Space

### ABI

O APK `dead-space-b1200.apk` tem somente ABI `armeabi`. Nao ha `arm64-v8a`.

Conclusao:

- Nao existe ARMv8 nativo dentro deste APK.
- O caminho e AArch32/32-bit em device ARMv8 multilib.

### Exports JNI importantes

O `libDeadSpace.so` exporta:

- `Java_com_ea_blast_KeyboardAndroid_NativeOnKeyDown`
- `Java_com_ea_blast_KeyboardAndroid_NativeOnKeyUp`
- `Java_com_ea_blast_TouchSurfaceAndroid_NativeOnPointerEvent`
- `Java_com_ea_blast_ModuleCatalog_NativeGetModuleTypeIdPhysicalKeyboard`
- `Java_com_ea_blast_ModuleCatalog_NativeGetModuleTypeIdTouchScreen`
- `Java_com_ea_blast_ModuleCatalog_NativeGetModuleTypeIdTouchPad`
- raw pointer down/move/up/cancel getters.

Nao foi encontrado export estilo:

- `nativeUpdateGamepadAxisValues`
- `nativeControllerSetData`
- `Gamepad`
- `Controller`
- `Joystick`

Conclusao:

- Dead Space nao tem API nativa direta de controle moderno como LEGO Star Wars.
- A rota esperada e modulo EA Blast: teclado fisico, touchscreen e touchpad.

### IDs de modulo confirmados por disassembly

`ModuleCatalog` retorna:

- PhysicalKeyboard: `600`
- TouchScreen: `1000`
- TouchPad: `1100`

`TouchSurfaceAndroid_NativeOnPointerEvent` usa a assinatura real:

```c
NativeOnPointerEvent(raw_event, module_id, pointer_id, x, y)
```

O fix ja aplicado esta correto: antes os argumentos estavam invertidos.

### TouchScreen filtra por modulo

`TouchScreen::HandleMessage` confere o campo `module_id` da mensagem raw e so processa raw
touch quando o id e `1000`.

Conclusao:

- Mandar tudo como TouchScreen força a camera direita a parecer um segundo dedo comum.
- O modulo `1100` TouchPad nao e decorativo; ele existe para outra rota.

### TouchPad/Xperia existem no binario e no DEX

DEX tem:

- `GetTouchPadCount`
- `TouchPadAndroidXperiaPlay.java`
- `KeyboardAndroid.java`
- `PhysicalKeyboardAndroid.java`
- `TouchSurfaceAndroid.java`
- `kModuleTypeIdTouchPad`

ELF tem:

- `TouchPad.cpp`
- `TouchPadFactoryAndroid.cpp`
- `TouchPadAndroidXperiaPlay.cpp`
- `KeyboardAndroidXperiaPlay.cpp`
- `sTouchPad`
- `sTouchPadPointerListeners`
- `kPropertyTouchPadCount`

A engine tambem tem listas separadas:

- `sTouchscreenPointerListeners`
- `sTouchPadPointerListeners`

Conclusao:

- TouchPad/Xperia nao e sobra aleatoria.
- Ha infraestrutura real para separar touchscreen normal do touchpad.

### Problema concreto no JNI atual

`src/jni_shim.c` hoje trata counts assim:

```c
if (strstr(name, "DisplayCount") || strstr(name, "TouchScreenCount") ||
    strstr(name, "PhysicalKeyboardCount")) return MID_STR_ONE;
if (strstr(name, "Count") || strstr(name, "Available")) return MID_STR_ZERO;
```

Entao `GetTouchPadCount` cai em `MID_STR_ZERO`.

Conclusao:

- O loader provavelmente informa ao jogo que nao existe TouchPad.
- Isso pode impedir a criacao/registro da rota Xperia Play/TouchPad.
- Antes de continuar calibrando toque da camera, precisa testar `TouchPadCount=1`.

## Diagnostico dos bugs atuais

### Menu: cima/baixo falhando

Causa provavel:

- Dead Space atual ainda depende de eventos SDL para botoes.
- Em builds SDL/handheld, eventos podem falhar ou virar pulsos curtos.
- LEGO Star Wars corrigiu classe parecida com polling por frame.

Fix correto:

- Migrar botoes digitais para polling por frame.
- Manter estado anterior e emitir key down/up somente em transicao.
- Fazer isso para SDL_GameController e fallback SDL_Joystick.

### A/B/X/Y e outros botoes incompletos

Causa provavel:

- O port envia keycodes Android e tambem toca pontos fixos da tela.
- Esses pontos fixos podem acertar HUD/menu errados e gerar conflito.
- Dead Space tem teclado Xperia Play; X/Y tem mapeamento especial no binario.

Fix correto:

- Usar keycodes Android/Xperia como rota primaria.
- Desligar overlay de touch para botoes por padrao.
- Manter overlay antigo so como fallback por env var para testes.

### Analogico esquerdo

Estado:

- Usuario confirmou que no gameplay ficou bom.

Conclusao:

- Movimento por touchscreen virtual no lado esquerdo e aceitavel por enquanto.
- Nao mexer nele primeiro, exceto para trocar atualizacao por polling estavel.

### Analogico direito/camera

Causa provavel:

- A camera esta sendo simulada como drag no TouchScreen principal.
- O jogo move rapido e recenter porque isso se comporta como dedo comum, nao como touchpad/rota
  Xperia esperada.

Fix correto:

- Habilitar TouchPad no JNI.
- Resolver `NativeGetModuleTypeIdTouchPad`.
- Enviar teste do stick direito pelo modulo `1100`, nao pelo `1000`.
- Validar em log se o jogo cria/usa `sTouchPad`.
- Se `TouchPadCount=1` nao ativar o receptor, estudar o factory/registro antes de voltar para
  calibragem de toque.

## Caminho de correcao proposto

Ordem segura:

1. Habilitar `TouchPadCount=1` no `jni_shim.c`.
2. Resolver e logar `NativeGetModuleTypeIdTouchPad` em `main.c`.
3. Refatorar touch para aceitar modulo destino:
   - TouchScreen para tap/menu/movimento esquerdo;
   - TouchPad para teste da camera direita.
4. Trocar controle digital para polling por frame, padrao LEGO Star Wars:
   - `SDL_GameControllerUpdate()`;
   - `SDL_GameControllerGetButton()`;
   - `SDL_GameControllerGetAxis()`;
   - transicao de estado para key down/up.
5. Desativar touch overlay de A/B/X/Y/LB/RB por padrao.
6. Manter fallbacks por env var:
   - rota antiga de face-button touch;
   - right stick antigo por TouchScreen;
   - logs de JNI/input.
7. Testar somente no device `192.168.31.114`.

## Criterio de validacao

Primeira rodada:

- `DS_JNILOG=1`: confirmar `GetTouchPadCount` retornando `1`.
- Log de modulo: `key=600 touch=1000 touchpad=1100`.
- Menu:
  - D-pad/analogico consegue subir/descer com 1 clique/passo.
  - A confirma.
  - B/Back volta.
- Gameplay:
  - esquerdo continua andando bem.
  - direito gira camera sem snap para frente.
  - A/B/X/Y/LB/RB/LT/RT/L3/R3/Start/Back tem eventos estaveis.

Segunda rodada:

- Audio audivel em menu/cutscene/gameplay.
- Cutscene, transicoes e fechamento limpo.
- Logs sem erro novo ou com erro documentado.

## Conclusao

O port completo continua viavel. A rota mais nativa nao e mais calibrar a gambiarra de touch
para tudo; e ativar o hardware que o proprio Dead Space declara no APK: PhysicalKeyboard,
TouchScreen e TouchPad/Xperia Play. A correcao deve partir desse mapa.

## Atualizacao apos testes no device

Resultado no `192.168.31.114`:

- `GetTouchPadCount=1` e modulo `touchpad=1100` foram confirmados.
- O envio de camera pelo TouchPad nao moveu a camera no teste manual do usuario; o modulo existe,
  mas nao ficou registrado como receptor pratico nesse fluxo do loader.
- A camera direita foi trocada para swipes curtos no TouchScreen (`id=5`) por padrao. O log
  `run-final-audio-control.log` confirma `touchscreen id=5` com `DOWN/MOVE/UP` em pulsos.
- O modo antigo continua disponivel para diagnostico com `DS_RIGHT_HOLD=1`; TouchPad continua
  testavel com `DS_RIGHT_TOUCHPAD=1`.

Audio:

- A causa do silencio era JNI incompleto: `SetShortArrayRegion` caia no stub e deixava o
  `short[]` do `AudioTrack` zerado.
- `SetShortArrayRegion` e `GetShortArrayRegion` foram implementados.
- `AudioTrack.flush` foi separado de `release/stop`, limpando o ring sem pausar o device.
- `audio_write` agora bloqueia quando o ring esta cheio, simulando o comportamento do Android
  `AudioTrack.write`.
- Validacao: writes e callback SDL mostram `peak` nao-zero e `underrun=0`.
