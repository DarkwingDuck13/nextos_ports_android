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

int debugPrintf(const char *text, ...) {
  va_list list;
  /* NÃO escrever em arquivo (fopen/fclose por chamada na vfat = lento + enche
   * disco com o spam da engine). NFS_DEBUGFILE=1 reativa p/ debug pontual. */
  static int filelog = -1;
  if (filelog < 0) filelog = getenv("NFS_DEBUGFILE") ? 1 : 0;
  if (filelog) {
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
