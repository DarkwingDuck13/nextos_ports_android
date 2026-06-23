/*
 * asset_shim.c -- AAssetManager backed by a directory on disk.
 *
 * libducktales only imports AAssetManager_open / AAsset_read / AAsset_close,
 * so a simple FILE*-backed asset is enough. Paths are relative to assets/
 * (DUCK_ASSETS env, default ./assets). Case-insensitive fallback like SOTN.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "util.h"

typedef struct {
  FILE *fp;
  long len;
} DTAsset;

static const char *asset_base(void) {
  const char *b = getenv("DUCK_ASSETS");
  return b ? b : "./assets";
}

/* case-insensitive resolve of a single path component inside dir; writes the
   real name into out. returns 1 on hit. */
static int ci_lookup(const char *dir, const char *want, char *out, size_t outsz) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  struct dirent *e;
  int hit = 0;
  while ((e = readdir(d))) {
    if (strcasecmp(e->d_name, want) == 0) {
      snprintf(out, outsz, "%s", e->d_name);
      hit = 1;
      break;
    }
  }
  closedir(d);
  return hit;
}

/* open <base>/<path> trying exact, then case-insensitive per component */
static FILE *open_asset_path(const char *path) {
  char full[2048];
  snprintf(full, sizeof(full), "%s/%s", asset_base(), path);
  FILE *fp = fopen(full, "rb");
  if (fp) return fp;

  /* case-insensitive walk */
  char cur[2048];
  snprintf(cur, sizeof(cur), "%s", asset_base());
  char tmp[2048];
  snprintf(tmp, sizeof(tmp), "%s", path);
  char *save = NULL;
  char *tok = strtok_r(tmp, "/", &save);
  while (tok) {
    char real[512];
    if (ci_lookup(cur, tok, real, sizeof(real))) {
      char nxt[2048];
      snprintf(nxt, sizeof(nxt), "%s/%s", cur, real);
      snprintf(cur, sizeof(cur), "%s", nxt);
    } else {
      char nxt[2048];
      snprintf(nxt, sizeof(nxt), "%s/%s", cur, tok);
      snprintf(cur, sizeof(cur), "%s", nxt);
    }
    tok = strtok_r(NULL, "/", &save);
  }
  return fopen(cur, "rb");
}

void *AAssetManager_open(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  if (!filename) return NULL;
  /* debug: skip fmod sound banks to isolate audio crashes from rendering */
  if (getenv("DUCK_NOFSB")) {
    size_t l = strlen(filename);
    if (l > 4 && strcasecmp(filename + l - 4, ".fsb") == 0) {
      debugPrintf("[asset] SKIP (NOFSB) %s\n", filename);
      return NULL;
    }
  }
  { const char *skip = getenv("DUCK_SKIPFILE");   /* comma-separated substrings */
    if (skip && strstr(filename, skip)) {
      debugPrintf("[asset] SKIP (SKIPFILE) %s\n", filename);
      return NULL;
    }
  }
  /* redirect a problematic asset to a smaller valid one (DUCK_REDIR=from:to) */
  { const char *rd = getenv("DUCK_REDIR");
    if (rd) {
      char buf[256]; snprintf(buf, sizeof(buf), "%s", rd);
      char *colon = strchr(buf, ':');
      if (colon) { *colon = 0;
        if (strstr(filename, buf)) {
          debugPrintf("[asset] REDIR %s -> %s\n", filename, colon + 1);
          filename = colon + 1;
        }
      }
    }
  }
  FILE *fp = open_asset_path(filename);
  static int n = 0;
  if (!fp) {
    if (n++ < 40) debugPrintf("[asset] MISS %s\n", filename);
    return NULL;
  }
  if (n++ < 40) debugPrintf("[asset] open %s\n", filename);
  DTAsset *a = (DTAsset *)calloc(1, sizeof(DTAsset));
  a->fp = fp;
  fseek(fp, 0, SEEK_END);
  a->len = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  return a;
}

int AAsset_read(void *asset, void *buf, size_t count) {
  DTAsset *a = (DTAsset *)asset;
  if (!a || !a->fp) return -1;
  size_t r = fread(buf, 1, count, a->fp);
  return (int)r;
}

void AAsset_close(void *asset) {
  DTAsset *a = (DTAsset *)asset;
  if (!a) return;
  if (a->fp) fclose(a->fp);
  free(a);
}

/* extras in case the engine reaches for them */
long AAsset_getLength(void *asset) {
  DTAsset *a = (DTAsset *)asset;
  return a ? a->len : 0;
}

int AAsset_seek(void *asset, long off, int whence) {
  DTAsset *a = (DTAsset *)asset;
  if (!a || !a->fp) return -1;
  if (fseek(a->fp, off, whence) != 0) return -1;
  return (int)ftell(a->fp);
}

const void *AAsset_getBuffer(void *asset) {
  (void)asset;
  return NULL; /* force read() path */
}
