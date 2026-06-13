/* fixpak.c — conserta os paks do DYSMANTLE no DEVICE (1ª vez, via launcher).
 * APKs modados deixam os .jpg/.png VAZIOS no pak, só o irmão "<nome>.jpg.ktx"
 * (ETC2 zlib-comprimido) tem dados. Aqui: decodifica o ETC2 → RGBA → reencoda
 * JPEG (slots .jpg) / PNG (slots .png) e preenche os slots vazios (anexa no fim
 * + reescreve o índice). Mesma lógica do tools/fix_empty_textures.py, em C.
 *
 * Formato pak: "PAK\0V11\0"(8) + idx_offset(u32) + filesize(u32);
 *   índice @idx_offset: por entrada = nome\0 + offset(u32)+size(u32)+hash+pad.
 * Libs do device via dlopen: libz (uncompress/compress2/crc32) + libturbojpeg.
 * Uso: fixpak <pak> [<pak2> ...]    (idempotente: pula slots já preenchidos)
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern unsigned char *etc2_decode_rgba(unsigned fmt, int w, int h,
                                       const void *data, int size);

/* ---- libz via dlopen ---- */
static int (*z_uncompress)(unsigned char *, unsigned long *, const unsigned char *, unsigned long);
static int (*z_compress2)(unsigned char *, unsigned long *, const unsigned char *, unsigned long, int);
static unsigned long (*z_crc32)(unsigned long, const unsigned char *, unsigned);
static unsigned long (*z_compressBound)(unsigned long);
/* ---- libturbojpeg via dlopen ---- */
static void *(*tj_init)(void);
static int (*tj_compress2)(void *, const unsigned char *, int, int, int, int,
                           unsigned char **, unsigned long *, int, int, int);
static void (*tj_free)(unsigned char *);
static int (*tj_destroy)(void *);
static void *g_tj;

#define TJPF_RGBA 7
#define TJSAMP_420 2

static int load_libs(void) {
  void *z = dlopen("libz.so.1", RTLD_NOW) ?: dlopen("libz.so", RTLD_NOW);
  void *t = dlopen("libturbojpeg.so.0", RTLD_NOW) ?: dlopen("libturbojpeg.so", RTLD_NOW);
  if (!z || !t) { fprintf(stderr, "fixpak: dlopen libz=%p tj=%p\n", z, t); return -1; }
  z_uncompress = dlsym(z, "uncompress");
  z_compress2 = dlsym(z, "compress2");
  z_crc32 = dlsym(z, "crc32");
  z_compressBound = dlsym(z, "compressBound");
  tj_init = dlsym(t, "tjInitCompress");
  tj_compress2 = dlsym(t, "tjCompress2");
  tj_free = dlsym(t, "tjFree");
  tj_destroy = dlsym(t, "tjDestroy");
  if (!z_uncompress || !z_compress2 || !z_crc32 || !tj_init || !tj_compress2) {
    fprintf(stderr, "fixpak: dlsym falhou\n"); return -1;
  }
  g_tj = tj_init();
  return 0;
}

/* KTX (descomprimido) → ETC2 RGBA. Retorna RGBA (free do caller) + w/h/alpha. */
static unsigned char *ktx_to_rgba(const unsigned char *ktx, int klen, int *ow,
                                  int *oh, int *has_alpha) {
  if (klen < 64 || memcmp(ktx, "\xabKTX 11\xbb\r\n\x1a\n", 12) != 0) return NULL;
  uint32_t glIntFmt = *(const uint32_t *)(ktx + 28);
  uint32_t w = *(const uint32_t *)(ktx + 36);
  uint32_t h = *(const uint32_t *)(ktx + 40);
  uint32_t kvlen = *(const uint32_t *)(ktx + 60);
  size_t q = 64 + kvlen;
  if (q + 4 > (size_t)klen) return NULL;
  uint32_t isz = *(const uint32_t *)(ktx + q);
  q += 4;
  if (q + isz > (size_t)klen) return NULL;
  *ow = (int)w; *oh = (int)h;
  *has_alpha = (glIntFmt == 0x9278 || glIntFmt == 0x9279 || glIntFmt == 0x9276 ||
                glIntFmt == 0x9277);
  return etc2_decode_rgba(glIntFmt, (int)w, (int)h, ktx + q, (int)isz);
}

