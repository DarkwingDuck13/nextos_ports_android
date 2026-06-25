/*
 * android_shim.c -- Fake Android NDK environment for LEGO Batman 3 (Fusion).
 *
 * AAssetManager: the game's assets live inside the apkvision data01/data02
 * ZIP archives (all entries STORED / uncompressed), laid out as Play Asset
 * Delivery packs: files/assetpacks/<pack>/<ver>/<ver>/assets/<file>.
 * We index those archives once and serve each asset by direct offset read,
 * so nothing is extracted or inflated. Lookup key = path after "/assets/".
 *
 * ANativeWindow: returns screen dimensions.
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android_shim.h"
#include "util.h"

#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

static char g_data_path[512] = ".";

void android_shim_set_data_path(const char *path) {
  strncpy(g_data_path, path, sizeof(g_data_path) - 1);
  g_data_path[sizeof(g_data_path) - 1] = '\0';
}

/* ---- ZIP index over data01/data02 (STORED entries only) ---- */
#define MAX_ZIPS 4
#define MAX_ENTRIES 2048

typedef struct {
  char key[256];   /* asset path after the last "/assets/" (the AAsset name) */
  char rel[256];   /* path after leading "files/" (alt key) */
  int zip;         /* index into g_zip_fp */
  long data_off;   /* byte offset of the entry payload in the zip */
  long size;       /* payload length */
} ZipEntry;

static FILE *g_zip_fp[MAX_ZIPS];
static char g_zip_path[MAX_ZIPS][1024];
static int g_num_zips = 0;
static ZipEntry g_entries[MAX_ENTRIES];
static int g_num_entries = 0;

static uint32_t rd32(const unsigned char *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }

/* compute payload offset by reading the local file header */
static long zip_data_offset(FILE *fp, long local_hdr) {
  unsigned char lh[30];
  if (fseek(fp, local_hdr, SEEK_SET) != 0) return -1;
  if (fread(lh, 1, 30, fp) != 30) return -1;
  if (rd32(lh) != 0x04034b50) return -1;
  uint16_t namelen = rd16(lh + 26);
  uint16_t extralen = rd16(lh + 28);
  return local_hdr + 30 + namelen + extralen;
}

static void set_keys(ZipEntry *e, const char *name) {
  /* key = part after the last "/assets/" */
  const char *a = NULL, *p = name;
  while ((p = strstr(p, "/assets/")) != NULL) { a = p + 8; p += 1; }
  snprintf(e->key, sizeof(e->key), "%s", a ? a : name);
  /* rel = part after leading "files/" */
  const char *r = name;
  if (strncmp(r, "files/", 6) == 0) r += 6;
  snprintf(e->rel, sizeof(e->rel), "%s", r);
}

