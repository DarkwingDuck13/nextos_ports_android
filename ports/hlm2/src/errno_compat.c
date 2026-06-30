/*
 * errno_compat.c — wrappers que PRESERVAM o slot tpidr_el0+0x28 nas funcoes libc
 * de I/O que a libyoyo chama.
 *
 * Por que: a libyoyo (NDK/bionic) le o stack-canary do slot TLS bionic tpidr+0x28.
 * No glibc 2.30 (R36S/ArkOS) o errno mora EXATAMENTE em tpidr+0x28 (32 bits baixos).
 * Quando uma funcao libc seta errno (ex: open de arquivo inexistente -> ENOENT), os
 * 32 bits baixos do "canary" mudam -> o check no epilogo da funcao libyoyo que chamou
 * falha -> __stack_chk_fail -> abort (nao-skipavel). No glibc 2.34 (.79 nativo) o errno
 * fica em outro offset, por isso nativo nao quebra.
 *
 * Fix: cada wrapper salva tpidr+0x28 na entrada, chama o real (glibc, via dlsym), e
 * restaura o slot antes de retornar -> a libyoyo nunca ve o slot corrompido. Em device
 * onde errno NAO esta em 0x28 (nativo) o save/restore e no-op inofensivo.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <fcntl.h>
#include "so_util.h"
#include "shims.h"

#define REAL(name) ({ static void *r_; if (!r_) r_ = dlsym(RTLD_DEFAULT, #name); r_; })
#define GUARD_CALL(expr) \
  unsigned long long __cy = hm_canary_save(); \
  __typeof__(expr) __r = (expr); \
  hm_canary_restore(__cy); \
  return __r;

/* --- fixed-arg --- */
int w_close(int fd) { GUARD_CALL(((int(*)(int))REAL(close))(fd)) }
long w_read(int fd, void *b, unsigned long n) { GUARD_CALL(((long(*)(int,void*,unsigned long))REAL(read))(fd,b,n)) }
long w_write(int fd, const void *b, unsigned long n) { GUARD_CALL(((long(*)(int,const void*,unsigned long))REAL(write))(fd,b,n)) }
long w_lseek(int fd, long off, int wh) { GUARD_CALL(((long(*)(int,long,int))REAL(lseek))(fd,off,wh)) }
int w_access(const char *p, int m) { GUARD_CALL(((int(*)(const char*,int))REAL(access))(p,m)) }
void *w_fopen(const char *p, const char *m) { GUARD_CALL(((void*(*)(const char*,const char*))REAL(fopen))(p,m)) }
int w_fclose(void *f) { GUARD_CALL(((int(*)(void*))REAL(fclose))(f)) }
unsigned long w_fread(void *b, unsigned long s, unsigned long n, void *f) { GUARD_CALL(((unsigned long(*)(void*,unsigned long,unsigned long,void*))REAL(fread))(b,s,n,f)) }
unsigned long w_fwrite(const void *b, unsigned long s, unsigned long n, void *f) { GUARD_CALL(((unsigned long(*)(const void*,unsigned long,unsigned long,void*))REAL(fwrite))(b,s,n,f)) }
int w_fseek(void *f, long o, int w) { GUARD_CALL(((int(*)(void*,long,int))REAL(fseek))(f,o,w)) }
int w_fseeko(void *f, long o, int w) { GUARD_CALL(((int(*)(void*,long,int))REAL(fseeko))(f,o,w)) }
long w_ftell(void *f) { GUARD_CALL(((long(*)(void*))REAL(ftell))(f)) }
int w_fileno(void *f) { GUARD_CALL(((int(*)(void*))REAL(fileno))(f)) }
void *w_fdopen(int fd, const char *m) { GUARD_CALL(((void*(*)(int,const char*))REAL(fdopen))(fd,m)) }
void *w_opendir(const char *p) { GUARD_CALL(((void*(*)(const char*))REAL(opendir))(p)) }
void *w_readdir(void *d) { GUARD_CALL(((void*(*)(void*))REAL(readdir))(d)) }
int w_closedir(void *d) { GUARD_CALL(((int(*)(void*))REAL(closedir))(d)) }
int w_mkdir(const char *p, unsigned int m) { GUARD_CALL(((int(*)(const char*,unsigned int))REAL(mkdir))(p,m)) }
int w_remove(const char *p) { GUARD_CALL(((int(*)(const char*))REAL(remove))(p)) }
int w_unlink(const char *p) { GUARD_CALL(((int(*)(const char*))REAL(unlink))(p)) }
int w_rename(const char *a, const char *b) { GUARD_CALL(((int(*)(const char*,const char*))REAL(rename))(a,b)) }
int w_statfs(const char *p, void *b) { GUARD_CALL(((int(*)(const char*,void*))REAL(statfs))(p,b)) }
int w_fstatfs(int fd, void *b) { GUARD_CALL(((int(*)(int,void*))REAL(fstatfs))(fd,b)) }
int w_statvfs(const char *p, void *b) { GUARD_CALL(((int(*)(const char*,void*))REAL(statvfs))(p,b)) }
int w_fstatvfs(int fd, void *b) { GUARD_CALL(((int(*)(int,void*))REAL(fstatvfs))(fd,b)) }

