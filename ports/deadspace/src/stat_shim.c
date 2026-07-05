/*
 * Android LP32 stat layout differs from glibc armhf. The game reads fields
 * from bionic offsets, so translate host stat results into bionic storage.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "imports.h"

static void glibc_to_bionic_stat(const struct stat *g, void *bp) {
  unsigned char *b = (unsigned char *)bp;
  memset(b, 0, 104);
  *(uint64_t *)(b + 0) = (uint64_t)g->st_dev;
  *(uint32_t *)(b + 12) = (uint32_t)g->st_ino;
  *(uint32_t *)(b + 16) = (uint32_t)g->st_mode;
  *(uint32_t *)(b + 20) = (uint32_t)g->st_nlink;
  *(uint32_t *)(b + 24) = (uint32_t)g->st_uid;
  *(uint32_t *)(b + 28) = (uint32_t)g->st_gid;
  *(uint64_t *)(b + 32) = (uint64_t)g->st_rdev;
  *(int64_t *)(b + 48) = (int64_t)g->st_size;
  *(uint32_t *)(b + 56) = (uint32_t)g->st_blksize;
  *(uint64_t *)(b + 64) = (uint64_t)g->st_blocks;
  *(int32_t *)(b + 72) = (int32_t)g->st_atime;
  *(int32_t *)(b + 80) = (int32_t)g->st_mtime;
  *(int32_t *)(b + 88) = (int32_t)g->st_ctime;
  *(uint64_t *)(b + 96) = (uint64_t)g->st_ino;
}

int bionic_fstat(int fd, void *bionic_buf) {
  struct stat g;
  int r = fstat(fd, &g);
  if (r == 0 && bionic_buf) glibc_to_bionic_stat(&g, bionic_buf);
  return r;
}

int bionic_stat(const char *path, void *bionic_buf) {
  struct stat g;
  char resolved[1024];
  deadspace_resolve_read_path(path, resolved, sizeof(resolved));
  int r = stat(resolved[0] ? resolved : path, &g);
  if (r == 0 && bionic_buf) glibc_to_bionic_stat(&g, bionic_buf);
  return r;
}

int bionic_lstat(const char *path, void *bionic_buf) {
  struct stat g;
  char resolved[1024];
  deadspace_resolve_read_path(path, resolved, sizeof(resolved));
  int r = lstat(resolved[0] ? resolved : path, &g);
  if (r == 0 && bionic_buf) glibc_to_bionic_stat(&g, bionic_buf);
  return r;
}

int bionic_fstatat(int dirfd, const char *path, void *bionic_buf, int flags) {
  struct stat g;
  char resolved[1024];
  deadspace_resolve_read_path(path, resolved, sizeof(resolved));
  int r = fstatat(dirfd, resolved[0] ? resolved : path, &g, flags);
  if (r == 0 && bionic_buf) glibc_to_bionic_stat(&g, bionic_buf);
  return r;
}
