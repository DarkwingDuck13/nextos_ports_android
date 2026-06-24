# template-arm — esqueleto de port ARMHF (32-bit)

Template para jogos cujo `.so` principal é **armv7 (armeabi-v7a)**, não arm64. Gerado e preenchido pelo [`tools/new-port-arm.sh`](../tools/new-port-arm.sh).

> Para jogos **arm64-v8a**, use o [`template/`](../template/) (não este). O `new-port.sh` detecta a arquitetura do `.so` e escolhe o template certo.

## Arquivos
| Arquivo | Papel |
|---|---|
| `main.c.tmpl` | loader multi-módulo + crash handler **ARMHF** (campos `arm_pc`/`arm_r0`/`arm_lr` do sigcontext 32-bit). Placeholders `@SO_NAME@`, `@SECONDARY_SOS@`, `@PORT_NAME@`. |
| `build.sh.tmpl` | build com a toolchain NextOS Amlogic-old **armhf** (`-march=armv7-a -mfpu=neon -mfloat-abi=hard`), linkando SDL2/EGL/GLESv2 do sysroot. Placeholder `@PORT@`. |
| `so_util.c/.h` | carregador de `.so` (relocação, resolução de símbolos, snapshot) — variante 32-bit. |
| `imports-shims.c` | stubs/shims de imports comuns (libc/FORTIFY/pthread) pré-preenchidos. |

## Diferenças ARMHF que importam
- **Crash handler:** o `mcontext_t` 32-bit usa `arm_pc`/`arm_lr`/`arm_r0..` (não `pc`/`regs[]` do arm64). Já vem certo no `main.c.tmpl`.
- **ABI de float:** `hard` float (`-mfloat-abi=hard`) — bater com o runtime `/usr/lib32` do device.
- **pthread/ABI:** a [ponte Bionic→Glibc](../facilitando_o_trabalho/receitas/02-pthread-e-abi.md) vale igual; só os tamanhos de struct mudam.

## Conhecimento aplicado
As receitas em [`facilitando_o_trabalho/`](../facilitando_o_trabalho/) valem para os dois templates (áudio, controle, Mali-450, texturas, display, empacotamento). Comece por [01-iniciando-um-port](../facilitando_o_trabalho/receitas/01-iniciando-um-port.md).