/* --- variadic (open/openat/fcntl/ioctl): pega o 3o arg como long --- */
int w_open(const char *p, int fl, ...) {
  long a3 = 0; if (fl & O_CREAT) { va_list v; va_start(v, fl); a3 = va_arg(v, long); va_end(v); }
  GUARD_CALL(((int(*)(const char*,int,long))REAL(open))(p, fl, a3))
}
int w_openat(int d, const char *p, int fl, ...) {
  long a4 = 0; if (fl & O_CREAT) { va_list v; va_start(v, fl); a4 = va_arg(v, long); va_end(v); }
  GUARD_CALL(((int(*)(int,const char*,int,long))REAL(openat))(d, p, fl, a4))
}
int w_fcntl(int fd, int cmd, ...) {
  long a3; va_list v; va_start(v, cmd); a3 = va_arg(v, long); va_end(v);
  GUARD_CALL(((int(*)(int,int,long))REAL(fcntl))(fd, cmd, a3))
}
int w_ioctl(int fd, unsigned long req, ...) {
  long a3; va_list v; va_start(v, req); a3 = va_arg(v, long); va_end(v);
  GUARD_CALL(((int(*)(int,unsigned long,long))REAL(ioctl))(fd, req, a3))
}

DynLibFunction errno_compat_table[] = {
  {"close", (uintptr_t)&w_close}, {"read", (uintptr_t)&w_read}, {"write", (uintptr_t)&w_write},
  {"lseek", (uintptr_t)&w_lseek}, {"access", (uintptr_t)&w_access},
  {"fopen", (uintptr_t)&w_fopen}, {"fclose", (uintptr_t)&w_fclose},
  {"fread", (uintptr_t)&w_fread}, {"fwrite", (uintptr_t)&w_fwrite},
  {"fseek", (uintptr_t)&w_fseek}, {"fseeko", (uintptr_t)&w_fseeko}, {"ftell", (uintptr_t)&w_ftell},
  {"fileno", (uintptr_t)&w_fileno}, {"fdopen", (uintptr_t)&w_fdopen},
  {"opendir", (uintptr_t)&w_opendir}, {"readdir", (uintptr_t)&w_readdir}, {"closedir", (uintptr_t)&w_closedir},
  {"mkdir", (uintptr_t)&w_mkdir}, {"remove", (uintptr_t)&w_remove}, {"unlink", (uintptr_t)&w_unlink},
  {"rename", (uintptr_t)&w_rename},
  {"statfs", (uintptr_t)&w_statfs}, {"fstatfs", (uintptr_t)&w_fstatfs},
  {"statvfs", (uintptr_t)&w_statvfs}, {"fstatvfs", (uintptr_t)&w_fstatvfs},
  {"open", (uintptr_t)&w_open}, {"openat", (uintptr_t)&w_openat},
  {"fcntl", (uintptr_t)&w_fcntl}, {"ioctl", (uintptr_t)&w_ioctl},
};
const int errno_compat_count = sizeof(errno_compat_table) / sizeof(errno_compat_table[0]);
