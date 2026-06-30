/*
 * errno_compat.c — wrappers que PRESERVAM o slot tpidr_el0+0x28 nas funcoes libc
 * de I/O/alocacao que a libyoyo chama.
 *
 * Por que: a libyoyo (NDK/bionic) le o stack-canary do slot TLS bionic tpidr+0x28.
 * No glibc 2.30 (R36S/ArkOS) o errno mora EXATAMENTE em tpidr+0x28 (32 bits baixos).
 * Quando uma funcao libc seta errno (ex: malloc OOM -> ENOMEM, open de inexistente ->
 * ENOENT), os 32 bits baixos do "canary" mudam -> o check no epilogo da funcao libyoyo
 * que chamou falha -> __stack_chk_fail -> abort. No glibc 2.34 (.79 nativo) o errno fica
 * em outro offset, por isso nativo nao quebra.
 *
 * Fix: cada wrapper salva tpidr+0x28 na entrada, chama o real (glibc, chamado DIRETO pelo
 * nome -> resolve pro glibc no NOSSO binario, sem recursao/dlsym), e restaura o slot antes
 * de retornar. Em device onde errno NAO esta em 0x28 (nativo) e no-op inofensivo.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include "so_util.h"
#include "shims.h"

#define GUARD_CALL(expr) \
  unsigned long long __cy = hm_canary_save(); \
  __typeof__(expr) __r = (expr); \
  hm_canary_restore(__cy); \
  return __r;

/* malloc family (setam errno=ENOMEM na falha) */
void *w_malloc(size_t n)  { GUARD_CALL(malloc(n)) }
void *w_calloc(size_t a, size_t b) { GUARD_CALL(calloc(a,b)) }
void *w_realloc(void *p, size_t n) { GUARD_CALL(realloc(p,n)) }
void *w_mmap(void *a, size_t l, int pr, int fl, int fd, off_t o) { GUARD_CALL(mmap(a,l,pr,fl,fd,o)) }
int   w_munmap(void *a, size_t l) { GUARD_CALL(munmap(a,l)) }

/* file/dir I/O (setam errno) */
int w_close(int fd) { GUARD_CALL(close(fd)) }
long w_read(int fd, void *b, size_t n) { GUARD_CALL((long)read(fd,b,n)) }
long w_write(int fd, const void *b, size_t n) { GUARD_CALL((long)write(fd,b,n)) }
long w_lseek(int fd, long off, int wh) { GUARD_CALL((long)lseek(fd,off,wh)) }
int w_access(const char *p, int m) { GUARD_CALL(access(p,m)) }
void *w_fopen(const char *p, const char *m) { GUARD_CALL((void*)fopen(p,m)) }
int w_fclose(void *f) { GUARD_CALL(fclose((FILE*)f)) }
size_t w_fread(void *b, size_t s, size_t n, void *f) { GUARD_CALL(fread(b,s,n,(FILE*)f)) }
size_t w_fwrite(const void *b, size_t s, size_t n, void *f) { GUARD_CALL(fwrite(b,s,n,(FILE*)f)) }
int w_fseek(void *f, long o, int w) { GUARD_CALL(fseek((FILE*)f,o,w)) }
int w_fseeko(void *f, off_t o, int w) { GUARD_CALL(fseeko((FILE*)f,o,w)) }
long w_ftell(void *f) { GUARD_CALL(ftell((FILE*)f)) }
int w_fileno(void *f) { GUARD_CALL(fileno((FILE*)f)) }
void *w_fdopen(int fd, const char *m) { GUARD_CALL((void*)fdopen(fd,m)) }
void *w_opendir(const char *p) { GUARD_CALL((void*)opendir(p)) }
void *w_readdir(void *d) { GUARD_CALL((void*)readdir((DIR*)d)) }
int w_closedir(void *d) { GUARD_CALL(closedir((DIR*)d)) }
int w_mkdir(const char *p, mode_t m) { GUARD_CALL(mkdir(p,m)) }
int w_remove(const char *p) { GUARD_CALL(remove(p)) }
int w_unlink(const char *p) { GUARD_CALL(unlink(p)) }
int w_rename(const char *a, const char *b) { GUARD_CALL(rename(a,b)) }

/* variadic (open/openat/fcntl/ioctl): 3o arg como long */
int w_open(const char *p, int fl, ...) {
  long a3 = 0; if (fl & O_CREAT) { va_list v; va_start(v, fl); a3 = va_arg(v, long); va_end(v); }
  GUARD_CALL(open(p, fl, (mode_t)a3))
}
int w_openat(int d, const char *p, int fl, ...) {
  long a4 = 0; if (fl & O_CREAT) { va_list v; va_start(v, fl); a4 = va_arg(v, long); va_end(v); }
  GUARD_CALL(openat(d, p, fl, (mode_t)a4))
}
int w_fcntl(int fd, int cmd, ...) {
  long a3; va_list v; va_start(v, cmd); a3 = va_arg(v, long); va_end(v);
  GUARD_CALL(fcntl(fd, cmd, a3))
}

DynLibFunction errno_compat_table[] = {
  {"malloc", (uintptr_t)&w_malloc}, {"calloc", (uintptr_t)&w_calloc}, {"realloc", (uintptr_t)&w_realloc},
  {"mmap", (uintptr_t)&w_mmap}, {"munmap", (uintptr_t)&w_munmap},
  {"close", (uintptr_t)&w_close}, {"read", (uintptr_t)&w_read}, {"write", (uintptr_t)&w_write},
  {"lseek", (uintptr_t)&w_lseek}, {"access", (uintptr_t)&w_access},
  {"fopen", (uintptr_t)&w_fopen}, {"fclose", (uintptr_t)&w_fclose},
  {"fread", (uintptr_t)&w_fread}, {"fwrite", (uintptr_t)&w_fwrite},
  {"fseek", (uintptr_t)&w_fseek}, {"fseeko", (uintptr_t)&w_fseeko}, {"ftell", (uintptr_t)&w_ftell},
  {"fileno", (uintptr_t)&w_fileno}, {"fdopen", (uintptr_t)&w_fdopen},
  {"opendir", (uintptr_t)&w_opendir}, {"readdir", (uintptr_t)&w_readdir}, {"closedir", (uintptr_t)&w_closedir},
  {"mkdir", (uintptr_t)&w_mkdir}, {"remove", (uintptr_t)&w_remove}, {"unlink", (uintptr_t)&w_unlink},
  {"rename", (uintptr_t)&w_rename},
  {"open", (uintptr_t)&w_open}, {"openat", (uintptr_t)&w_openat}, {"fcntl", (uintptr_t)&w_fcntl},
};
const int errno_compat_count = sizeof(errno_compat_table) / sizeof(errno_compat_table[0]);
