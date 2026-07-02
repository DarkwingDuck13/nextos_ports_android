# 🎬 Unity por dentro: bootstrap JNI, render loop e GC

O que o `UnityPlayer` Java real faz por baixo dos panos — e que o nosso loader precisa replicar na ordem certa. Minerado do estudo do Bogodroid neo ([`ports/bogodroid/`](../../ports/bogodroid/)), validado lá em Unity **2019.4, 2021.3 e 2022.3** (IL2CPP e Mono). Vale pra Terraria, Elderand, FF9, Graveyard Keeper, Pixel Cup, Cuphead, RE4.

## 1. A ordem de carga das libs
```
libc++_shared.so → libmain.so → libil2cpp.so → libunity.so → lib_burst_generated.so (se existir)
```
Variante **Mono** (jogos antigos, ex. RE4): no lugar de `libil2cpp.so` entra o trio `libmonobdwgc-2.0.so` + `libMonoPosixHelper.so` + `libmono-native.so`. IL2CPP moderno também carrega os metadados `assets/bin/Data/Managed/*.dll.so` quando existem.

## 2. A sequência de boot JNI (a "dança" exata)
1. `JNI_OnLoad(libmain)` → chamar `com/unity3d/player/NativeLoader.load("lib/arm64-v8a")` — **retorna bool; se false, nada vai funcionar** (é o primeiro health-check do port).
2. `JNI_OnLoad(libil2cpp)` → `JNI_OnLoad(libunity)`.
3. `UnityPlayer.initJni(Context)` — nativo em libunity.
4. `nativeRecreateGfxState(0, Surface)` → `nativeRestartActivityIndicator()` → `nativeSendSurfaceChangedEvent()` → `nativeResume()` → `nativeFocusChanged(true)`.

Pular `nativeSendSurfaceChangedEvent`/`nativeFocusChanged` = engine roda "pausada"/sem surface — sintoma clássico de **áudio/log vivos com tela preta ou congelada**.

## 3. 🔑 O render é bombeado DE FORA
A Unity no Android **não tem loop próprio de render**: quem chama `UnityPlayer.nativeRender()` a cada frame é o lado Java (Choreographer). No nosso loader, **o host precisa ficar num loop chamando `nativeRender()`** depois do boot. Se o seu port Unity "roda mas não desenha", confira ANTES DE TUDO se alguém está bombeando o render — e se a sequência da seção 2 foi completada.

## 4. GC incremental: desligue
Unity 2021+ liga o **GC incremental** por padrão, e ele tem incompatibilidade conhecida rodando fora do Android real (write barriers dependem de sinais/mprotect que o nosso ambiente trata diferente). Fix de 1 linha no launcher:
```sh
export GC_DISABLE_INCREMENTAL=1
```
Suspeite dele em: hang aleatório, tempo congelado (classe do bug `Time.time` do FF9), crash intermitente no GC.

## 5. Input canônico: `nativeInjectEvent`
Em vez de inventar caminho de input por jogo: criar objetos fake `android/view/KeyEvent`/`MotionEvent` no JNI fake e injetar pelo método nativo estático `UnityPlayer.nativeInjectEvent(Landroid/view/InputEvent;)Z`. É o mesmo funil que o `UnityPlayerActivity` real usa — funciona pra qualquer versão/jogo Unity. (Layout dos campos de KeyEvent/MotionEvent: ver `ports/bogodroid/upstream-neo/javastubs/android_view.cpp`.)

## 6. `ReflectionHelper`: quando o C# toca o Android
Todo `AndroidJavaObject`/`AndroidJavaClass` do C# passa por `com.unity3d.player.ReflectionHelper.getMethodID/getFieldID/getConstructorID` no nosso JNI fake. Se o jogo crasha ao tocar código "Android" vindo do C# (analytics, Play Games, review prompt), o buraco está aí — resolver por busca de nome no fake-JNI e stubar a classe alvo (ex.: `PlayAssetDeliveryUnityWrapper.playCoreApiMissing() → true` desarma o Play Asset Delivery inteiro).

## 7. Precisa ver o que a engine está fazendo?
Use as duas lupas da [troubleshooting 03 — Pegando logs: jnitrace e shim IL2CPP](../troubleshooting/03-logs-jnitrace-e-shim-il2cpp.md): o jnitrace mapeia os stubs JNI **antes** do port; o shim IL2CPP loga cada chamada do runtime **durante** a execução.

---
*Dica: endereços de load fixos por lib (ex. libunity em `0x38_0000_0000`, libil2cpp em `0x36_0000_0000`) fazem qualquer backtrace revelar a lib culpada só pelo prefixo do endereço.*
