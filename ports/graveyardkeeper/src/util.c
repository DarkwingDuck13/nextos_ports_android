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

#define LOG_NAME "debug.log"

/* 🔻 debugPrintf é chamado TODO FRAME pelo jni_shim (CallObjectMethod etc). A versão
 * antiga fazia fopen("debug.log")+fwrite+fclose no SD vfat a CADA chamada -> saturava a
 * I/O do device fraco (S905L) -> processo em D-state -> wedge/morte. Default agora =
 * SILENCIOSO. Liga com GK_DEBUGLOG=1 (stdout) ou GK_DEBUGLOG=2 (+ debug.log no SD).
 * Os diagnósticos úteis usam fprintf(stderr,...) direto, não passam por aqui. */
static int g_dbg = -1;
int debugPrintf(const char *text, ...) {
  if (g_dbg < 0) { const char *e = getenv("GK_DEBUGLOG"); g_dbg = e ? atoi(e) : 0; }
  if (g_dbg <= 0) return 0;
  va_list list;
  if (g_dbg >= 2) {
    FILE *f = fopen(LOG_NAME, "a");
    if (f) { va_start(list, text); vfprintf(f, text, list); va_end(list); fclose(f); }
  }
  va_start(list, text);
  vprintf(text, list);
  va_end(list);
  return 0;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
