# Unity Android no NextOS: guia mestre para novos ports

> Estado da base: 2026-07-04. Este guia consolida o que os ports Unity do
> `nextos_ports_android` ensinaram até agora. Use junto com
> `12-unity-bootstrap-render-gc.md`, não no lugar dele: a receita 12 explica o
> boot mínimo; esta aqui explica como transformar boot em port jogável.

## 0. O veredito antes de começar

Unity Android via so-loader é viável, mas só quando a camada que falta é
identificada cedo. O erro caro é tratar "Unity" como um único motor. Na prática,
cada jogo cruza estes eixos:

| Eixo | Pergunta que decide o caminho |
|---|---|
| Runtime | `IL2CPP` ou `Mono`? metadata plaintext? libmono/Boehm? |
| Render | GLES2 nativo, GLES3 only, Vulkan, URP, shader variants disponíveis? |
| Threading | Render single-thread, `GfxDeviceWorker`, Choreographer, job-system/async load? |
| Assets | `assets/bin/Data` simples, bundles `.split`, Play Asset Delivery, AssetManager Java? |
| Audio | FMOD interno Unity, `org.fmod.FMODAudioDevice`, OpenSL/AudioTrack, libfmod externa, sdlib? |
| Input | Unity `Input`, InControl, Rewired, touch mobile, API gerenciada própria, evdev direto? |
| Device | Mali-450 GLES2/832MB, X5M ES3/ASTC/RAM maior, R36S/G31, desktop teste? |

Regra prática: primeiro classifique o port. Só depois copie código de outro
port. Copiar o scaffold errado cria semanas de debug em offset velho.

## 1. Matriz honesta dos ports Unity que temos

| Port | Stack | Estado | Video/render | Audio | Controle/input | Lição principal |
|---|---|---|---|---|---|---|
| `ports/terraria` | Unity 2021.3 IL2CPP, ES2 | Jogável: player, mundo, audio, controle | Caminho Unity IL2CPP do scaffold; render bombeado por `nativeRender`; precisa jobs inline/skip com cuidado | FMOD a 24000 Hz; taxa real vem do `fmodGetInfo`, não de chute | `TER_NATPAD=1`: expõe Xbox via InControl/UnityInputDevice; evita mouse falso que prende UI em modo touch | Referência principal de runtime jogável. Para Unity IL2CPP ES2, comece aqui. |
| `ports/ff9` | Unity 2022.3 IL2CPP, arm64 | Campo 3D atingido com audio, UI e controle; backgrounds EBG pretos pendentes | ES2 forçado, asset redirect, FMVs por player externo; risco ES3/RAM | Audio real via stack do jogo/sdlib; FMVs podem ser video-only | Hook direto de UI/título (`FF9_TITLEPAD`) e controle físico | Bom modelo para FMV, title menu e jogo pesado. X5M é alvo mais natural que Mali-450 para o final. |
| `experiments/re4-recon` + acervo RE4 | Unity 2018 Mono, armhf, GLES2 | Boot, menu, Cap.1, som/HUD/save/fullscreen; fechado como demo porque andar congela | EGL/configs exatos; GL/threading foi grande parede; fullscreen exigiu cap real de RT | FMOD `org.fmod.FMODAudioDevice`; SDL callback pull chamando `fmodProcess` | Menu via uGUI/Mono runtime; gameplay tinha touch/HUD, mas input não era a raiz do freeze | Melhor referência de Unity Mono/Boehm e audio FMOD antigo. Parede final: job-system ao andar. |
| `ports/megamanx` | Unity 2021.3 IL2CPP, GLES3 only | Boot/render/stage parcial; gating full version, audio e input pendentes | Shader transpiler GLES3->GLES2; patch `Sprites/CutOut`; `MMX_INLINETASK`/WaitForJobGroup | FMOD clips falham (`Cannot create FMOD::Sound instance`) | Mobile/touch/controlKey; KeyEvent não basta; pad->touch em validação | Se não há GLES2 variant, shader transpiler vira parte do port, não polish. |
| `experiments/hollow-recon` | Unity 2020.2 IL2CPP | Cena renderiza 25-28fps com caps; input é a parede | `HK_MAXTEX=512` evita ASTC gigante/CPU decode; AssetManager Java real necessário | FMOD inicializa | Rewired/evdev/touch; não consome `nativeInjectEvent` nem gamepad fake | O jogo pode renderizar só depois de capar textura agressivamente. Input pode estar fora da Unity. |
| `ports/elderand` | Unity 2021.3 IL2CPP + URP + pairip | Pesquisa/loader; não jogável | URP ES3, handshake `GfxDeviceWorker`/Choreographer, lazy PLT, RAM pesada | libfmod/libfmodstudio separadas | Não chegou no input final | Para URP/ES3 pesado, X5M é mais racional; Mali-450 exige shader/device work grande. |
| `ports/cuphead` | Unity IL2CPP | WIP; display depende de fbdev Mali; inimigos/sprites invisíveis em estágio | Caminho usa fbdev real; X5M KMSDRM precisa backend diferente; debug de scissor/alpha/vertex color | A validar | Rewired/gerenciado era o alvo histórico | Bom caso para debug de draw invisível: scissor, matriz, vertex color, shader dump. |
| `ports/graveyardkeeper` | Unity 2018.2 IL2CPP, ES2/ETC1 | Loading renderiza; `scene_main_mobile` async e FMOD ainda pendentes | Choreographer fake destravou; asset redirect e fast paths de stat ajudaram bundles | FMOD nativo; `GK_NOSOUNDASSERT`, `GK_STREAMFALLBACK` em estudo | Controle só depois do menu | Confirma que boot/render não bastam: async scene + FMOD podem ser a parede real. |
| `ports/pixelcup` | Unity 2022.3 IL2CPP, ES2 | Loading renderiza; load async não progride | `m_MTRendering=False` em `globalgamemanagers` resolveu deadlock GL; depois veio async/job-system | Recursos/audio `Resources.LoadAsync` não abrem | Rewired NullRef não fatal; input não é a parede atual | Caso mais importante de Unity 2022: job/async thread-local não drenada, Play Asset Delivery e `/tmp` cheio confundiram diagnóstico. |

