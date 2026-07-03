/* stdio_shim.c -- ponte stdio bionic -> glibc + tabelas ctype + stubs.
 *
 * O jogo (bionic) referencia `__sF` (array de FILE p/ stdin/stdout/stderr,
 * stride sizeof(bionic FILE)=84) e chama fwrite/fputs/fputc/fread/fseek/ftell/
 * fclose com esses ponteiros. Sob glibc esses FILE* não são glibc FILE* válidos
 * -> deref/crash (o construtor de std::ios_base::Init lia __sF[1]+off=0x70 com
 * base 0 -> SIGSEGV). Provemos __sF como storage real e interceptamos as funções
 * stdio: se o FILE* cai dentro de __sF, mapeamos p/ a stream glibc; senão passa
 * direto (ex.: FILE* de fdopen). Também provê _ctype_/_toupper_tab_ (bionic) e
 * stubs de profiling.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "so_util.h"

/* bionic __sFILE em 32-bit = 84 bytes; 3 entradas (stdin/stdout/stderr). */
#define BIONIC_FILE_SZ 84
unsigned char __sF[BIONIC_FILE_SZ * 3];

static FILE *map_file(void *f) {
  uintptr_t p = (uintptr_t)f, base = (uintptr_t)__sF;
  if (p >= base && p < base + sizeof(__sF)) {
    int idx = (int)((p - base) / BIONIC_FILE_SZ);
    if (idx == 0) return stdin;
    if (idx == 1) return stdout;
    return stderr;
  }
  /* VALIDA magic glibc (_IO_MAGIC=0xFBAD0000 no _flags@off0). Se um FILE* bionic/
     custom (não-glibc) chega aqui e passamos direto, o glibc aborta ("invalid
     stdio handle"). Loga o handle ruim e retorna NULL (wrapper trata como erro). */
  if (!f) return NULL;
  unsigned flags = *(unsigned*)f;
  if ((flags & 0xFFFF0000u) != 0xFBAD0000u) {
    unsigned char *b = (unsigned char*)f;
    fprintf(stderr,"[stdio] FILE* invalido %p flags=0x%x bytes=%02x%02x%02x%02x%02x%02x%02x%02x\n",
            f, flags, b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
    return NULL;
  }
  return (FILE *)f; /* FILE* real do glibc */
}

size_t b_fwrite(const void *ptr, size_t sz, size_t n, void *f) {
  FILE *r = map_file(f); if(!r) return 0; return fwrite(ptr, sz, n, r);
}
size_t b_fread(void *ptr, size_t sz, size_t n, void *f) {
  FILE *r = map_file(f); if(!r) return 0; return fread(ptr, sz, n, r);
}
int b_fputs(const char *s, void *f) { FILE *r=map_file(f); return r?fputs(s,r):-1; }
int b_fputc(int c, void *f) { FILE *r=map_file(f); return r?fputc(c,r):-1; }
int b_fgetc(void *f) { FILE *r=map_file(f); return r?fgetc(r):-1; }
int b_fseek(void *f, long off, int wh) { FILE *r=map_file(f); return r?fseek(r,off,wh):-1; }
long b_ftell(void *f) { FILE *r=map_file(f); return r?ftell(r):-1; }
int b_fflush(void *f) { if(!f) return fflush(NULL); FILE *r=map_file(f); return r?fflush(r):0; }
int b_fclose(void *f) {
  FILE *r = map_file(f);
  if (!r) return 0;
  if (r == stdin || r == stdout || r == stderr) return 0; /* nunca fechar std */
  return fclose(r);
}
/* fprintf/vfprintf/ungetc/setvbuf também recebem FILE* (bionic __sF ou custom).
   Sem override iam direto pro glibc com ponteiro bruto -> "invalid stdio handle"
   (abort do jogo ao logar em stdout/stderr, ex.: na seleção de fases do MM6). */
int b_vfprintf(void *f, const char *fmt, va_list ap) {
  FILE *r = map_file(f); if(!r) return 0; return vfprintf(r, fmt, ap);
}
int b_fprintf(void *f, const char *fmt, ...) {
  FILE *r = map_file(f); if(!r) return 0;
  va_list ap; va_start(ap, fmt); int n = vfprintf(r, fmt, ap); va_end(ap); return n;
}
int b_ungetc(int c, void *f) { FILE *r=map_file(f); return r?ungetc(c,r):-1; }
int b_setvbuf(void *f, char *buf, int mode, size_t sz) {
  FILE *r=map_file(f); return r?setvbuf(r,buf,mode,sz):-1;
}
int b_feof(void *f){ FILE *r=map_file(f); return r?feof(r):1; }
int b_ferror(void *f){ FILE *r=map_file(f); return r?ferror(r):1; }
char *b_fgets(char *s, int n, void *f){ FILE *r=map_file(f); return r?fgets(s,n,r):NULL; }

/* ---------------- ctype (bionic) ----------------
 * bionic declara `extern const char* _ctype_;` e `const short* _toupper_tab_;`
 * (PONTEIROS p/ tabelas; macro indexa [(c)+1], c em -1..255). bits do _ctype_:
 * _U=1 _L=2 _N=4 _S=8 _P=16 _C=32 _X=64 _B=128. */
static char _ctype_table[1 + 256];
static short _toupper_table[1 + 256];
static short _tolower_table[1 + 256];
const char *_ctype_ = _ctype_table;
const short *_toupper_tab_ = _toupper_table;
const short *_tolower_tab_ = _tolower_table;
__attribute__((constructor)) static void ctype_init(void) {
  _ctype_table[0] = 0;
  _toupper_table[0] = -1;
  _tolower_table[0] = -1;
  for (int c = 0; c < 256; c++) {
    unsigned v = 0;
    if (isupper(c)) v |= 0x01;
    if (islower(c)) v |= 0x02;
    if (isdigit(c)) v |= 0x04;
    if (isspace(c)) v |= 0x08;
    if (ispunct(c)) v |= 0x10;
    if (iscntrl(c)) v |= 0x20;
    if (isxdigit(c)) v |= 0x40;
    if (c == ' ') v |= 0x80;
    _ctype_table[c + 1] = (char)v;
    _toupper_table[c + 1] = (short)toupper(c);
    _tolower_table[c + 1] = (short)tolower(c);
  }
}

/* ---------------- stubs ---------------- */
void __google_potentially_blocking_region_begin(void) {}
void __google_potentially_blocking_region_end(void) {}
int AKeyEvent_getRepeatCount(void *event) { (void)event; return 0; }
