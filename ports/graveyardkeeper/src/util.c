/*
 * util.c -- misc utility functions
 *
 * Based on max_arm64 by Jaakko Lukkari / fgsfds / Andy Nguyen
 * Adapted for Syberia ARM64 port
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

/* 🔻 debugPrintf é chamado TODO FRAME pelo jni_shim. A versão antiga fazia fopen+fwrite+
 * fclose em "debug.log" no SD vfat a CADA chamada -> saturava a I/O do S905L -> wedge E
 * CORROMPIA a FAT (errors=remount-ro). FIX: o arquivo vai pra /dev/shm (RAM/tmpfs) -> I/O
 * instantânea, ZERO toque no SD. Mantém o vprintf p/ stdout (que o run.sh já manda pra
 * /dev/shm/gk.out). Preserva o comportamento/timing do logging (que mascara um lost-wakeup
 * latente do job-system do Unity) SEM matar o device. GK_NODBGLOG=1 silencia de vez. */
#define LOG_NAME "/dev/shm/gk_debug.log"
static int g_dbg = -1;
int debugPrintf(const char *text, ...) {
  if (g_dbg < 0) g_dbg = getenv("GK_NODBGLOG") ? 0 : 1;
  if (!g_dbg) return 0;
  va_list list;
  FILE *f = fopen(LOG_NAME, "a");
  if (f) { va_start(list, text); vfprintf(f, text, list); va_end(list); fclose(f); }
  va_start(list, text);
  vprintf(text, list);
  va_end(list);
  return 0;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