## 2. Escolha do port base

Use esta ordem:

1. **Unity IL2CPP ES2 simples:** copie a cabeça de `ports/terraria`.
2. **Unity 2022 IL2CPP:** use Terraria para boot, mas leia FF9 e Pixel Cup antes de mexer em job-system,
   `globalgamemanagers`, Play Asset Delivery e FMV.
3. **Unity 2018 Mono:** use RE4 como referência conceitual. Mono/Boehm tem armadilhas próprias que
   Terraria não cobre.
4. **URP/GLES3/ASTC:** assuma alvo X5M primeiro. Mali-450 só se houver plano explícito de transpilar
   shaders, capar texturas e aceitar muito RE.
5. **Input Rewired/InControl/touch:** escolha pelo sistema de input do jogo, não pelo motor Unity.

Nunca use um offset `CUP_*`, `TER_*`, `GK_*`, `MMX_*` em outro jogo sem rederivar. Nome de env herdado é
útil para debug, mas offset herdado é quase sempre bug.

## 3. Recon obrigatório do APK

Antes de escrever código, monte este quadro:

```sh
unzip -l jogo.apk | rg 'lib/|assets/bin/Data|global-metadata|boot.config|globalgamemanagers'
readelf -d lib/arm64-v8a/libunity.so
readelf -d lib/arm64-v8a/libil2cpp.so
strings assets/bin/Data/globalgamemanagers | rg -i 'unity|20[0-9][0-9]\.'
xxd -l 16 assets/bin/Data/Managed/Metadata/global-metadata.dat
```

Checklist:

- Unity version exata (`2018.2.2f1`, `2021.3.56f2`, `2022.3.62f3` etc.).
- ABI: arm64, armv7, x86; Mono RE4 foi armhf, a maioria atual é arm64.
- Runtime: `libil2cpp.so` + metadata ou `libmono*.so` + `Managed/*.dll`.
- Libs extras: `lib_burst_generated.so`, `libmain.so`, `libfmod.so`, `libsdlib_android.so`, `libgpg.so`,
  `libpairipcore.so`, `libAPKVISION.so`.
