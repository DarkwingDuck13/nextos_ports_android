# 🚀 Iniciando um Port do Zero

Você tem um APK de Android e quer rodar no NextOS? Aqui está o "Caminho das Pedras".

> **Novidades do framework (leia antes de começar):**
> - **JNI por tabela** ([receita 14](14-jni-por-tabela.md)): não escreva mais `switch`
>   gigante no `jni_shim` — declare os métodos numa tabela e o `nx_jni` despacha.
> - **Loader genérico por engine** ([receita 15](15-loaders-genericos-por-engine.md)):
>   se o seu jogo é GameMaker/Cocos2d-x e já existe um loader da engine, talvez seja
>   só largar o APK + `game.cfg` — sem port novo.
> - **Alvo PC/x86_64** ([receita 16](16-alvo-pc-e-multiarch.md)): dá pra debugar o
>   port no desktop (gdb/asan) antes de subir pro device. Muito mais rápido.

## Passo 1: Preparação do APK
1.  Extraia o APK (use `7z x meujogo.apk`).
2.  Vá em `lib/arm64-v8a/` e identifique o binário principal (geralmente `libGame.so`, `libunity.so` ou `libmain.so`).
3.  Verifique se o jogo é **NativeActivity** ou **JNI**. Se tiver `libmain.so`, é Unity. Se tiver `libGame.so`, provavelmente é custom.

## Passo 2: O Bootstrap com `new-port.sh`
No terminal do seu PC de build:
```bash
./tools/new-port.sh ~/meujogo.apk meujogo
```
Este script vai:
*   Criar a pasta `ports/meujogo/`.
*   Extrair o `.so`.
*   Gerar o `imports.gen.c` com todos os símbolos que o jogo precisa.

## Passo 3: Resolvendo os Símbolos (O Coração do Trabalho)
Abra o `ports/meujogo/src/imports.gen.c`. Você verá uma lista enorme.
*   **Auto-resolvidos:** O script já mapeia GLES, LibC, LibM.
*   **UNKNOWN:** Estes são os que você precisa resolver. 
    *   Muitos são `__chk` (FORTIFY). Mapeie para as versões normais no `bionic_shims.c`.
    *   Outros são específicos de áudio ou rede. Se o jogo não precisar deles para abrir, você pode criar um **Stub** (uma função vazia que não faz nada).

## Passo 4: JNI e Identidade
Edite o `jni_shim.c` do seu novo port.
*   Configure o `PACKAGENAME` (ex: `com.rockstargames.bully`).
*   Configure os caminhos de arquivos. O jogo vai perguntar ao Android "onde estão meus dados?". Você deve responder com o caminho da pasta no Linux (ex: `/home/root/ports/meujogo/assets`).

## Passo 5: Build e Primeiro Teste
```bash
make -C ports/meujogo
```
Transfira para o device e rode. 
*   **Não abriu?** Cheque o `stderr`. Provavelmente falta algum símbolo ou o `so_load` falhou por falta de memória.
*   **Abriu e crashou?** Veja o guia de [Troubleshooting](../troubleshooting/01-diagnostico-de-crash.md).

---
*Dica: Comece pequeno. Tente fazer o jogo chegar no log de "Init" antes de se preocupar com gráficos ou som.*