/* RGBA → PNG (na heap; *plen). Encoder mínimo: IHDR+IDAT(zlib)+IEND. */
static unsigned char *rgba_to_png(const unsigned char *rgba, int w, int h, long *plen) {
  long raw = (long)h * (w * 4 + 1);
  unsigned char *filt = malloc(raw);
  if (!filt) return NULL;
  for (int y = 0; y < h; y++) {
    filt[y * (w * 4 + 1)] = 0; /* filter None */
    memcpy(filt + y * (w * 4 + 1) + 1, rgba + (long)y * w * 4, w * 4);
  }
  unsigned long cb = z_compressBound ? z_compressBound(raw) : raw + raw / 2 + 64;
  unsigned char *idat = malloc(cb);
  if (!idat) { free(filt); return NULL; }
  if (z_compress2(idat, &cb, filt, raw, 6) != 0) { free(filt); free(idat); return NULL; }
  free(filt);
  long cap = 8 + 25 + (12 + (long)cb) + 12;
  unsigned char *out = malloc(cap);
  if (!out) { free(idat); return NULL; }
  long o = 0;
  memcpy(out, "\x89PNG\r\n\x1a\n", 8); o = 8;
#define PUT32(v) do{out[o++]=(v)>>24;out[o++]=(v)>>16;out[o++]=(v)>>8;out[o++]=(v);}while(0)
  /* IHDR */
  PUT32(13); long c0 = o; memcpy(out + o, "IHDR", 4); o += 4;
  PUT32((unsigned)w); PUT32((unsigned)h);
  out[o++] = 8; out[o++] = 6; out[o++] = 0; out[o++] = 0; out[o++] = 0;
  { unsigned long c = z_crc32(0, out + c0, (unsigned)(o - c0)); PUT32((unsigned)c); }
  /* IDAT */
  PUT32((unsigned)cb); long c1 = o; memcpy(out + o, "IDAT", 4); o += 4;
  memcpy(out + o, idat, cb); o += cb;
  { unsigned long c = z_crc32(0, out + c1, (unsigned)(4 + cb)); PUT32((unsigned)c); }
  free(idat);
  /* IEND */
  PUT32(0); long c2 = o; memcpy(out + o, "IEND", 4); o += 4;
  { unsigned long c = z_crc32(0, out + c2, 4); PUT32((unsigned)c); }
#undef PUT32
  *plen = o;
  return out;
}

typedef struct { char name[256]; long fpos; uint32_t off, size; } Ent;

/* lê `n` bytes de `f` no offset `off` para `buf` */
static int pread_at(FILE *f, long off, void *buf, long n) {
  if (fseek(f, off, SEEK_SET) != 0) return -1;
  return fread(buf, 1, n, f) == (size_t)n ? 0 : -1;
}