int android_shim_add_zip(const char *path) {
  FILE *fp = fopen(path, "rb");
  if (!fp) { debugPrintf("zip: cannot open %s\n", path); return -1; }
  if (g_num_zips >= MAX_ZIPS) { fclose(fp); return -1; }
  int zid = g_num_zips;

  /* locate End Of Central Directory (scan last 64KB) */
  fseek(fp, 0, SEEK_END);
  long fsz = ftell(fp);
  long scan = fsz > 65557 ? fsz - 65557 : 0;
  long buflen = fsz - scan;
  unsigned char *buf = malloc(buflen);
  fseek(fp, scan, SEEK_SET);
  if (fread(buf, 1, buflen, fp) != (size_t)buflen) { free(buf); fclose(fp); return -1; }
  long eocd = -1;
  for (long i = buflen - 22; i >= 0; i--) {
    if (buf[i] == 0x50 && rd32(buf + i) == 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) { debugPrintf("zip: no EOCD in %s\n", path); free(buf); fclose(fp); return -1; }
  uint32_t cd_off = rd32(buf + eocd + 16);
  uint16_t total = rd16(buf + eocd + 10);
  free(buf);

  /* walk the central directory */
  long pos = cd_off;
  int added = 0;
  for (int i = 0; i < total && g_num_entries < MAX_ENTRIES; i++) {
    unsigned char cd[46];
    if (fseek(fp, pos, SEEK_SET) != 0) break;
    if (fread(cd, 1, 46, fp) != 46) break;
    if (rd32(cd) != 0x02014b50) break;
    uint16_t method = rd16(cd + 10);
    uint32_t comp = rd32(cd + 20);
    uint16_t namelen = rd16(cd + 28);
    uint16_t extralen = rd16(cd + 30);
    uint16_t commentlen = rd16(cd + 32);
    uint32_t local_hdr = rd32(cd + 42);
    char name[512];
    long nlen = namelen < (int)sizeof(name) - 1 ? namelen : (int)sizeof(name) - 1;
    if (fread(name, 1, nlen, fp) != (size_t)nlen) break;
    name[nlen] = 0;
    if (namelen > nlen) fseek(fp, namelen - nlen, SEEK_CUR);
    pos += 46 + namelen + extralen + commentlen;

    if (name[0] == 0 || name[nlen - 1] == '/') continue; /* skip dirs */
    if (method != 0) { /* only STORED supported */
      debugPrintf("zip: skip non-stored %s (method=%u)\n", name, method);
      continue;
    }
    long doff = zip_data_offset(fp, local_hdr);
    if (doff < 0) continue;
    ZipEntry *e = &g_entries[g_num_entries++];
    e->zip = zid; e->data_off = doff; e->size = comp;
    set_keys(e, name);
    added++;
  }
  g_zip_fp[zid] = fp;
  snprintf(g_zip_path[zid], sizeof(g_zip_path[zid]), "%s", path);
  g_num_zips++;
  debugPrintf("zip: indexed %s: %d entries (total %d)\n", path, added, g_num_entries);
  return added;
}

static ZipEntry *zip_find(const char *name) {
  if (!name) return NULL;
  /* strip a leading "assets/" or "./" the engine may prepend */
  if (strncmp(name, "./", 2) == 0) name += 2;
  if (strncmp(name, "assets/", 7) == 0) name += 7;
  for (int i = 0; i < g_num_entries; i++) {
    if (strcmp(g_entries[i].key, name) == 0) return &g_entries[i];
  }
  for (int i = 0; i < g_num_entries; i++) {
    if (strcmp(g_entries[i].rel, name) == 0) return &g_entries[i];
  }
  /* last resort: bare basename match */
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  for (int i = 0; i < g_num_entries; i++) {
    const char *ek = strrchr(g_entries[i].key, '/');
    ek = ek ? ek + 1 : g_entries[i].key;
    if (strcmp(ek, base) == 0) return &g_entries[i];
  }
  return NULL;
}

/* ---- AAssetManager ---- */
typedef struct {
  FILE *fp;      /* owned (filesystem) or shared (zip) */
  int owns_fp;
  long base;     /* start offset of payload within the source file */
  long size;     /* payload length */
  long cursor;   /* read cursor relative to base */
  char src[1024];/* source file path (zip or fs) for fresh fd via open() */
} FakeAsset;

void *AAssetManager_open_fake(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;

  ZipEntry *e = zip_find(filename);
  if (e) {
    debugPrintf("AAsset_open HIT: %s (size=%ld)\n", filename ? filename : "?", e->size);
    FakeAsset *a = calloc(1, sizeof(FakeAsset));
    a->fp = g_zip_fp[e->zip];
    a->owns_fp = 0;
    a->base = e->data_off;
    a->size = e->size;
    a->cursor = 0;
    snprintf(a->src, sizeof(a->src), "%s", g_zip_path[e->zip]);
    return a;
  }

  /* filesystem fallback */
  char path[1024];
  if (filename && filename[0] == '/')
    snprintf(path, sizeof(path), "%s", filename);
  else
    snprintf(path, sizeof(path), "%s/%s", g_data_path, filename);
  const char *resolved = resolve_android_path(path);
  FILE *fp = fopen(resolved, "rb");
  if (!fp) {
    debugPrintf("AAsset_open: MISS %s\n", filename ? filename : "(null)");
    return NULL;
  }
  FakeAsset *a = calloc(1, sizeof(FakeAsset));
  a->fp = fp; a->owns_fp = 1; a->base = 0; a->cursor = 0;
  snprintf(a->src, sizeof(a->src), "%s", resolved);
  fseek(fp, 0, SEEK_END); a->size = ftell(fp); fseek(fp, 0, SEEK_SET);
  return a;
}

void AAsset_close_fake(void *asset) {
  if (!asset) return;
  FakeAsset *a = (FakeAsset *)asset;
  if (a->owns_fp && a->fp) fclose(a->fp);
  free(a);
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  if (!asset) return -1;
  FakeAsset *a = (FakeAsset *)asset;
  long remain = a->size - a->cursor;
  if (remain <= 0) return 0;
  if ((long)count > remain) count = remain;
  fseek(a->fp, a->base + a->cursor, SEEK_SET);
  size_t n = fread(buf, 1, count, a->fp);
  a->cursor += (long)n;
  return (int)n;
}

long AAsset_getLength_fake(void *asset) {
  if (!asset) return 0;
  return ((FakeAsset *)asset)->size;
}

long AAsset_seek_fake(void *asset, long offset, int whence) {
  if (!asset) return -1;
  FakeAsset *a = (FakeAsset *)asset;
  long ncur = a->cursor;
  if (whence == SEEK_SET) ncur = offset;
  else if (whence == SEEK_CUR) ncur += offset;
  else if (whence == SEEK_END) ncur = a->size + offset;
  if (ncur < 0) ncur = 0;
  if (ncur > a->size) ncur = a->size;
  a->cursor = ncur;
  return ncur;
}

/* The Fusion engine opens assets via AAsset_openFileDescriptor + fdopen, then
   fseeks to outStart. Our assets are byte ranges inside data01/data02 (or a
   loose file), so we hand back a FRESH fd on the source file plus the payload
   [start,length]. A fresh fd (not dup) gives the caller an independent file
   offset. NOTE: returning -1 here makes the engine fdopen(-1)=NULL -> crash. */
int AAsset_openFileDescriptor_fake(void *asset, long *outStart, long *outLength) {
  if (!asset) return -1;
  FakeAsset *a = (FakeAsset *)asset;
  int fd = open(a->src, O_RDONLY);
  if (fd < 0) {
    debugPrintf("AAsset_openFileDescriptor: open(%s) failed\n", a->src);
    return -1;
  }
  if (outStart) *outStart = a->base;
  if (outLength) *outLength = a->size;
  return fd;
}

/* ---- ANativeWindow ---- */
static char fake_native_window[8192] __attribute__((aligned(64)));

void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env; (void)surface;
  return fake_native_window;
}
int ANativeWindow_getWidth_fake(void *window) { (void)window; return SCREEN_WIDTH; }
int ANativeWindow_getHeight_fake(void *window) { (void)window; return SCREEN_HEIGHT; }
int ANativeWindow_setBuffersGeometry_fake(void *window, int w, int h, int fmt) {
  (void)window; (void)w; (void)h; (void)fmt; return 0;
}