- Renderer: GLES2 strings, GLES3 only, Vulkan only, URP, shader blob platforms.
- Texturas: ETC1/ETC2/ASTC/DXT; Mali-450 só gosta de ETC1/RGBA8 e GLES2.
- Data path: `assets/bin/Data` normal, `.split0`, bundles hash, PAD/datapack, OBB.
- Android package, activity, `Application.persistentDataPath` esperado, permissões, PAD/PlayCore.

Classifique também o risco:

| Sinal | Interpretação |
|---|---|
| `force-gles20`, shader platform GLES2, ETC1 | Bom alvo Mali-450. |
| Unity 2022 + `GfxDeviceWorker` + load async pesado | Alto risco de job-system/async. |
| URP, ASTC, GLES3 only | X5M primeiro; Mali-450 exige conversão séria. |
| `Resources.LoadAsync`, `AssetBundle.LoadFromFileAsync`, PAD | Planeje debug de async antes do menu. |
| Rewired/InControl | `nativeInjectEvent` pode ser irrelevante; precisa mapear a API real. |

## 4. Bootstrap Unity correto

A ordem base continua a da receita 12:

```text
libc++_shared.so se existir
libmain.so
libil2cpp.so ou libmono*.so
libunity.so
lib_burst_generated.so se existir
libs extras do jogo

JNI_OnLoad(libmain)
NativeLoader.load("lib/arm64-v8a")
JNI_OnLoad(libil2cpp/libmono)
JNI_OnLoad(libunity)
UnityPlayer.initJni(Context)
nativeRecreateGfxState(0, Surface)
nativeRestartActivityIndicator()
nativeSendSurfaceChangedEvent()
nativeResume()
nativeFocusChanged(true)
loop: nativeRender()
```

Pontos que não podem ficar "mais ou menos":

- `Context`, `Activity`, `ApplicationInfo`, `PackageManager`, `AssetManager`, `Handler`, `Looper`,
  `Choreographer` e `Surface` precisam ser objetos fake consistentes, não só ponteiros não nulos.
- `GetObjectClass`/`IsInstanceOf` precisam diferenciar `KeyEvent`, `MotionEvent`, `AssetManager`, `File`,
  `String`, callbacks e classes de plugins.
- `jstring` usado depois precisa ser persistente. RE4 teve crash de path/prefs por lifetime errado.
- `dlopen("")` e self refs precisam retornar handle global útil.
- `R_AARCH64_ABS64` contra símbolo `UNDEF` deve resolver import de verdade, não `base+0`.
- Canário bionic no TLS (`tpidr_el0+0x28`) precisa sobreviver a chamadas SDL/EGL/glibc.
- `GC_DISABLE_INCREMENTAL=1` é padrão seguro para Unity 2021+ no nosso ambiente.

## 5. Mono Unity: diferenças que quebram se copiar IL2CPP

RE4 mostrou que Unity Mono antigo tem paredes que não aparecem no IL2CPP:

- Boehm GC consulta memória, page size e stack. `sysconf`, `pthread_getattr_np`, `pthread_attr_*` e
  layout bionic/glibc precisam bater.
- `stat64`, `fstat64`, `fstatat64`, `lstat64` devem devolver layout bionic/kernel correto em ARM32.
  Layout glibc truncou `mscorlib` e virou CIL inválido.
- `sigaction` bionic não pode receber escrita do tamanho glibc.
- `libmono` multi-módulo resolve muitos `mono_*` por `dlsym`; `dlopen`/global symbol scope importam.
- Managed exceptions escondem erro real; injete logs em `il2cpp_raise_exception` no IL2CPP e use
  tracing/Mono runtime no Mono.

