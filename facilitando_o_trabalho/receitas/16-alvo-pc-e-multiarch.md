# 🖥️ Alvo PC (x86_64) e multiarch — rodar os ports no desktop

> **O que muda:** o framework nasceu pra Linux **ARM64** (Mali). Mas muitos APKs
> trazem `lib/x86_64/` e `lib/x86/` (Android roda em emulador/Chromebook/PC). A
> **mesma técnica de so-loader** carrega o `.so` x86_64 e roda direto num **PC
> Linux x86_64** — zero emulação, igual no ARM. Isso dá:
> 1. **Desenvolvimento no PC:** debugar o port na sua máquina (gdb/valgrind/asan)
>    antes de subir pro device — muito mais rápido que iterar no Mali por SSH.
> 2. **Distribuição PC:** jogos com libs x86_64 rodam em desktop Linux.

## O que já é agnóstico e o que precisa de trabalho

| Camada | Estado p/ x86_64 |
|---|---|
| `egl_shim` (EGL→SDL2) | ✅ agnóstico (SDL2 é multiplataforma) |
| `opensles_shim` (áudio→SDL2) | ✅ agnóstico |
| `nx_jni` (JNI por tabela) | ✅ C puro, sem asm — compila x86_64 |
| shims bionic/glibc, pthread bridge | ✅ em C; revisar structs ABI (ver abaixo) |
| **`so_util` (loader ELF + relocs + hooks)** | 🟡 **é o ponto** — precisa de x86_64 |

O loader hoje trata **ELF ARM64** (`R_AARCH64_*`) e ELF32-ARM (`R_ARM_*`). Pra PC
precisa de **ELF x86_64** (`R_X86_64_*`) e de um `hook_*` x86_64.

## O que implementar no `so_util` (checklist)

1. **Relocations x86_64** (`R_X86_64_GLOB_DAT`, `_JUMP_SLOT`, `_RELATIVE`,
   `_64`, `_PC32`, `_TPOFF64`). Espelha o `switch` que já existe pro AArch64.
2. **TLS (`_TPOFF64`)**: modelo de TLS do x86_64 é diferente do ARM (`tpidr_el0`
   vs `%fs`). O canário bionic e o `pthread` self precisam do slot certo em `%fs`.
3. **Trampolim de hook** (`hook_x86_64`): em ARM64 é `ldr x16,#8; br x16` + pool;
   em x86_64 é `movabs rax, imm64; jmp rax` (12 bytes) ou `jmp [rip+0]` + endereço.
4. **Alinhamento/permissões**: `mmap` + `mprotect` das seções PT_LOAD (igual, só
   conferir page size 4K).

O resto do fluxo (`init_array`, GOT patch, resolução de imports) é o mesmo — só
muda a máquina das relocs e do hook.

## Build multiarch

Dois toolchains, mesmo código:

```bash
# ARM64 (device Mali) — como hoje
CC=aarch64-...-gcc  ./build.sh

# x86_64 (PC, dev/debug)
CC=gcc  NX_ARCH=x86_64  ./build.sh   # linka SDL2/GLESv2 do host
```

No `build.sh` do port, selecione o `.so` do APK pela arch:
`lib/arm64-v8a/…` (device) vs `lib/x86_64/…` (PC). O `new-port.sh` deve extrair as
duas quando existirem (hoje só pega `arm64-v8a` — ver [próximos passos](#próximos-passos)).

## GL no PC

- Jogos **GLES2**: no PC use **GLESv2 do Mesa** direto, ou **SDL2 + GLES context**.
- Jogos **GLES1**: Mesa tem `GLES_CM`; se faltar, `gl4es` (já usado no Carrion)
  cobre GL desktop→GLES.
- Jogos que pedem **GL desktop**: `gl4es`/Mesa nativo.

## Cuidados de ABI (ARM vs x86_64)

- `struct` bionic com layout dependente de ponteiro (ex.: `sigaction`, `dirent`,
  `pthread_*`) mudam de tamanho/offsets entre 32/64 e entre arquiteturas.
  Os shims que hoje assumem AArch64 precisam de `#if defined(__aarch64__)` /
  `__x86_64__`. (Ver a lição do `sigaction` LP32 no cvgos: layout errado = crash.)
- Varargs (`va_list`) em x86_64 é bem diferente de ARM — o `nx_jni` já isola isso
  via `va_arg`, mas código que mexe em `va_list` cru precisa de cuidado.

## Próximos passos concretos

1. `so_util`: adicionar bloco `R_X86_64_*` + `hook_x86_64` (espelhar o AArch64).
2. `new-port.sh`: extrair `lib/x86_64/` e `lib/x86/` quando existirem; gerar alvo
   `make PC=1`.
3. Escolher **1 port simples já jogável** (ex.: um GameMaker/YYC) como piloto do
   alvo PC — validar render+áudio+input no desktop antes de generalizar.
4. Documentar no `COMPAT.md` de cada port quais arches o APK traz.

> **BYO-data continua valendo:** rodar no PC não muda nada — você fornece o APK
> que possui; o repo só tem o loader.