static int fix_one(const char *path) {
  /* 🪶 STREAMING/baixa-memória: NUNCA carrega o pak inteiro (eram 549MB ->
   * OOM-kill em device de 1GB). Lê só o índice + cada .ktx sob demanda; copia
   * o resto em blocos de 1MB. Pico de RAM ~ 1 textura (~3MB). */
  FILE *f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "fixpak: abrir %s falhou\n", path); return -1; }
  fseek(f, 0, SEEK_END); long flen = ftell(f);
  unsigned char hdr[12];
  if (pread_at(f, 0, hdr, 12) != 0 || memcmp(hdr, "PAK\0V11\0", 8) != 0) {
    fprintf(stderr, "fixpak: %s nao e pak\n", path); fclose(f); return -1;
  }
  uint32_t idx = *(uint32_t *)(hdr + 8);
  long idxlen = flen - idx;
  if (idxlen <= 0 || idxlen > 64L * 1024 * 1024) { fprintf(stderr, "fixpak: indice invalido %s\n", path); fclose(f); return -1; }

  /* índice na RAM (pequeno: ~1-2MB) */
  unsigned char *idxbuf = malloc(idxlen);
  if (!idxbuf || pread_at(f, idx, idxbuf, idxlen) != 0) { fprintf(stderr, "fixpak: ler indice %s\n", path); fclose(f); free(idxbuf); return -1; }

  /* parse entradas (offsets relativos ao idxbuf) */
  long cap = 4096; Ent *ent = malloc(cap * sizeof(Ent)); int ne = 0;
  long p = 0;
  while (p < idxlen - 16) {
    long e = p; while (e < idxlen && idxbuf[e]) e++;
    if (e >= idxlen || e == p || e - p > 250) break;
    if (ne >= cap) { cap *= 2; ent = realloc(ent, cap * sizeof(Ent)); }
    int n = (int)(e - p);
    memcpy(ent[ne].name, idxbuf + p, n); ent[ne].name[n] = 0;
    ent[ne].fpos = e + 1;  /* rel ao idxbuf */
    ent[ne].off = *(uint32_t *)(idxbuf + e + 1);
    ent[ne].size = *(uint32_t *)(idxbuf + e + 5);
    ne++;
    p = e + 1 + 16;
  }

  long fixed = 0;
  char tmp[512]; snprintf(tmp, sizeof(tmp), "%s.fixtmp", path);
  FILE *out = fopen(tmp, "wb");
  if (!out) { fprintf(stderr, "fixpak: criar %s\n", tmp); fclose(f); free(idxbuf); free(ent); return -1; }

  /* copia [0..idx) em blocos de 1MB (dados originais, verbatim) */
  { unsigned char *cp = malloc(1 << 20); long left = idx, o = 0;
    while (left > 0) { long c = left < (1 << 20) ? left : (1 << 20);
      if (pread_at(f, o, cp, c) != 0) { fprintf(stderr, "fixpak: COPIA FALHOU em o=%ld c=%ld (idx=%u flen=%ld)\n", o, c, idx, flen); c = 0; }
      if (c <= 0) break;
      size_t wr = fwrite(cp, 1, c, out);
      if (wr != (size_t)c) { fprintf(stderr, "fixpak: WRITE curto wr=%zu c=%ld o=%ld (disco cheio?)\n", wr, c, o); fclose(out); fclose(f); free(cp); free(idxbuf); free(ent); remove(tmp); return -1; }
      o += c; left -= c; }
    free(cp);
    if (o != idx) { fprintf(stderr, "fixpak: copia incompleta (%ld/%u) — abortando, pak intacto\n", o, idx); fclose(out); fclose(f); free(idxbuf); free(ent); remove(tmp); return -1; }
  }
  long blob_base = idx, running = 0;

  for (int i = 0; i < ne; i++) {
    char *nm = ent[i].name; int L = (int)strlen(nm);
    if (ent[i].size != 0) continue;
    if (!(L > 4 && (!strcmp(nm + L - 4, ".jpg") || !strcmp(nm + L - 4, ".png")))) continue;
    char kn[260]; snprintf(kn, sizeof(kn), "%s.ktx", nm);
    int ki = -1;
    for (int j = 0; j < ne; j++) if (!strcmp(ent[j].name, kn)) { ki = j; break; }
    if (ki < 0 || ent[ki].size == 0) continue;
    /* lê o .ktx comprimido do arquivo (só ele) */
    unsigned char *kc = malloc(ent[ki].size);
    if (!kc) continue;
    if (pread_at(f, ent[ki].off, kc, ent[ki].size) != 0) { free(kc); continue; }
    unsigned long usz = (unsigned long)ent[ki].size * 12 + 1024;
    unsigned char *ktx = malloc(usz);
    if (!ktx) { free(kc); continue; }
    while (z_uncompress(ktx, &usz, kc, ent[ki].size) == -5 /*Z_BUF_ERROR*/) {
      usz *= 2; unsigned char *nk = realloc(ktx, usz); if (!nk) { free(ktx); ktx = NULL; break; }
      ktx = nk;
    }
    free(kc);
    if (!ktx) continue;
    int w, h, alpha;
    unsigned char *rgba = ktx_to_rgba(ktx, (int)usz, &w, &h, &alpha);
    free(ktx);
    if (!rgba) continue;
    unsigned char *blob = NULL; long blen = 0; int tjfree = 0;
    if (!strcmp(nm + L - 4, ".png")) {
      blob = rgba_to_png(rgba, w, h, &blen);
    } else {
      unsigned long js = 0; unsigned char *jb = NULL;
      if (tj_compress2(g_tj, rgba, w, w * 4, h, TJPF_RGBA, &jb, &js, TJSAMP_420, 88, 0) == 0) {
        blob = jb; blen = (long)js; tjfree = 1;
      }
    }
    free(rgba);
    if (!blob || blen <= 0) { if (blob && tjfree) tj_free(blob); else free(blob); continue; }
    fwrite(blob, 1, blen, out);
    ent[i].off = (uint32_t)(blob_base + running);
    ent[i].size = (uint32_t)blen;
    running += blen;
    if (tjfree) tj_free(blob); else free(blob);
    fixed++;
  }
  fclose(f);

  /* índice patchado */
  for (int i = 0; i < ne; i++) {
    long rel = ent[i].fpos;
    *(uint32_t *)(idxbuf + rel) = ent[i].off;
    *(uint32_t *)(idxbuf + rel + 4) = ent[i].size;
  }
  fwrite(idxbuf, 1, idxlen, out);
  free(idxbuf);

  long newlen = ftell(out);
  fseek(out, 8, SEEK_SET);
  uint32_t nidx = idx + (uint32_t)running, nlen = (uint32_t)newlen;
  fwrite(&nidx, 4, 1, out); fwrite(&nlen, 4, 1, out);
  fclose(out);
  free(ent);

  if (fixed > 0) {
    if (rename(tmp, path) != 0) { fprintf(stderr, "fixpak: rename %s falhou\n", tmp); return -1; }
    fprintf(stderr, "fixpak: %s -> %ld texturas consertadas (size %ld)\n", path, fixed, newlen);
  } else {
    remove(tmp);
    fprintf(stderr, "fixpak: %s -> nada a consertar\n", path);
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "uso: fixpak <pak>...\n"); return 2; }
  if (load_libs() != 0) return 1;
  for (int i = 1; i < argc; i++) fix_one(argv[i]);
  if (g_tj && tj_destroy) tj_destroy(g_tj);
  return 0;
}
