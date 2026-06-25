# BANCADA DE CAPTURA — Moto G100 rooted (reusável p/ QUALQUER port pairip/Unity)

## Setup (permanente, já montado)
- Device: Moto G100, Android 12, KernelSU-Next. adb id `0074124494` (USB, plugado).
- `su` interativo sai CAPLESS (CapEff=0, sem CAP_SYS_PTRACE) → frida não sobe por ali.
- FIX (poder real): módulo KernelSU `/data/adb/modules/elddump/` com `service.sh` que sobe o
  **frida-server no boot como init** = root COMPLETO (CAP_SYS_PTRACE). Arquivos: module.prop +
  service.sh + frida-server (17.15.3 arm64, localhost). Reboot ativa.
- PC: venv `/tmp/fridavenv` (frida-tools 17.15.3). Sanidade: `/tmp/fridavenv/bin/frida-ps -U`.

## App alvo
- `com.pid.elderand` 1.3.22 modded PDALIFE (tem libPDALIFE.so). Roda no device real, user 0.
- Essa versão NÃO tem pairip nos libs nativos; global-metadata.dat é PLAINTEXT (magic af1bb1fa).
- libunity real md5 8fe7856d == `device_libs_1.3.22/libunity.so`.

## Scripts (em /tmp; logs em ~/elderand-build/devharness/dev_out/)
- run_frida.py + hook.js: spawn + hook dlsym/dlopen no GOT. API frida17: Module.getGlobalExportByName.
- got_syms.py: attach + lê .got.plt da libunity (vaddr 0x1380f88, len 0x3070) + DebugSymbol.fromAddress
  → 365 slots / 364 com nome (dev_out/got_resolved_symbols.log). Provou: só 12 binds lazy.

## Puxar libs/assets do device (root)
- device_libs_1.3.22/ (libunity/libil2cpp/libmain/libfmod/libpairipcore/...).
- base.apk 276MB em /tmp/base.apk. Extrair: unzip base.apk "assets/bin/Data/*" "lib/arm64-v8a/*".
- OBB dir do app real VAZIO → confirma SKIPOBB.

## O QUE CAPTURAR AGORA (trava do Choreographer/doFrame)
Port (device Mali-450 .100) trava: a main fica em FUTEX_WAIT esperando doFrame sinalizar; nosso
invoke do delegate C# não acorda ela. No app REAL (bench) o Choreographer é real → frida-trace:
1. Fluxo de frame-pacing: quem chama nativeRender, qual futex/cond a main espera no frame 2,
   e QUEM faz o FUTEX_WAKE (uaddr) quando o doFrame chega.
2. Como o JNIBridge.invoke real despacha doFrame(long): handle do proxy + args (Long boxed nanos).
3. Offsets 1.3.22 dos símbolos de sincronização (o cond que a main espera; 0x2f3680 é da
   Cuphead/Terraria, errado p/ 1.3.22).
Hook útil: attach em com.pid.elderand, Interceptor em il2cpp_thread_attach, no export de
nativeRender do libunity, e em syscall/__futex_* logando (tid,uaddr,op) ao redor do 1º doFrame.