Se for Mono, valide primeiro: `mscorlib` carrega, C# runtime inicializa, `PlayerPrefs` funciona,
`Application.persistentDataPath` aponta para `userdata`, e paths Windows `\` viram `/` quando o jogo usar.

## 6. Video/render: de tela preta a cena real

### EGL e surface

Unity valida configs com mais rigor que muitos jogos nativos. RE4 exigiu config com luminance/cor
correta; Mali-450 é sensível a ES2, depth e FBO.

Faça:

- Não force `SDL_VIDEODRIVER` no launcher. Deixe o sistema escolher, salvo caso específico documentado.
- Exponha `ANativeWindow_getWidth/Height` e `eglQuerySurface` coerentes com a surface real.
- Faça teardown limpo: `glFinish`, `eglMakeCurrent(...NO_SURFACE...)`, `eglTerminate`, `SDL_QuitSubSystem`.
- Capture `/dev/fb0`, mas lembre: em alguns devices fb0 mente ou é double-buffer (`1280x1440` para 720p).
- Em Mali-450, bug de depth pode rejeitar toda geometria; RE4 precisou `glDepthMask(1)+glClearDepthf(1.0)`.

### GfxDeviceWorker e multithreaded rendering

Se a tela congela em primeiro frame ou loading:

1. Veja threads: `UnityGfxDeviceW`, `Job.Worker`, `Background Job.`, `Loading.Preload`, `Loading.AsyncRead`.
2. Se `GfxDeviceWorker` está em GL (`glClear`, `mali_frame_builder`) e main espera job, suspeite
   multithreaded rendering no Mali.
3. Para Unity que respeita args, tente `-force-gfx-direct`.
4. Para Unity 2022 que ignora cmdline, Pixel Cup provou que editar `m_MTRendering=False` no
   `globalgamemanagers` pode resolver a camada GL.

Pixel Cup: `boot.config gfx-disable-mt-rendering=1` não bastou; o setting serializado venceu. O fix real
foi mudar PlayerSettings via UnityPy/repack LZ4. Depois disso o muro mudou para async load, que é outro
problema.

### Shaders

Classifique os shader blobs:

- **GLES2/platform 5 presente:** melhor caso para Mali-450.
- **GLES3/platform 9 apenas:** precisa transpiler ou device ES3. Mega Man X é o modelo: GLSL 300 es -> 100,
  remove `layout`, transforma `in/out`, achata UBO, `texture()` -> `texture2D`, preserva bytes finais de
  channel/binding e relabela platform/programType.
- **URP:** risco alto no Mali-450. Elderand aponta X5M como alvo honesto.
- **Sprite invisível:** debug por scissor, alpha, vertex color, matriz e shader dump. Cuphead é o caso.

Nunca trate shader transpiler como "último polish" se o jogo não tem GLES2. É parte do boot.

### Texturas

Mali-450:

- ETC1 ok.
- ETC2/ASTC não são caminho nativo.
- DXT5 solto precisa transcode ou upload RGBA8.
- Cap de textura pode ser obrigatório. Hollow Knight só renderizou corretamente quando `HK_MAXTEX=512`
  evitou ASTC 2K/4K e decode pesado.
- Reduzir no upload é preferível a converter asset inteiro no runtime.

## 7. Audio: FMOD, OpenSL, AudioTrack e taxa real

Unity normalmente cai em uma destas rotas:

| Rota | Exemplo | Como resolver |
|---|---|---|
| FMOD interno Unity | Terraria, Graveyard Keeper | Hook FMOD/createSound/process, SDL callback pull, taxa real via introspecção. |
| `org.fmod.FMODAudioDevice` Java | RE4 | Fake Java object não nulo, DirectByteBuffer, thread FMOD puxada por SDL callback. |
| libfmod/libfmodstudio externa | Elderand | Carregar libs extras e mapear imports antes de Unity pedir audio. |
| sdlib/plataforma do publisher | FF9 | Não matar exceções cegamente; entender ponte SQEX antes de stubar. |
| FMV externo | FF9 | Player externo para video; não iniciar audio player quando o MP4 não tem audio. |

Lições que devem virar regra:

- Nunca chute sample rate. Terraria tocava errado quando SDL ficou em 44100 para mix FMOD 24000.
- Prefira callback pull a `QueueAudio` quando o jogo controla o mixer.
- Backpressure precisa ser generoso e pouco verboso; log em hot path altera timing.
- Não force `SDL_AUDIODRIVER`; só use override em teste documentado.
- Não "resolve" pulando audio se o objetivo do port é final. Graveyard Keeper mostrou que pular audio pode
  avançar, mas deixa handle real faltando e troca uma parede por outra.

## 8. Controle: escolha a camada certa

O maior erro recorrente foi achar que `nativeInjectEvent` resolve todo Unity. Ele só resolve quando o jogo
lê o input Android/Unity nessa rota. Muitos jogos não leem.

| Sistema | Exemplo | O que alimentar |
|---|---|---|
| InControl / `UnityInputDevice` | Terraria atual | `Input.GetJoystickNames()` + botões/eixos Xbox. `TER_NATPAD=1`. |
| Unity UI gerenciado específico | FF9 título, RE4 menu | Hook/dirija classes de UI diretamente quando a tela é conhecida. |
| Mobile touch/controlKey | Mega Man X | Pad -> MotionEvent/touch ou hook direto em `controlKey`; KeyEvent é ignorado. |
| Rewired | Hollow Knight, Cuphead histórico | Rewired/evdev/touch; `nativeInjectEvent` pode ser invisível. |
| XNA/FNA gerenciado dentro Unity | estudo Terraria input antigo | Hook `GamePad.GetState`/API gerenciada, não gptokeyb OS. |
| Unity antigo com Android InputEvent | RE4 parcialmente | `nativeInjectEvent(KeyEvent/MotionEvent)` com objetos Java bem tipados. |

Fluxo recomendado para um port novo:

1. Procure strings: `Rewired`, `InControl`, `UnityEngine.InputSystem`, `GamePad.GetState`,
   `Input.GetJoystickNames`, `Touch`, `controlKey`.
2. Com jnitrace ou log JNI, veja se Unity chama `nativeInjectEvent`, `InputDevice`, `MotionEvent`,
   `KeyEvent`, `Vibrator`, `InputManager`.
3. Se houver metadata IL2CPP, dump métodos e ache quem lê input no menu.
4. Decida se o jogo está em modo touch, mouse ou gamepad. Terraria mostrou que misturar mouse falso e
   gamepad falso pode deixar a UI em modo errado.
5. Só use gptokeyb se o jogo lê teclado/mouse reais do OS via SDL/FNA/desktop. No Android so-loader,
   gptokeyb geralmente não entra no pipeline.

## 9. Job-system, Choreographer e async load

Esta é a maior parede dos Unitys atuais.

### Sintomas

- Boot mostra logo/loading e congela.
- `nativeRender` para em `WaitForJobGroup`, `pthread_cond_wait`, `Baselib Semaphore::Acquire` ou futex.
- Threads `Loading.Preload`/`Loading.AsyncRead` ficam ociosas.
- `Resources.LoadAsync`/`AssetBundle.LoadFromFileAsync` nunca abre o arquivo.
- `datapack.unity3d` ou `resources.resource` nunca aparece no `strace openat`.
- Render thread existe, mas main espera completion que nunca sinaliza.

### O que já funcionou

- **Preservar posts em `sem_init`:** RE4 perdeu wakeup porque o shim reinicializava semáforo com post
  pendente. Corrigir isso levou menu/capítulo a funcionar.
- **Choreographer fake real:** Graveyard Keeper só avançou quando `doFrame` foi capturado e chamado em
  thread própria, com skip cirúrgico no wait correto.
- **Inline/skip task específico:** Terraria e Mega Man X avançaram com patches pontuais (`TER_INLINETASK`,
  `MMX_INLINETASK`, skip de WaitForJobGroup). Só use se o job não carrega dados essenciais.
- **Desligar MT rendering no asset:** Pixel Cup resolveu a primeira parede GL com `m_MTRendering=False`.

### O que não é fix suficiente

- Acordar futex/worker no escuro. Pixel Cup mostrou que worker acordado sem job na fila só volta a dormir.
- `CUP_CONDPOLL`/timed wait quando o predicado nunca muda. Isso só prova que não é lost wakeup.
- `SKIPJOBWAIT` genérico. Se o job carrega bundles/audio/cena, o jogo avança com dados nulos e quebra.
- CPU spoof para reduzir workers. Unity 2022 criou pool fixo mesmo com core count falsificado.

### Diagnóstico correto

1. Logue `WAIT`/`WAKE` com tid, comm, uaddr e caller.
2. Separe stdout/stderr/log de arquivo. Pixel Cup teve diagnósticos errados por ler o log errado.
3. Pegue `libunity` load base do log; em gdb use `add-symbol-file libunity.so -o <base>` quando possível.
4. Se gdb falhar, use `/proc/PID/mem` + stack walk. Pixel Cup foi melhor por `/proc/mem`.
5. Verifique se o arquivo async é aberto. Se nem `openat` acontece, a falha está antes do IO real.
6. Diferencie:
   - lost wakeup: alguém posta e alguém espera no mesmo semáforo, mas o post se perde;
   - job órfão: workers acordam mas não acham trabalho;
   - produtor nunca enfileira: sem WAKE e sem mudança de contador;
   - Choreographer/Handler faltando: callback Java nunca dispara a parte nativa.

## 10. Assets, paths e Play Asset Delivery

Unity Android espera um mundo Java/Android:

- `Application.dataPath`
- `persistentDataPath`
- `AssetManager.open/openFd`
- `Context.getFilesDir/getCacheDir/getExternalFilesDir`
- `ApplicationInfo.sourceDir/nativeLibraryDir/dataDir`
- `SplitInstall`/Play Asset Delivery em Unity 2021/2022

Regras:

- Redirecione `assets/bin/Data/<nome>.split0` e bundles hash de forma estruturada.
- Não faça `snprintf(buf, "%s", buf)` nem reuse buffer fonte/destino.
- `access`, `stat`, `open`, `fopen`, `mkdir`, `rename`, `unlink` devem passar pelo mesmo normalizador.
- Paths Windows `\` viram `/` se o jogo usa save antigo/portado.
- `statfs`/low storage pode bloquear jogo. Pixel Cup teve ruído real de `/tmp` cheio; limpe `/tmp` do device
  e não confunda ENOSPC com bug de Unity.
- PAD: responder `getAssetPackState(...)=COMPLETED` pode não bastar. Se Unity nunca chama
  `getAssetPackPath` nem monta `datapack`, a callback nativa que registra localização ainda falta ou o job
  async que reage ao status não está rodando.

## 11. Memória e escolha de device

Mali-450 / Amlogic-old:

- GLES2 estrito.
- 832MB RAM típico; GPU usa UMA e não aparece toda no RSS.
- ETC1/RGBA8; sem ASTC/ETC2 nativo.
- Excelente para Unity 2018-2021 2D/ES2 leve.
- Sofre com Unity 2022 pesado, URP, ASTC e async/job-system.

X5M / hardware mais novo:

- Melhor para ES3, ASTC/ETC2, RAM maior e URP.
- Não copie backend fbdev Mali para KMSDRM esperando funcionar. Cuphead mostrou que display backend é parte
  do port.

Política prática:

- Se o jogo é ES2/ETC1 e assets moderados, tente Mali-450.
- Se é Unity 2022 + URP/GLES3/ASTC, faça primeiro no X5M para provar gameplay/audio/input.
- Só volte ao Mali-450 quando souber exatamente quais shaders/texturas/render paths precisam converter.
- Swap/zram só entram se a política do projeto/device permitir. Não esconda bug de load atrás de swap.

## 12. Ferramentas e logs que devem existir em todo port Unity

Inclua cedo, desligado por padrão:

- `*_DLLOG`: `dlopen/dlsym` e libs carregadas.
- `*_JNILOG`: classes/métodos/callers JNI sem floodar frame.
- `*_LOADSPY`/`*_NATIVELOADSPY`: cenas, AssetBundle, Resources, paths.
- `*_ASYNCPOLL`: `AsyncOperation.isDone/progress` por frame limitado.
- `*_AUDIOSPY`: createSound/process/rate/falhas.
- `*_SHADERDUMP`: shader source, platform, compile log.
- `*_TEXSTAT`: formato/tamanho/cap de textura.
- `*_FUTEXLOG`/`*_CONDTRACE`: waits/wakes com caller.
- `*_FRAMES=N`: limite de frame para teste reprodutível.
- Watchdog/timeout/nice para não matar SSH em busy-wait.

Cuidados:

- Logs em hot path mudam timing.
- `tee`/redirecionamento para vfat pode travar ou perder linhas; prefira arquivo em ext4/tmpfs controlado.
- Limpe logs antigos antes de concluir.
- Mate processo por `/proc/*/exe`, não por nome solto.
- Confirme binário novo por md5 no device; RE4 teve binário stale por scp falhar com processo segurando exe.

## 13. Fluxo recomendado de um novo port Unity

### Fase A: recon

1. Extraia APK.
2. Preencha a matriz runtime/render/assets/audio/input/device.
3. Rode jnitrace no Android real, se possível, antes de portar.
4. Dump IL2CPP/metadata se plaintext.
5. Escolha port base.

### Fase B: boot mínimo

1. Carregue libs na ordem correta.
2. Faça fake JNI mínimo, mas tipado.
3. Abra `assets/bin/Data`.
4. Rode `UnityPlayer.initJni`.
5. Crie surface/EGL.
6. Bombeie `nativeRender`.
7. Capture framebuffer e log de Unity.

Meta desta fase: `nativeRender` roda 300-1000 frames sem crash, mesmo que tela seja loading.

### Fase C: render real

1. Resolva EGL config/depth/FBO.
2. Resolva shaders/variants.
3. Cap texturas se travar por ASTC/VRAM.
4. Desligue MT rendering se Mali/Unity entrar em GL concorrente.
5. Valide na TV ou screenshot confiável.

Meta: cena/menu visível de verdade.

### Fase D: load/async

1. Veja se cenas/bundles/resource files abrem.
2. Se `AsyncOperation.progress` fica 0, trace Preload/AsyncRead/job-system.
3. Só use skip/inline se provar que não pula dados necessários.
4. Para PAD, prove que `datapack.unity3d` abre.

Meta: sair do loading para menu/jogo.

### Fase E: audio

1. Identifique FMOD/OpenSL/sdlib.
2. Faça objeto Java/AudioTrack fake não nulo quando necessário.
3. Descubra sample rate real.
4. Use callback pull se o mixer esperar pull.
5. Valide sem desligar audio final.

Meta: musica/SFX sem velocidade errada nem underrun constante.

### Fase F: input

1. Descubra a camada real de input.
2. Faça só uma estratégia por vez: gamepad, touch ou UI hook.
3. Evite misturar mouse falso e gamepad falso até entender modo de UI.
4. Valide menu, pause, gameplay e text entry separadamente.

Meta: fluxo normal sem autotap/debug.

### Fase G: empacotamento

1. Limpe env de debug.
2. Mantenha flags de compatibilidade documentadas.
3. Não distribua dados do jogo.
4. Atualize `STUDY.md`/`HANDOFF.md` com offsets reais e o estado honesto.

## 14. Checklist de "não repetir"

- Não começar por input se o jogo nem abriu o bundle da próxima cena.
- Não declarar "touch-only" sem provar; Terraria corrigiu essa hipótese.
- Não usar gptokeyb em Android so-loader esperando que Unity leia OS events.
- Não usar offset de outro Unity.
- Não pular audio como solução final.
- Não confundir `/tmp` cheio, log velho ou binário stale com bug do jogo.
- Não confiar só em `fb0` quando o device compõe direto no HDMI.
- Não forçar SDL driver sem motivo documentado.
- Não tratar `SKIPJOBWAIT` como fix geral.
- Não escolher Mali-450 para URP/ASTC pesado se X5M pode provar o port primeiro.

## 15. Referências internas

- `facilitando_o_trabalho/receitas/12-unity-bootstrap-render-gc.md`
- `facilitando_o_trabalho/troubleshooting/02-deadlock-job-system.md`
- `facilitando_o_trabalho/troubleshooting/03-logs-jnitrace-e-shim-il2cpp.md`
- `ports/terraria/HANDOFF.md`
- `ports/ff9/STUDY.md`, `ports/ff9/HANDOFF.md`
- `experiments/re4-recon/RE4-MALI450-PORT.md`
- `/mnt/ARQUIVOS/TRABALHO CLAUDE CODE/03-PORTS-E-RECEITAS/projetos/RE4-GAMEPLAY-2026-06-17.md`
- `/mnt/ARQUIVOS/TRABALHO CLAUDE CODE/03-PORTS-E-RECEITAS/projetos/RE4-CONTROLES-PENDENTE.md`
- `ports/megamanx/STUDY.md`, `ports/megamanx/HANDOFF.md`
- `experiments/hollow-recon/STATUS.md`, `experiments/hollow-recon/HANDOFF-2026-06-18.md`
- `ports/elderand/README.md`, `ports/elderand/HANDOFF_s8.md`
- `ports/cuphead/README.md`, `ports/cuphead/HANDOFF-SESSAO17.md`
- `ports/graveyardkeeper/STUDY.md`
- `ports/pixelcup/STUDY.md`, `ports/pixelcup/HANDOFF.md`
