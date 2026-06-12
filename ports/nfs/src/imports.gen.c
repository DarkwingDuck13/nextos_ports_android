/*
 * imports.gen.c — tabela de imports do NFS (armhf).
 *
 * F0: tabela MÍNIMA — tudo resolve via fallback dlsym(RTLD_DEFAULT) em
 * so_resolve (libc/m/dl/pthread/SDL2/EGL/GLESv2 linkados no loader + __aeabi_*
 * do libgcc via --export-dynamic). Os shims reais (egl→SDL2, jni/GameActivity,
 * pthread bridge bionic→glibc, FMOD, AndroidBitmap) entram aqui conforme as
 * fases F1+ — ver imports.gen.c.bak (gerado por new-port-arm.sh) p/ a lista
 * completa categorizada do que existe pra wirar.
 */
#include "imports.h"
#include "so_util.h"
#include <stdio.h>

FILE *stderr_fake;

DynLibFunction dynlib_functions[] = {
    {"__nfs_dummy_never_matches__", 0}, /* sentinela; resolução real = dlsym */
};
size_t dynlib_numfunctions =
    sizeof(dynlib_functions) / sizeof(dynlib_functions[0]);
