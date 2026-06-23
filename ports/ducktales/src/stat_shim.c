/*
 * stat_shim.c -- bionic-layout stat/fstat/lstat for glibc host.
 *
 * libducktales was built with the NDK r17c armeabi-v7a headers, whose
 * `struct stat` has a DIFFERENT field layout than glibc's. The engine reads
 * st_size from the bionic offset (48); glibc fstat writes a glibc-layout
 * struct, so st_size lands at the wrong offset -> garbage size -> the package
 * loader allocates a too-small buffer and the FILELINK parser walks off the
 * end (SIGSEGV). We translate glibc stat -> bionic stat here.
 *
 * bionic LP32 (arm) struct stat offsets:
 *   0  unsigned long long st_dev
 *   8  unsigned char __pad0[4]
 *   12 unsigned long __st_ino
 *   16 unsigned int st_mode
 *   20 unsigned int st_nlink
 *   24 uid_t st_uid
 *   28 gid_t st_gid
 *   32 unsigned long long st_rdev
 *   40 unsigned char __pad3[4]
 *   48 long long st_size
 *   56 unsigned long st_blksize
 *   60 (pad)
 *   64 unsigned long long st_blocks
 *   72 timespec st_atim (sec,nsec)
 *   80 timespec st_mtim
 *   88 timespec st_ctim
 *   96 unsigned long long st_ino
 */
#include <sys/stat.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void glibc_to_bionic(const struct stat *g, void *bp) {
  unsigned char *b = (unsigned char *)bp;
  memset(b, 0, 104);
  *(uint64_t *)(b + 0)  = (uint64_t)g->st_dev;
  *(uint32_t *)(b + 12) = (uint32_t)g->st_ino;
  *(uint32_t *)(b + 16) = (uint32_t)g->st_mode;
  *(uint32_t *)(b + 20) = (uint32_t)g->st_nlink;
  *(uint32_t *)(b + 24) = (uint32_t)g->st_uid;
  *(uint32_t *)(b + 28) = (uint32_t)g->st_gid;
  *(uint64_t *)(b + 32) = (uint64_t)g->st_rdev;
  *(int64_t  *)(b + 48) = (int64_t)g->st_size;
  *(uint32_t *)(b + 56) = (uint32_t)g->st_blksize;
  *(uint64_t *)(b + 64) = (uint64_t)g->st_blocks;
  *(int32_t  *)(b + 72) = (int32_t)g->st_atime;
  *(int32_t  *)(b + 80) = (int32_t)g->st_mtime;
  *(int32_t  *)(b + 88) = (int32_t)g->st_ctime;
  *(uint64_t *)(b + 96) = (uint64_t)g->st_ino;
}

int bionic_fstat(int fd, void *bionic_buf) {
  struct stat g;
  int r = fstat(fd, &g);
  if (r == 0 && bionic_buf) glibc_to_bionic(&g, bionic_buf);
  return r;
}

int bionic_stat(const char *path, void *bionic_buf) {
  struct stat g;
  int r = stat(path, &g);
  if (r == 0 && bionic_buf) glibc_to_bionic(&g, bionic_buf);
  return r;
}

int bionic_lstat(const char *path, void *bionic_buf) {
  struct stat g;
  int r = lstat(path, &g);
  if (r == 0 && bionic_buf) glibc_to_bionic(&g, bionic_buf);
  return r;
}

int bionic_fstatat(int dirfd, const char *path, void *bionic_buf, int flags) {
  struct stat g;
  int r = fstatat(dirfd, path, &g, flags);
  if (r == 0 && bionic_buf) glibc_to_bionic(&g, bionic_buf);
  return r;
}
