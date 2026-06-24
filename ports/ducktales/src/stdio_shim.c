/*
 * stdio_shim.c -- bionic __sF (stdin/stdout/stderr table) for glibc host.
 *
 * THE BUG this fixes: libducktales (NDK r17c) imports `__sF` and accesses the
 * standard streams as &__sF[0]=stdin, &__sF[1]=stdout, &__sF[2]=stderr, where
 * the array stride is sizeof(bionic FILE). The old resolver returned glibc's
 * `stdout` for __sF, so &__sF[1]/&__sF[2] = (glibc stdout var addr)+stride =
 * GARBAGE FILE*. The engine then calls glibc fprintf/fwrite/fputc/fflush on
 * that garbage FILE* -> glibc dereferences bogus _IO_write_ptr/_IO_buf_base
 * fields and writes characters through them = WILD HEAP WRITES. That is the
 * deterministic, glibc-only heap corruption (free-list smashed during the
 * asset-load logging) that crashed the loader. Works on bionic (real __sF).
 *
 * Fix: __sF resolves to our own region; the stdio functions that take a FILE*
 * detect a pointer inside that region and substitute the REAL glibc stream.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* Generous region so &__sF[i] for any plausible bionic FILE stride lands here.
   bionic 32-bit FILE is ~84B; 1KB covers 3 slots with margin. */
#define SF_REGION_SZ 1024
static char g_sF_region[SF_REGION_SZ] __attribute__((aligned(16)));

void *dt_sF_table(void) { return g_sF_region; }

static inline int in_sF(const void *p) {
  return (const char *)p >= g_sF_region && (const char *)p < g_sF_region + SF_REGION_SZ;
}
/* map an engine FILE* to the real glibc stream. Slot 0->stdin, 1->stdout, else
   stderr. We don't know the exact bionic stride, so anything past slot 0 that
   isn't clearly stdin is treated as an output stream (stderr) -- safe for a
   game's console logging. Non-__sF pointers (real fopen'd files) pass through. */
static int g_sf_hits = 0;
static FILE *real_file(void *fp) {
  if (!in_sF(fp)) return (FILE *)fp;
  if (g_sf_hits < 8) { g_sf_hits++;
    fprintf(stderr, "[__sF] engine stdio on std stream slot off=%ld (#%d)\n",
            (long)((char *)fp - g_sF_region), g_sf_hits); }
  size_t off = (char *)fp - g_sF_region;
  if (off == 0) return stdin;
  /* slot 1 (stdout) vs slot 2 (stderr): both are console; use stdout for the
     first non-zero slot region, stderr beyond. Heuristic; logging only. */
  return stderr;
}

int dt_fprintf(void *fp, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = vfprintf(real_file(fp), fmt, ap);
  va_end(ap);
  return r;
}
int dt_vfprintf(void *fp, const char *fmt, va_list ap) { return vfprintf(real_file(fp), fmt, ap); }
size_t dt_fwrite(const void *p, size_t s, size_t n, void *fp) { return fwrite(p, s, n, real_file(fp)); }
size_t dt_fread(void *p, size_t s, size_t n, void *fp) { return fread(p, s, n, real_file(fp)); }
int dt_fputs(const char *s, void *fp) { return fputs(s, real_file(fp)); }
int dt_fputc(int c, void *fp) { return fputc(c, real_file(fp)); }
int dt_putc(int c, void *fp) { return fputc(c, real_file(fp)); }
int dt_fflush(void *fp) { return fflush(fp && in_sF(fp) ? real_file(fp) : (FILE *)fp); }
int dt_fileno(void *fp) { return fileno(real_file(fp)); }
int dt_ferror(void *fp) { return ferror(real_file(fp)); }
int dt_feof(void *fp) { return feof(real_file(fp)); }
void dt_clearerr(void *fp) { clearerr(real_file(fp)); }
int dt_getc(void *fp) { return getc(real_file(fp)); }
char *dt_fgets(char *s, int n, void *fp) { return fgets(s, n, real_file(fp)); }
int dt_setvbuf(void *fp, char *buf, int mode, size_t sz) { return setvbuf(real_file(fp), buf, mode, sz); }
int dt_fseek(void *fp, long off, int wh) { return fseek(real_file(fp), off, wh); }
long dt_ftell(void *fp) { return ftell(real_file(fp)); }
int dt_fclose(void *fp) { return in_sF(fp) ? 0 : fclose((FILE *)fp); }  /* never close a std stream */
