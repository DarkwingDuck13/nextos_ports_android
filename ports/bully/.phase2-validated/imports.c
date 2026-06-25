/* imports.c (DEVICE) -- shims bionic/NDK do libGame.so como tabela DynLibFunction.
 * Igual aos shims validados no PC; aqui expostos como bully_stub_table[] p/ o
 * so_resolve do so_util AArch64 (fallback dlsym pega libc/GLES/EGL/openal/mpg123
 * do device). Ponte pthread bionic->glibc vem do pthread_bridge.c. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include "so_util.h"
#include "jni_shim.h"
#include "zip_fs.h"

/* caminho do .tex atualmente aberto (chave da sidetable ETC1; setado em nv_open).
 * Espelho do bk_last_bmp_name do dysmantle. */
char bully_cur_tex_path[256];
void bully_set_tex_path(const char *p) {
  if (!p) { bully_cur_tex_path[0] = '\0'; return; }
  size_t n = strlen(p); if (n >= sizeof(bully_cur_tex_path)) n = sizeof(bully_cur_tex_path) - 1;
  memcpy(bully_cur_tex_path, p, n); bully_cur_tex_path[n] = '\0';
}

/* ===================== CACHE ETC1 OFFLINE (bakeado, embarcado) =====================
 * O engine decodifica o .tex (formato console tiled/swizzled, fechado) e sobe RGBA/16-bit
 * via glTexImage2D -- e ele NAO tem caminho ETC1. Entao: bakeamos o ETC1 do NOSSO lado
 * (BULLY_BAKE=1 captura os pixels decodificados full-res e grava por nome do asset) e
 * embarcamos o cache no pacote (a textura e identica p/ todos -- mesmo APK 1.4.311).
 * No device do usuario: so LE ETC1 pronto e sobe GL_ETC1_RGB8_OES (4x menos VRAM ->
 * resolve o limite de MMU do Utgard) -- ZERO conversao/encode em runtime, zero engasgo.
 * So texturas OPACAS 565 (sem alpha); 4444/8888/luminance caem no caminho original. */
#include "etc1_encode.h"
#define GL_ETC1_RGB8_OES 0x8D64
static const char *bully_etc1_dir(void) {
  static const char *d = NULL; static int got = 0;
  if (!got) { d = getenv("BULLY_ETC1CACHE"); got = 1; }
  return d;
}
static int bully_etc1_bake(void) { static int m = -1; if (m < 0) m = getenv("BULLY_BAKE") ? 1 : 0; return m; }
/* BULLY_ETC1_FORCE: usar ETC1 TAMBEM no KMSDRM (devices KMSDRM <=1.2GB, ex R36S 1GB, que
 * PRECISAM da economia de VRAM). Nesses, tratamos a textura como no fbdev: MIN_FILTER=
 * LINEAR + SEM mipmap (o cache ETC1 so tem o nivel base). Setado pelo launcher por RAM. */
static int bully_etc1_force(void) { static int m = -1; if (m < 0) m = getenv("BULLY_ETC1_FORCE") ? 1 : 0; return m; }
/* BULLY_TRILINEAR: TESTE de qualidade no fbdev/Mali-450 -- DESLIGA o ETC1 e deixa o
 * trilinear+mipmaps do jogo passar (igual kmsdrm). Custa ~2.6x VRAM -> pode OOM em 1GB. */
static int bully_trilinear(void) { static int m = -1; if (m < 0) m = getenv("BULLY_TRILINEAR") ? 1 : 0; return m; }
/* TESTE: ligar mipmap tb nos RECORTES (arvores/cercas) -> some a cintilacao. Risco: halo preto na borda
 * se a arte tiver alpha com RGB preto. Gate p/ A/B sem rebuild. */
static int bully_cutout_mip(void) { static int m = -1; if (m < 0) m = getenv("BULLY_CUTOUT_MIP") ? 1 : 0; return m; }
static void bully_etc1_key(char *out, size_t n, const char *path, int w, int h) {
  char san[200]; size_t j = 0;
  for (const char *p = path; *p && j < sizeof(san) - 1; p++) san[j++] = (*p == '/' || *p == '\\') ? '_' : *p;
  san[j] = '\0';
  snprintf(out, n, "%s/%s_%dx%d.etc1", bully_etc1_dir(), san, w, h);
}
/* 565 (u16 LE) -> RGB888 */
static void bully_expand565(const unsigned char *s, int w, int h, unsigned char *rgb) {
  for (int i = 0; i < w * h; i++) {
    unsigned v = s[2*i] | (s[2*i+1] << 8);
    rgb[3*i]   = (unsigned char)((((v >> 11) & 31) * 255 + 15) / 31);
    rgb[3*i+1] = (unsigned char)((((v >> 5) & 63) * 255 + 31) / 63);
    rgb[3*i+2] = (unsigned char)(((v & 31) * 255 + 15) / 31);
  }
}
extern unsigned char *etc2_decode_rgba(unsigned, int, int, const void *, int);
/* VERIFICACAO DE CONTEUDO (anti-roxo): decodifica ~24 blocos do ETC1 do cache e compara
 * com os pixels 565 que o engine ia subir. MAD alto = o cache foi salvo no nome errado
 * (corrida no bake) -> rejeita e usa a textura original. Igual o anti-magenta do dysmantle. */
static int bully_etc1_verify(const unsigned char *blob, int w, int h, const unsigned char *px565) {
  int bw = w / 4, bh = h / 4; if (bw <= 0 || bh <= 0 || !px565) return -1;
  int gx = bw < 5 ? bw : 5, gy = bh < 5 ? bh : 5; long sum = 0, cnt = 0; int sampled = 0;
  for (int iy = 0; iy < gy; iy++) for (int ix = 0; ix < gx; ix++) {
    int bx = (int)((long)ix * (bw - 1) / (gx > 1 ? gx - 1 : 1));
    int by = (int)((long)iy * (bh - 1) / (gy > 1 ? gy - 1 : 1));
    unsigned char *dec = etc2_decode_rgba(0x9274, 4, 4, blob + ((long)by * bw + bx) * 8, 8);
    if (!dec) continue; sampled++;
    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
      long p = (long)(by * 4 + j) * w + (bx * 4 + i);
      unsigned v = px565[2 * p] | (px565[2 * p + 1] << 8);
      int sr = (((v >> 11) & 31) * 255 + 15) / 31, sg = (((v >> 5) & 63) * 255 + 31) / 63, sb = ((v & 31) * 255 + 15) / 31;
      const unsigned char *d = dec + (j * 4 + i) * 4;
      int dr = d[0] - sr, dg = d[1] - sg, db = d[2] - sb;
      sum += (dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db); cnt += 3;
    }
    free(dec);
  }
  if (!sampled || !cnt) return -1;
  return (int)(sum / cnt);
}
static unsigned (*real_glCompressedTexImage2D2)(unsigned, int, unsigned, int, int, int, int, const void *) = NULL;
extern int bully_is_kmsdrm(void);
/* retorna 1 se tratou (subiu ETC1 do cache, ou pulou mip de textura ja-ETC1). */
static int bully_try_etc1(unsigned tgt, int lvl, int w, int h, unsigned fmt, unsigned type, const void *px) {
  static int cur_cached = 0;            /* base atual virou ETC1? -> pula seus mips */
  if (!bully_etc1_dir()) return 0;      /* cache desligado */
  if (bully_trilinear()) return 0;      /* TESTE trilinear: sem ETC1 (RGBA+mips) */
  if (bully_is_kmsdrm() && !bully_etc1_force()) return 0;  /* fbdev sempre; kmsdrm so com FORCE (low-RAM) */
  if (lvl > 0) return cur_cached;       /* pula mips da textura cacheada */
  cur_cached = 0;
  if (type != 0x8363 || fmt != 0x1907) return 0;          /* SO 565 opaco (RGB) */
  if (w < 64 || h < 64 || w > 2048 || h > 2048 || (w & 3) || (h & 3)) return 0;
  if (!bully_cur_tex_path[0]) return 0;
  char key[300]; bully_etc1_key(key, sizeof(key), bully_cur_tex_path, w, h);
  size_t etcsz = (size_t)(w / 4) * (h / 4) * 8;
  if (!real_glCompressedTexImage2D2) real_glCompressedTexImage2D2 = dlsym(RTLD_DEFAULT, "glCompressedTexImage2D");
  FILE *f = fopen(key, "rb");
  if (f) {                              /* RUNTIME: sobe o ETC1 pronto */
    unsigned char *buf = malloc(etcsz);
    size_t r = buf ? fread(buf, 1, etcsz, f) : 0; fclose(f);
    if (buf && r == etcsz && real_glCompressedTexImage2D2) {
      /* anti-roxo: so sobe o ETC1 se ele BATE com a textura que o engine ia subir. */
      static int vmax = -1; if (vmax < 0) { const char *e = getenv("BULLY_VERIFY_MAX"); vmax = e ? atoi(e) : 34; }
      int mad = (px && type == 0x8363) ? bully_etc1_verify(buf, w, h, (const unsigned char *)px) : 0;
      if (mad > vmax) {  /* cache no nome errado (corrida do bake) -> usa a original */
        static int rj = 0; if (rj < 12) { fprintf(stderr, "[etc1] REJEITA '%s' %dx%d mad=%d (usa original)\n", bully_cur_tex_path, w, h, mad); rj++; }
        free(buf); return 0;
      }
      real_glCompressedTexImage2D2(tgt, 0, GL_ETC1_RGB8_OES, w, h, 0, (int)etcsz, buf);
      free(buf); cur_cached = 1;
      static int up = 0; if (up < 8) { fprintf(stderr, "[etc1] up '%s' %dx%d\n", bully_cur_tex_path, w, h); up++; }
      return 1;
    }
    free(buf); return 0;
  }
  if (bully_etc1_bake() && px) {        /* BAKE (nosso lado): encoda full-res e grava */
    unsigned char *rgb = malloc((size_t)w * h * 3);
    unsigned char *etc = malloc(etcsz);
    if (rgb && etc) {
      bully_expand565(px, w, h, rgb);
      etc1_encode_image(rgb, w, h, 3, etc);
      FILE *wf = fopen(key, "wb");
      if (wf) { fwrite(etc, 1, etcsz, wf); fclose(wf); static int bk = 0; if (bk < 12) { fprintf(stderr, "[bake] '%s' %dx%d -> %zuB\n", bully_cur_tex_path, w, h, etcsz); bk++; } }
    }
    free(rgb); free(etc);
  }
  return 0;
}

/* ===================== PAGINACAO DE TEXTURA (resident-set / VM de textura) =====================
 * O motor do Bully NUNCA despeja textura (provado: gen=1386 del=1). Na UMA do Mali, textura GL =
 * RAM. Entao implementamos o despejo NOS: cada textura ETC1-cacheada vira "pageavel". A FONTE fica
 * no SD (etc1cache/). Acima do orcamento, despejamos a mais FRIA (re-define 1x1 -> libera a RAM, o
 * id continua valido). Quando o motor RE-BINDA uma despejada (page fault), re-subimos do cache.
 * Gate: BULLY_PAGE=1 ; orcamento: BULLY_PAGE_CAP_MB (default 220). Diag: BULLY_PAGELOG=1. */
extern long long g_texbytes_live;
unsigned bully_g_texbytes(unsigned id);            /* fwd (def. junto do array g_texbytes) */
void     bully_g_texbytes_set(unsigned id, unsigned b);
static int bpp_of(unsigned fmt, unsigned type);    /* fwd (def. perto do my_glTexImage2D) */
static char         *g_page_name[262144];   /* strdup do asset ETC1 (so p/ kind=1) ~2MB */
static unsigned char  g_page_kind[262144];  /* 0=nao-pageavel, 1=ETC1(cache por nome), 2=SWAP(arquivo por id) */
static unsigned short g_page_w[262144], g_page_h[262144];
static unsigned char  g_page_present[262144];/* 1 = textura cheia carregada; 0 = despejada (1x1) */
static unsigned       g_page_use[262144];   /* relogio LRU */
static unsigned       g_page_clock = 0;
static long long      g_page_resident = 0;  /* bytes PRESENTES (pageaveis) */
static unsigned       g_page_list[40000]; static int g_page_n = 0; /* lista compacta de ids pageaveis */
static long g_pf = 0, g_ev = 0;              /* contadores: page-faults / evicts */
/* cabecalho do arquivo de swap (kind=2): re-upload precisa de w/h/formato */
struct bully_swap_hdr { unsigned magic; int w, h, ifmt; unsigned ufmt, utype, bytes; };
#define BULLY_SWAP_MAGIC 0xB0115A91u
static const char *bully_swapdir(void){ static const char*d; static int got; if(!got){ d=getenv("BULLY_PAGE_SWAP"); got=1; } return d; }
static int bully_paging(void){ static int m=-1; if(m<0)m=getenv("BULLY_PAGE")?1:0; return m; }
static long long bully_page_cap(void){ static long long c=-1; if(c<0){ const char*e=getenv("BULLY_PAGE_CAP_MB"); c=(long long)(e?atoll(e):220)*1024*1024; } return c; }
extern unsigned bully_g_texbytes(unsigned id);          /* getter (def. junto do array) */
extern void     bully_g_texbytes_set(unsigned id, unsigned b);
/* registra uma textura ETC1 recem-subida como pageavel (chamado de dentro do bully_try_etc1) */
static void bully_page_register(unsigned id, const char *name, int w, int h, unsigned etcsz) {
  if (!bully_paging() || id >= 262144) return;
  if (!g_page_kind[id] && g_page_n < 40000) g_page_list[g_page_n++] = id;  /* 1a vez na lista */
  if (!g_page_name[id]) g_page_name[id] = strdup(name);
  g_page_kind[id] = 1;                         /* ETC1 (fonte = etc1cache por nome) */
  g_page_w[id] = (unsigned short)w; g_page_h[id] = (unsigned short)h; g_page_use[id] = ++g_page_clock;
  /* o my_glTexImage2D ja tinha setado g_texbytes[id] com a estimativa RGBA -> corrige p/ o ETC1 real */
  g_texbytes_live += (long long)etcsz - bully_g_texbytes(id);
  bully_g_texbytes_set(id, etcsz);
  if (!g_page_present[id]) { g_page_present[id] = 1; g_page_resident += etcsz; }  /* presente */
}
/* registra uma textura NAO-ETC1 (alpha/RGBA) como pageavel: grava os pixels num arquivo de swap no SD
 * (fonte p/ re-upload) e marca kind=2. Render targets (px=NULL) e pequenas sao puladas pelo chamador. */
static void bully_page_write_swap(unsigned id, int ifmt, int w, int h, unsigned ufmt, unsigned utype, const void *data) {
  if (!bully_paging() || !bully_swapdir() || id >= 262144 || !data) return;
  if (g_page_kind[id] == 1) return;            /* ETC1 ja cuida (fonte no etc1cache) */
  int bpp = bpp_of(ufmt, utype); if (bpp <= 0) return;
  size_t bytes = (size_t)w * h * bpp;
  if (bytes < 96 * 1024) return;               /* pula texturas pequenas (<96KB) -- nao vale paginar */
  char path[320]; snprintf(path, sizeof path, "%s/%u.tx", bully_swapdir(), id);
  if (g_page_kind[id] != 2) {                  /* 1a vez: grava o swap (write-once) + entra na lista */
    FILE *f = fopen(path, "wb");
    if (f) { struct bully_swap_hdr hd = { BULLY_SWAP_MAGIC, w, h, ifmt, ufmt, utype, (unsigned)bytes };
      fwrite(&hd, sizeof hd, 1, f); fwrite(data, 1, bytes, f); fclose(f); }
    if (g_page_n < 40000) g_page_list[g_page_n++] = id;
    g_page_kind[id] = 2;
  }
  g_page_w[id] = (unsigned short)w; g_page_h[id] = (unsigned short)h; g_page_use[id] = ++g_page_clock;
  if (!g_page_present[id]) { g_page_present[id] = 1; g_page_resident += bully_g_texbytes(id); }
}
/* despeja as mais FRIAS ate voltar ao orcamento. 'keep'/'target' = a textura sendo usada agora
 * (nao despejar; re-bindar no fim). Re-define 1x1 -> libera a RAM da textura grande. */
static void bully_page_evict(unsigned target, unsigned keep) {
  static void (*rTexImg)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*) = NULL;
  static void (*rBind)(unsigned,unsigned) = NULL;
  if (!rTexImg) rTexImg = dlsym(RTLD_DEFAULT, "glTexImage2D");
  if (!rBind)   rBind   = dlsym(RTLD_DEFAULT, "glBindTexture");
  if (!rTexImg || !rBind) return;
  static const unsigned char black[4] = {0,0,0,0};
  int guard = 0;
  while (g_page_resident > bully_page_cap() && guard++ < 4096) {
    unsigned best = 0; unsigned bu = 0xffffffffu; int fi = -1;
    for (int i = 0; i < g_page_n; i++) { unsigned id = g_page_list[i];
      if (id == keep || !g_page_present[id] || bully_g_texbytes(id) == 0) continue; /* pula a atual e as JA-despejadas (1x1) -- senao thrash em "cadaveres" de 3 bytes */
      if (g_page_use[id] < bu) { bu = g_page_use[id]; best = id; fi = i; } }
    if (fi < 0) break;                                            /* nada pra despejar */
    long long b = bully_g_texbytes(best);                         /* bytes residentes (ETC1 ou SWAP) */
    rBind(0x0DE1, best);
    rTexImg(0x0DE1, 0, 0x1907, 1, 1, 0, 0x1907, 0x1401, black);   /* RGB 1x1 -> libera a grande */
    g_texbytes_live += 3 - b; bully_g_texbytes_set(best, 3);      /* agora ocupa ~3 bytes */
    g_page_resident -= b; g_page_present[best] = 0; g_ev++;       /* despejada */
  }
  rBind(target, keep);                                            /* restaura o bind do motor */
}
/* ===================== STREAMING ASSINCRONO (estilo GTA: pop-in, sem freeze) =====================
 * O re-upload SINCRONO do SD (ler arquivo no meio do frame) CONGELA com cap agressivo. Solucao: uma
 * thread de fundo le o SD (ZERO GL -- so file I/O) e enfileira o buffer pronto; a render thread sobe
 * o GL no proximo bind (pop-in de 1-2 frames). GL SO na render thread (contexto unico).
 * Gate: BULLY_PAGE_ASYNC=1. Sem ele, cai no caminho sincrono (fallback abaixo).
 * forward p/ o bind corrente do motor (def. real mais adiante, mesmo objeto de linkage interno). */
static unsigned g_cur_tex2d;
static unsigned char g_tex_alpha[262144]; /* forward: textura tem ALPHA/recorte (def. real adiante) -> trilinear=LINEAR */
/* pedido (render->worker): snapshot dos metadados sob lock p/ o worker NAO tocar globais compartilhados
 * (g_page_name pode ser free()ado pelo my_glDeleteTextures na render thread). */
struct page_req   { unsigned id; unsigned char kind; int w, h; char name[256]; };
/* buffer pronto (worker->render): pixels lidos do SD, prontos p/ subir no GL. */
struct page_ready { unsigned id; unsigned char *buf; int w, h; int ifmt; unsigned ufmt, utype, bytes; unsigned char kind; };
#define PAGE_RING 4096
static struct page_req   g_req_ring[PAGE_RING]; static int g_req_head = 0, g_req_tail = 0; /* tail=produz, head=consome */
static struct page_ready g_ready[PAGE_RING];    static int g_ready_n = 0;
static unsigned char     g_page_req[262144];    /* 1 = pedido em voo (enfileirado/lendo/pronto) -- nao duplicar */
static struct page_ready g_drain_buf[PAGE_RING];/* copia-out da drenagem (render thread, single-thread) */
static pthread_mutex_t g_page_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_page_cv  = PTHREAD_COND_INITIALIZER;
static pthread_t g_page_thr; static int g_page_thr_on = 0;        /* 0=nao iniciado, 1=rodando, -1=falhou(sync) */
static int bully_async(void){ static int m=-1; if(m<0)m=getenv("BULLY_PAGE_ASYNC")?1:0; return m; }

/* === SO I/O (pode rodar no worker) === le o arquivo do SD pro buffer. Devolve 1 + out preenchido (buf malloc). */
static int page_req_read(const struct page_req *rq, struct page_ready *out) {
  if (rq->kind == 1) {                                           /* ETC1 (etc1cache por nome) */
    char key[300]; bully_etc1_key(key, sizeof key, rq->name, rq->w, rq->h);
    size_t sz = (size_t)(rq->w/4) * (rq->h/4) * 8;
    FILE *f = fopen(key, "rb"); if (!f) return 0;
    unsigned char *buf = malloc(sz); size_t r = buf ? fread(buf, 1, sz, f) : 0; fclose(f);
    if (!buf || r != sz) { free(buf); return 0; }
    out->id=rq->id; out->buf=buf; out->w=rq->w; out->h=rq->h; out->ifmt=0; out->ufmt=0; out->utype=0; out->bytes=(unsigned)sz; out->kind=1;
    return 1;
  } else if (rq->kind == 2 && bully_swapdir()) {                 /* SWAP cru (arquivo por id, com cabecalho) */
    char path[320]; snprintf(path, sizeof path, "%s/%u.tx", bully_swapdir(), rq->id);
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    struct bully_swap_hdr hd; if (fread(&hd, sizeof hd, 1, f) != 1 || hd.magic != BULLY_SWAP_MAGIC) { fclose(f); return 0; }
    unsigned char *buf = malloc(hd.bytes); size_t r = buf ? fread(buf, 1, hd.bytes, f) : 0; fclose(f);
    if (!buf || r != hd.bytes) { free(buf); return 0; }
    out->id=rq->id; out->buf=buf; out->w=hd.w; out->h=hd.h; out->ifmt=hd.ifmt; out->ufmt=hd.ufmt; out->utype=hd.utype; out->bytes=hd.bytes; out->kind=2;
    return 1;
  }
  return 0;
}
/* === SO GL (render thread) === sobe o buffer pronto no GL. Binda o id, sobe, filtro LINEAR, contabiliza. */
static void page_upload(const struct page_ready *pr) {
  static unsigned (*rCompr)(unsigned,int,unsigned,int,int,int,int,const void*) = NULL;
  static void (*rTexImg)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*) = NULL;
  static void (*rParam)(unsigned,unsigned,int) = NULL;
  static void (*rBind)(unsigned,unsigned) = NULL;
  if (!rCompr)  rCompr  = dlsym(RTLD_DEFAULT, "glCompressedTexImage2D");
  if (!rTexImg) rTexImg = dlsym(RTLD_DEFAULT, "glTexImage2D");
  if (!rParam)  rParam  = dlsym(RTLD_DEFAULT, "glTexParameteri");
  if (!rBind)   rBind   = dlsym(RTLD_DEFAULT, "glBindTexture");
  unsigned id = pr->id;
  if (rBind) rBind(0x0DE1, id);
  if (pr->kind == 1) { if (rCompr)  rCompr(0x0DE1, 0, 0x8D64, pr->w, pr->h, 0, (int)pr->bytes, pr->buf); }
  else               { if (rTexImg) rTexImg(0x0DE1, 0, pr->ifmt, pr->w, pr->h, 0, pr->ufmt, pr->utype, pr->buf); }
  /* re-upload subiu so o nivel 0. Se TRILINEAR (e nao for recorte/alpha), REGENERA a cadeia de
   * mip + filtro trilinear -- senao GLES amostra niveis inexistentes = PRETO. kind=2 (nativo) so. */
  int trimip = (pr->kind == 2) && bully_trilinear() && (bully_cutout_mip() || !(id < 262144 && g_tex_alpha[id]));
  if (trimip) {
    static void (*rGenMip)(unsigned) = NULL;
    if (!rGenMip) rGenMip = dlsym(RTLD_DEFAULT, "glGenerateMipmap");
    if (rGenMip) rGenMip(0x0DE1);
    if (rParam) { rParam(0x0DE1, 0x2801, 0x2703); rParam(0x0DE1, 0x2800, 0x2601); } /* min=LINEAR_MIPMAP_LINEAR mag=LINEAR */
  } else if (rParam) { rParam(0x0DE1, 0x2801, 0x2601); rParam(0x0DE1, 0x2800, 0x2601); } /* LINEAR (sem mip) */
  g_texbytes_live += (long long)pr->bytes - bully_g_texbytes(id); bully_g_texbytes_set(id, pr->bytes);
  g_page_resident += pr->bytes; g_page_present[id] = 1; g_pf++;
}
/* page fault SINCRONO (fallback async OFF): le + sobe na hora (BINDADA agora; target sempre GL_TEXTURE_2D). */
static void bully_page_fault(unsigned target, unsigned id) {
  (void)target;
  struct page_req rq; rq.id = id; rq.kind = g_page_kind[id]; rq.w = g_page_w[id]; rq.h = g_page_h[id];
  if (rq.kind == 1 && g_page_name[id]) { strncpy(rq.name, g_page_name[id], sizeof rq.name - 1); rq.name[sizeof rq.name - 1] = '\0'; }
  else rq.name[0] = '\0';
  struct page_ready pr;
  if (page_req_read(&rq, &pr)) { page_upload(&pr); free(pr.buf); }  /* binda id e o deixa bindado (== o motor acabou de bindar) */
}
/* worker: dorme no cond; acorda, tira um pedido, LE o arquivo (so I/O), poe na lista pronto. Nunca toca GL. */
static void *page_worker(void *arg) {
  (void)arg;
  for (;;) {
    struct page_req rq;
    pthread_mutex_lock(&g_page_mtx);
    while (g_req_head == g_req_tail) pthread_cond_wait(&g_page_cv, &g_page_mtx);
    rq = g_req_ring[g_req_head]; g_req_head = (g_req_head + 1) % PAGE_RING;
    pthread_mutex_unlock(&g_page_mtx);
    struct page_ready pr;
    if (page_req_read(&rq, &pr)) {
      pthread_mutex_lock(&g_page_mtx);
      if (g_ready_n < PAGE_RING) g_ready[g_ready_n++] = pr; else free(pr.buf); /* lista cheia: descarta (re-pedido depois) */
      pthread_mutex_unlock(&g_page_mtx);
    } else {                                                      /* leitura falhou (arquivo sumiu/delete) -> libera p/ re-pedir */
      pthread_mutex_lock(&g_page_mtx);
      if (rq.id < 262144) g_page_req[rq.id] = 0;
      pthread_mutex_unlock(&g_page_mtx);
    }
  }
  return NULL;
}
/* render thread: enfileira um pedido (snapshot dos metadados sob lock) e sinaliza o worker. */
static void page_enqueue(unsigned id) {
  pthread_mutex_lock(&g_page_mtx);
  if (!g_page_req[id]) {
    int nt = (g_req_tail + 1) % PAGE_RING;
    if (nt != g_req_head) {                                      /* ring nao cheio */
      struct page_req *rq = &g_req_ring[g_req_tail];
      rq->id = id; rq->kind = g_page_kind[id]; rq->w = g_page_w[id]; rq->h = g_page_h[id];
      if (rq->kind == 1 && g_page_name[id]) { strncpy(rq->name, g_page_name[id], sizeof rq->name - 1); rq->name[sizeof rq->name - 1] = '\0'; }
      else rq->name[0] = '\0';
      g_req_tail = nt; g_page_req[id] = 1;
      pthread_cond_signal(&g_page_cv);
    }
  }
  pthread_mutex_unlock(&g_page_mtx);
}
/* render thread: sobe no GL tudo que o worker ja leu. So sobe se ainda valido (kind bate) e despejado. */
static void page_drain(void) {
  int n;
  pthread_mutex_lock(&g_page_mtx);
  n = g_ready_n; for (int i = 0; i < n; i++) g_drain_buf[i] = g_ready[i]; g_ready_n = 0;
  pthread_mutex_unlock(&g_page_mtx);
  if (!n) return;
  unsigned saved = g_cur_tex2d;
  for (int i = 0; i < n; i++) {
    struct page_ready *pr = &g_drain_buf[i]; unsigned id = pr->id;
    if (id < 262144 && g_page_kind[id] == pr->kind && !g_page_present[id]) page_upload(pr); /* motor pode ter deletado/reusado/ja-subido */
    free(pr->buf);
  }
  pthread_mutex_lock(&g_page_mtx);
  for (int i = 0; i < n; i++) if (g_drain_buf[i].id < 262144) g_page_req[g_drain_buf[i].id] = 0; /* pedido concluido */
  pthread_mutex_unlock(&g_page_mtx);
  static void (*rBind)(unsigned,unsigned) = NULL; if (!rBind) rBind = dlsym(RTLD_DEFAULT, "glBindTexture");
  if (rBind) rBind(0x0DE1, saved);                              /* restaura o bind do motor */
}
/* chamado do my_glBindTexture: toca o LRU, atende page fault (async ou sync), e despeja se acima do orcamento. */
void bully_page_on_bind(unsigned target, unsigned id) {
  if (!bully_paging() || target != 0x0DE1 || id >= 262144 || !g_page_kind[id]) return;
  g_page_use[id] = ++g_page_clock;
  if (bully_async()) {
    if (g_page_thr_on == 0) g_page_thr_on = (pthread_create(&g_page_thr, NULL, page_worker, NULL) == 0) ? 1 : -1;
    if (g_page_thr_on == 1) {
      if (!g_page_present[id]) page_enqueue(id);                 /* pop-in: NAO sobe agora -- fica 1x1 ate o worker ler */
      page_drain();                                             /* sobe o que o worker ja leu */
    } else if (!g_page_present[id]) bully_page_fault(target, id);/* thread falhou -> sincrono */
  } else if (!g_page_present[id]) bully_page_fault(target, id);  /* async OFF -> sincrono (comportamento antigo) */
  if (g_page_resident > bully_page_cap()) bully_page_evict(target, id);
  if (getenv("BULLY_PAGELOG")) { static long c=0; if ((c++ % 600)==0)
    fprintf(stderr, "[page] resident=%lldMB cap=%lldMB pf=%ld ev=%ld pageaveis=%d ready=%d\n",
            g_page_resident/(1024*1024), bully_page_cap()/(1024*1024), g_pf, g_ev, g_page_n, g_ready_n); }
}

/* ---- bionic libc bridges ---- */
static int *bionic___errno(void) { extern int *__errno_location(void); return __errno_location(); }
static size_t b_strlen_chk(const char *s, size_t n) { (void)n; return strlen(s); }
static char *b_strrchr_chk(const char *s, int c, size_t n) { (void)n; return strrchr(s, c); }
static char *b_strchr_chk(const char *s, int c, size_t n) { (void)n; return strchr(s, c); }
static char *b_strncpy_chk2(char *d, const char *s, size_t n, size_t dn, size_t sn) { (void)dn; (void)sn; return strncpy(d, s, n); }
static void b_assert2(const char *f, int l, const char *fn, const char *e) {
  fprintf(stderr, "assert: %s:%d %s: %s\n", f, l, fn, e); abort();
}
static int b_android_log(int prio, const char *tag, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap);
  return 0;
}

/* bionic __sF[3] = stdin/out/err. Wrappers traduzem p/ stream real. */
static char bionic_sF[3][512];
static FILE *map_sF(void *fp) {
  if (fp == (void *)&bionic_sF[0]) return stdin;
  if (fp == (void *)&bionic_sF[1]) return stdout;
  if (fp == (void *)&bionic_sF[2]) return stderr;
  return (FILE *)fp;
}
static int w_fprintf(void *fp, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); int r = vfprintf(map_sF(fp), fmt, ap); va_end(ap); return r;
}
static int w_vfprintf(void *fp, const char *fmt, va_list ap) { return vfprintf(map_sF(fp), fmt, ap); }
static size_t w_fwrite(const void *p, size_t s, size_t n, void *fp) { return fwrite(p, s, n, map_sF(fp)); }
static int w_fputs(const char *str, void *fp) { return fputs(str, map_sF(fp)); }
static int w_fputc(int c, void *fp) { return fputc(c, map_sF(fp)); }
static int w_fflush(void *fp) { return fflush(fp ? map_sF(fp) : NULL); }

/* _ctype_ legado (BSD): tabela 1+256, indexada de -1. */
static unsigned char ctype_tab[1 + 256];
#define _CT_U 0x01
#define _CT_L 0x02
#define _CT_N 0x04
#define _CT_S 0x08
#define _CT_P 0x10
#define _CT_C 0x20
#define _CT_X 0x40
#define _CT_B 0x80
static void ctype_init(void) {
  for (int c = 0; c < 256; c++) {
    unsigned char f = 0;
    if (isupper(c)) f |= _CT_U; if (islower(c)) f |= _CT_L;
    if (isdigit(c)) f |= _CT_N; if (isspace(c)) f |= _CT_S;
    if (ispunct(c)) f |= _CT_P; if (iscntrl(c)) f |= _CT_C;
    if (isxdigit(c)) f |= _CT_X; if (c == ' ') f |= _CT_B;
    ctype_tab[1 + c] = f;
  }
}

/* ---- NDK ANativeWindow (a janela é do egl_shim Mali fbdev) ---- */
static void *aw_fromSurface(void *env, void *surface) { (void)env; (void)surface; return (void *)0xAA11; }
static int aw_setBuffersGeometry(void *w, int x, int y, int f) { (void)w;(void)x;(void)y;(void)f; return 0; }
extern int bully_screen_w(void); extern int bully_screen_h(void);
static int aw_getWidth(void *w) { (void)w; return bully_screen_w(); }
static int aw_getHeight(void *w) { (void)w; return bully_screen_h(); }
static void aw_release(void *w) { (void)w; }

/* ---- NDK AAssetManager / AAsset (lê dos arquivos reais) ---- */
#ifndef ASSET_DIR
#define ASSET_DIR "assets"
#endif
typedef struct { FILE *fp; long len; } AAsset;
static void *am_fromJava(void *env, void *obj) { (void)env; (void)obj; return (void *)0xA55E7; }
static void *aa_open(void *mgr, const char *path, int mode) {
  (void)mgr; (void)mode;
  char full[1024]; snprintf(full, sizeof(full), "%s/%s", ASSET_DIR, path);
  FILE *fp = fopen(full, "rb");
  if (!fp) { fprintf(stderr, "[asset] FALTA %s\n", full); return NULL; }
  AAsset *a = calloc(1, sizeof(AAsset)); a->fp = fp;
  fseek(fp, 0, SEEK_END); a->len = ftell(fp); fseek(fp, 0, SEEK_SET);
  return a;
}
static int aa_read(void *h, void *buf, size_t n) { AAsset *a = h; return a ? fread(buf, 1, n, a->fp) : -1; }
static long aa_seek64(void *h, long off, int wh) { AAsset *a = h; if (!a) return -1; fseek(a->fp, off, wh); return ftell(a->fp); }
static long aa_getLength64(void *h) { AAsset *a = h; return a ? a->len : 0; }
static long aa_getRemainingLength64(void *h) { AAsset *a = h; return a ? a->len - ftell(a->fp) : 0; }
static void aa_close(void *h) { AAsset *a = h; if (a) { fclose(a->fp); free(a); } }

/* ---- fopen: disco; se falhar (leitura), serve de DENTRO dos data_*.zip ---- */
static FILE *w_fopen(const char *path, const char *mode) {
  static FILE *(*real)(const char *, const char *) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "fopen");
  FILE *f = real ? real(path, mode) : NULL;
  /* device: os dados ficam em assets/ (vfat sem symlink); o jogo fopena
   * "data_N.zip" no cwd -> redireciona p/ "assets/data_N.zip". */
  if (!f && real && path && mode && mode[0] == 'r' && strncmp(path, "assets/", 7) != 0) {
    char alt[1024]; snprintf(alt, sizeof(alt), "assets/%s", path);
    f = real(alt, mode);
  }
  /* TRACE settings/storage.ini -> descobrir se/onde o jogo le/escreve a config */
  if (path && (strstr(path, "settings.ini") || strstr(path, "storage.ini")))
    fprintf(stderr, "[cfg] fopen(\"%s\",\"%s\") -> %s\n", path, mode ? mode : "?", f ? "OK" : "FALHOU");
  return f;
}

/* ---- stat/lstat/fstat/fstatat (CAUSA do texto invisivel em glibc velha) ----
 * Em glibc < 2.33 os NOMES "stat"/"lstat"/"fstat" NAO sao simbolos exportados
 * (sao macros/wrappers inline -> __xstat(_STAT_VER,...)). O so_resolve resolve
 * imports por dlsym(NOME), entao nos devices de glibc antiga -- justamente os que
 * caem no bully.compat -- esses imports do libGame/libc++ ficam UNRESOLVED. O
 * jogo entao nao consegue stat() dos arquivos (ex.: as fontes) e o texto some,
 * mesmo com o resto renderizando. Resolvemos via SYSCALL crua: o kernel preenche
 * a `struct stat` no layout arm64 == layout do bionic (libGame), sem conversao.
 * Replica o fallback "assets/" do w_fopen (dados em vfat sem symlink). */
static int g_stat_log = 0;
static int stat_at(const char *path, void *buf, int flag) {
  int r = syscall(SYS_newfstatat, AT_FDCWD, path, buf, flag);
  if (r != 0 && path && strncmp(path, "assets/", 7) != 0) {
    char alt[1024]; snprintf(alt, sizeof(alt), "assets/%s", path);
    r = syscall(SYS_newfstatat, AT_FDCWD, alt, buf, flag);
  }
  if (g_stat_log < 24) { fprintf(stderr, "[stat] \"%s\" flag=%d -> %d\n", path ? path : "(null)", flag, r); g_stat_log++; }
  return r;
}
static int my_stat(const char *path, void *buf)  { return stat_at(path, buf, 0); }
static int my_lstat(const char *path, void *buf) { return stat_at(path, buf, AT_SYMLINK_NOFOLLOW); }
static int my_fstatat(int dfd, const char *path, void *buf, int flag) {
  if (dfd == AT_FDCWD) return stat_at(path, buf, flag);
  return syscall(SYS_newfstatat, dfd, path, buf, flag);
}
static int my_fstat(int fd, void *buf) {
  int r = syscall(SYS_fstat, fd, buf);
  if (g_stat_log < 24) { fprintf(stderr, "[stat] fstat(fd=%d) -> %d\n", fd, r); g_stat_log++; }
  return r;
}

/* ---- glGetString nunca-NULL ---- */
static const unsigned char *w_glGetString(unsigned name) {
  static const unsigned char *(*real)(unsigned) = NULL;
  if (!real) real = dlsym(RTLD_DEFAULT, "glGetString");
  const unsigned char *r = real ? real(name) : NULL;
  return r ? r : (const unsigned char *)"";
}

/* ---- bionic-only (não existem na glibc; libc++/libGame usam) ---- */
static void b_set_abort_message(const char *m) { fprintf(stderr, "[abort_msg] %s\n", m ? m : "?"); }
static int b_system_property_get(const char *name, char *value) { (void)name; if (value) value[0] = 0; return 0; }

/* ---- C++ thread-local init helpers (_ZTH*): no-op ---- */
static void tl_noop(void) {}

/* ================= GLES2 fixes Mali-450 Utgard (receitas do reVC) ============
 * O game importa glShaderSource/glTexImage2D/glTexParameteri direto -> a tabela
 * resolve p/ estes wrappers, que corrigem p/ o Utgard e chamam o real (Mali). */
static char *str_replace_all(const char *src, const char *find, const char *repl) {
  size_t fl = strlen(find), rl = strlen(repl), n = 0;
  for (const char *p = src; (p = strstr(p, find)); p += fl) n++;
  char *out = malloc(strlen(src) + n * (rl > fl ? rl - fl : 0) + 1);
  char *o = out; const char *p = src, *q;
  while ((q = strstr(p, find))) { memcpy(o, p, q - p); o += q - p; memcpy(o, repl, rl); o += rl; p = q + fl; }
  strcpy(o, p);
  return out;
}
extern int bully_is_kmsdrm(void);
static void (*real_glShaderSource)(unsigned, int, const char *const *, const int *) = NULL;
static void my_glShaderSource(unsigned sh, int count, const char *const *str, const int *len) {
  (void)len;
  if (!real_glShaderSource) real_glShaderSource = dlsym(RTLD_DEFAULT, "glShaderSource");
  size_t total = 1;
  for (int i = 0; i < count; i++) if (str && str[i]) total += strlen(str[i]);
  char *cat = malloc(total); cat[0] = 0;
  for (int i = 0; i < count; i++) if (str && str[i]) strcat(cat, str[i]);
  /* Utgard (Mali-400/450 GP) não suporta highp -> mediump (cores lavadas) */
  /* Mali-450: a GP (vertex) É FP32 e SUPORTA highp; só a PP (fragment) não tem.
   * Forçar mediump no VERTEX quebra a precisão do skinning -> braços/corpo do
   * Jimmy (muita deformação) colapsam/NaN -> invisíveis. Então só troca
   * highp->mediump nos shaders de FRAGMENTO (mantém highp no vertex). */
  /* só fragment: highp->mediump (Utgard PP não tem highp). Vertex mantém highp (skinning). */
  int is_vertex = strstr(cat, "gl_Position") != NULL;
  /* G310/kmsdrm: MANTEM highp em TUDO (qualidade). Mali-450/fbdev: fragment->mediump (PP sem highp). */
  char *s0 = (is_vertex || bully_is_kmsdrm()) ? strdup(cat) : str_replace_all(cat, "highp", "mediump");
  free(cat);
  /* alpha-test SÓ nos shaders de PERSONAGEM (têm `fadeandcolor`, exclusivo de
   * peds/Jimmy): a roupa do Jimmy é composta numa textura (RTT, 163 draws OK),
   * mas o alpha da textura composta sai baixo no Mali -> `if (a<0.7) discard`
   * corta a roupa -> aparece e some. Baixa p/ 0.04 só nesses shaders (folhagem
   * NÃO tem fadeandcolor -> intacta). */
  char *s1 = s0;
  if (!is_vertex && strstr(s0, "fadeandcolor")) {
    s1 = str_replace_all(s0, "< 0.7)", "< 0.04)");
    free(s0);
  }
  const char *one = s1;
  if (real_glShaderSource) real_glShaderSource(sh, 1, &one, NULL);
  free(s1);
}
/* forward (defs reais mais adiante): usados aqui no my_glTexParameteri */
#define RESMAP 262144
static unsigned char g_tex_alpha[RESMAP];   /* textura tem ALPHA (recorte) -> trilinear=LINEAR (sem halo preto) */
static unsigned g_cur_tex2d;
static void (*real_glTexParameteri)(unsigned, unsigned, int) = NULL;
static void my_glTexParameteri(unsigned target, unsigned pname, int param) {
  if (!real_glTexParameteri) real_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
  if (pname == 0x813D) return;                                  /* GL_TEXTURE_MAX_LEVEL: inexistente em GLES2 */
  /* fbdev (Mali-450): mipmap filter -> GL_LINEAR (Utgard sem mipmap = preto).
   * kmsdrm (G310): deixa o trilinear do jogo passar (mipmaps gerados no glTexImage2D). */
  int fl = 0;
  if (!bully_trilinear()) { if (!bully_is_kmsdrm() || bully_etc1_force()) fl = 1; }       /* fbdev/etc1: sempre LINEAR */
  else if (g_cur_tex2d < RESMAP && g_tex_alpha[g_cur_tex2d] && !bully_cutout_mip()) fl = 1; /* trilinear: recorte = LINEAR (a menos que CUTOUT_MIP) */
  if (fl && (pname == 0x2801 || pname == 0x2800) && param >= 0x2700 && param <= 0x2703)
    param = 0x2601;
  if (real_glTexParameteri) real_glTexParameteri(target, pname, param);
}
/* log de erros de compile/link de shader (achar o shader do Jimmy que falha) */
static void (*real_glCompileShader)(unsigned) = NULL;
static void my_glCompileShader(unsigned sh) {
  if (!real_glCompileShader) real_glCompileShader = dlsym(RTLD_DEFAULT, "glCompileShader");
  if (real_glCompileShader) real_glCompileShader(sh);
  void (*giv)(unsigned, unsigned, int *) = dlsym(RTLD_DEFAULT, "glGetShaderiv");
  void (*gil)(unsigned, int, int *, char *) = dlsym(RTLD_DEFAULT, "glGetShaderInfoLog");
  int ok = 1; if (giv) giv(sh, 0x8B81, &ok); /* GL_COMPILE_STATUS */
  if (!ok) { char log[1500] = {0}; if (gil) gil(sh, 1500, NULL, log); fprintf(stderr, "[shader] COMPILE FAIL sh=%u: %s\n", sh, log); }
}
static void (*real_glLinkProgram)(unsigned) = NULL;
static void my_glLinkProgram(unsigned p) {
  if (!real_glLinkProgram) real_glLinkProgram = dlsym(RTLD_DEFAULT, "glLinkProgram");
  if (real_glLinkProgram) real_glLinkProgram(p);
  void (*giv)(unsigned, unsigned, int *) = dlsym(RTLD_DEFAULT, "glGetProgramiv");
  void (*gil)(unsigned, int, int *, char *) = dlsym(RTLD_DEFAULT, "glGetProgramInfoLog");
  int ok = 1; if (giv) giv(p, 0x8B82, &ok); /* GL_LINK_STATUS */
  if (!ok) { char log[1500] = {0}; if (gil) gil(p, 1500, NULL, log); fprintf(stderr, "[shader] LINK FAIL p=%u: %s\n", p, log); }
}
/* texturas comprimidas: Mali-450 só faz ETC1 (0x8D64). Loga formatos p/ achar
 * se a camisa/skin do Jimmy usa um formato que o Mali rejeita -> transparente. */
static void (*real_glCompressedTexImage2D)(unsigned,int,unsigned,int,int,int,int,const void*) = NULL;
static void my_glCompressedTexImage2D(unsigned t,int l,unsigned ifmt,int w,int h,int b,int sz,const void*d) {
  if (!real_glCompressedTexImage2D) real_glCompressedTexImage2D = dlsym(RTLD_DEFAULT, "glCompressedTexImage2D");
  static int n = 0;
  if (n < 40) { fprintf(stderr, "[tex] compressed fmt=0x%x %dx%d sz=%d\n", ifmt, w, h, sz); n++; }
  if (real_glCompressedTexImage2D) real_glCompressedTexImage2D(t,l,ifmt,w,h,b,sz,d);
}
/* TESTE: ignora glEnable(GL_BLEND) -> se a camisa do Jimmy aparecer OPACA,
 * confirma que ela some por alpha-blend (alpha~0). (Quebra transparências
 * legítimas; é só p/ diagnóstico.) */
static void (*real_glEnable)(unsigned) = NULL;
static void my_glEnable(unsigned cap) {
  if (!real_glEnable) real_glEnable = dlsym(RTLD_DEFAULT, "glEnable");
  if (real_glEnable) real_glEnable(cap); /* (skip de GL_BLEND revertido: não era blend) */
}
static void (*real_glClear)(unsigned) = NULL;
static int g_cleardbg = 0;
unsigned g_pending_clear = 0;   /* clear adiado dentro de FBO (só efetiva se vier draw) */
static int g_defer_clear = -1;
void bully_flush_pending_clear(void) { /* chamado pelos draws dentro do FBO */
  if (g_pending_clear) {
    if (!real_glClear) real_glClear = dlsym(RTLD_DEFAULT, "glClear");
    if (real_glClear) real_glClear(g_pending_clear);
    g_pending_clear = 0;
  }
}
static void my_glClear(unsigned mask) {
  extern int g_in_fbo, g_rtt_clears;
  if (!real_glClear) real_glClear = dlsym(RTLD_DEFAULT, "glClear");
  if (g_in_fbo) g_rtt_clears++;
  /* CAUSA-RAIZ da roupa que some: este wrapper forçava GL_COLOR_BUFFER_BIT em
   * TODO clear (fix de tela preta). Dentro do render-to-texture da roupa, o jogo
   * faz clear só de PROFUNDIDADE -> o nosso force-COLOR APAGAVA a cor (a roupa já
   * composta). FIX: forçar COR só FORA de FBO (a tela). Dentro de FBO, respeita
   * a máscara do jogo (clear de profundidade NÃO apaga a roupa). */
  unsigned m = g_in_fbo ? mask : (mask | 0x4000);
  if (g_cleardbg < 12) { fprintf(stderr, "[gl] glClear in_fbo=%d mask=0x%x -> 0x%x\n", g_in_fbo, mask, m); g_cleardbg++; }
  if (real_glClear) real_glClear(m);
}
/* ===== INSTRUMENTACAO DE VAZAMENTO (recursos GL vivos) =====
 * Conta texturas/FBOs/renderbuffers vivos (gen - delete) + bytes estimados, p/
 * descobrir O QUE vaza (testers: RAM enche e OOM em ~30min no R36S). Report a
 * cada 120 frames via bully_resource_report(). So medicao; nao altera render. */
long g_tex_live = 0, g_fb_live = 0, g_rb_live = 0, g_tex_gen = 0, g_tex_del = 0;
long long g_texbytes_live = 0;
/* medicao p/ decidir halving SELETIVO: full-bytes (em res cheia) das texturas que
 * o TEX_HALF reduz, separadas por classe. delta de manter 512 cheias = 3/4 do full. */
long long g_half512_full = 0, g_half1024_full = 0; long g_half512_cnt = 0, g_half1024_cnt = 0;
/* RESMAP + g_tex_alpha declarados acima (antes do my_glTexParameteri) */
static unsigned g_texbytes[RESMAP];   /* bytes correntes por id de textura */
unsigned bully_g_texbytes(unsigned id) { return id < RESMAP ? g_texbytes[id] : 0; }
void bully_g_texbytes_set(unsigned id, unsigned b) { if (id < RESMAP) g_texbytes[id] = b; }
static unsigned g_rbbytes[RESMAP];    /* idem renderbuffer */
static unsigned char g_half_seen[RESMAP]; /* medicao: id ja contado no tally do halving */
static unsigned g_cur_tex2d = 0, g_cur_rb = 0;
static int bpp_internal(unsigned ifmt) {
  switch (ifmt) {
    case 0x8056: case 0x8D62: case 0x8033: case 0x8034: case 0x8363: return 2; /* RGBA4/565/4444/5551 */
    case 0x1907: case 0x81A6: return 3;       /* RGB / DEPTH24 */
    default: return 4;                          /* RGBA8/RGBA/DEPTH24_STENCIL8/etc */
  }
}
static long tex_chain_bytes(unsigned ifmt, int w, int h, int levels) {
  if (levels < 1) levels = 1;
  long bpp = bpp_internal(ifmt), t = 0;
  for (int i = 0; i < levels && (w > 0 || h > 0); i++) {
    int ww = w > 0 ? w : 1, hh = h > 0 ? h : 1;
    t += (long)ww * hh * bpp; w >>= 1; h >>= 1;
  }
  return t;
}
static void (*real_glBindTexture)(unsigned, unsigned) = NULL;
extern void bully_page_on_bind(unsigned target, unsigned id);
static void my_glBindTexture(unsigned target, unsigned tex) {
  if (target == 0x0DE1) g_cur_tex2d = tex;   /* GL_TEXTURE_2D */
  if (!real_glBindTexture) real_glBindTexture = dlsym(RTLD_DEFAULT, "glBindTexture");
  if (real_glBindTexture) real_glBindTexture(target, tex);
  bully_page_on_bind(target, tex);           /* PAGINACAO: LRU + page-fault + despejo (gated BULLY_PAGE) */
}
static void (*real_glGenTextures)(int, unsigned*) = NULL;
static void my_glGenTextures(int n, unsigned *ids) {
  if (!real_glGenTextures) real_glGenTextures = dlsym(RTLD_DEFAULT, "glGenTextures");
  if (real_glGenTextures) real_glGenTextures(n, ids);
  g_tex_live += n; g_tex_gen += n;
}
static void (*real_glDeleteTextures)(int, const unsigned*) = NULL;
static void my_glDeleteTextures(int n, const unsigned *ids) {
  for (int i = 0; ids && i < n; i++) { unsigned id = ids[i];
    if (id < RESMAP) {
      if (g_page_kind[id]) {   /* PAGINACAO: motor deletou uma pageavel -> limpa estado (id pode ser reusado) */
        if (g_page_present[id]) g_page_resident -= (long long)bully_g_texbytes(id);
        if (g_page_kind[id] == 2 && bully_swapdir()) {   /* apaga o arquivo de swap (id sera reusado) */
          char p[320]; snprintf(p, sizeof p, "%s/%u.tx", bully_swapdir(), id); remove(p);
        }
        if (g_page_name[id]) { free(g_page_name[id]); g_page_name[id] = NULL; }
        g_page_kind[id] = 0; g_page_present[id] = 0; g_page_req[id] = 0; /* async: pedido em voo se auto-descarta no drain (kind!=) */
      }
      g_texbytes_live -= g_texbytes[id]; g_texbytes[id] = 0; } }
  if (!real_glDeleteTextures) real_glDeleteTextures = dlsym(RTLD_DEFAULT, "glDeleteTextures");
  if (real_glDeleteTextures) real_glDeleteTextures(n, ids);
  g_tex_live -= n; g_tex_del += n;
}
static void (*real_glGenFramebuffers)(int, unsigned*) = NULL;
static void my_glGenFramebuffers(int n, unsigned *ids) {
  if (!real_glGenFramebuffers) real_glGenFramebuffers = dlsym(RTLD_DEFAULT, "glGenFramebuffers");
  if (real_glGenFramebuffers) real_glGenFramebuffers(n, ids);
  g_fb_live += n;
}
static void (*real_glDeleteFramebuffers)(int, const unsigned*) = NULL;
static void my_glDeleteFramebuffers(int n, const unsigned *ids) {
  if (!real_glDeleteFramebuffers) real_glDeleteFramebuffers = dlsym(RTLD_DEFAULT, "glDeleteFramebuffers");
  if (real_glDeleteFramebuffers) real_glDeleteFramebuffers(n, ids);
  g_fb_live -= n;
}
static void (*real_glGenRenderbuffers)(int, unsigned*) = NULL;
static void my_glGenRenderbuffers(int n, unsigned *ids) {
  if (!real_glGenRenderbuffers) real_glGenRenderbuffers = dlsym(RTLD_DEFAULT, "glGenRenderbuffers");
  if (real_glGenRenderbuffers) real_glGenRenderbuffers(n, ids);
  g_rb_live += n;
}
static void (*real_glDeleteRenderbuffers)(int, const unsigned*) = NULL;
static void my_glDeleteRenderbuffers(int n, const unsigned *ids) {
  for (int i = 0; ids && i < n; i++) { unsigned id = ids[i];
    if (id < RESMAP) { g_texbytes_live -= g_rbbytes[id]; g_rbbytes[id] = 0; } }
  if (!real_glDeleteRenderbuffers) real_glDeleteRenderbuffers = dlsym(RTLD_DEFAULT, "glDeleteRenderbuffers");
  if (real_glDeleteRenderbuffers) real_glDeleteRenderbuffers(n, ids);
  g_rb_live -= n;
}
static void (*real_glBindRenderbuffer)(unsigned, unsigned) = NULL;
static void my_glBindRenderbuffer(unsigned target, unsigned rb) {
  g_cur_rb = rb;
  if (!real_glBindRenderbuffer) real_glBindRenderbuffer = dlsym(RTLD_DEFAULT, "glBindRenderbuffer");
  if (real_glBindRenderbuffer) real_glBindRenderbuffer(target, rb);
}
static void (*real_glRenderbufferStorage)(unsigned, unsigned, int, int) = NULL;
static void my_glRenderbufferStorage(unsigned target, unsigned ifmt, int w, int h) {
  long b = (long)(w > 0 ? w : 1) * (h > 0 ? h : 1) * bpp_internal(ifmt);
  if (g_cur_rb < RESMAP) { g_texbytes_live += b - g_rbbytes[g_cur_rb]; g_rbbytes[g_cur_rb] = b; }
  if (!real_glRenderbufferStorage) real_glRenderbufferStorage = dlsym(RTLD_DEFAULT, "glRenderbufferStorage");
  if (real_glRenderbufferStorage) real_glRenderbufferStorage(target, ifmt, w, h);
}
/* chamado pelo loop de render (jni_shim) a cada 120 frames */
void bully_resource_report(void) {
  fprintf(stderr, "[leak] tex_live=%ld (gen=%ld del=%ld) fbo_live=%ld rb_live=%ld | ~%lld MB em texturas/RB vivos\n",
          g_tex_live, g_tex_gen, g_tex_del, g_fb_live, g_rb_live, g_texbytes_live / (1024*1024));
  /* medicao halving seletivo: full = bytes em res CHEIA das texturas reduzidas.
   * Hoje (todas reduzidas) usam full/4. Manter 512 cheias custaria +3/4 do full-512. */
  fprintf(stderr, "[texmem] 512-range: %ld tex full=%lld MB (manter-cheias custa +%lld MB) | 1024+: %ld tex full=%lld MB (reduzidas ja economizam %lld MB)\n",
          g_half512_cnt, g_half512_full/(1024*1024), (g_half512_full*3/4)/(1024*1024),
          g_half1024_cnt, g_half1024_full/(1024*1024), (g_half1024_full*3/4)/(1024*1024));
}

/* glTexStorage2D (GLES3) — a camisa do Jimmy pode vir por aqui (não pelo
 * glTexImage2D). Loga formato/níveis. */
static void (*real_glTexStorage2D)(unsigned, int, unsigned, int, int) = NULL;
static void my_glTexStorage2D(unsigned t, int levels, unsigned ifmt, int w, int h) {
  if (!real_glTexStorage2D) real_glTexStorage2D = dlsym(RTLD_DEFAULT, "glTexStorage2D");
  static int n = 0;
  if (n < 30) { fprintf(stderr, "[tex] STORAGE levels=%d ifmt=0x%x %dx%d\n", levels, ifmt, w, h); n++; }
  { long b = tex_chain_bytes(ifmt, w, h, levels);
    if (g_cur_tex2d < RESMAP) { g_texbytes_live += b - g_texbytes[g_cur_tex2d]; g_texbytes[g_cur_tex2d] = b; } }
  if (real_glTexStorage2D) real_glTexStorage2D(t, levels, ifmt, w, h);
}
static void (*real_glTexSubImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void*) = NULL;
static void my_glTexSubImage2D(unsigned t,int l,int xo,int yo,int w,int h,unsigned fmt,unsigned type,const void*px) {
  if (!real_glTexSubImage2D) real_glTexSubImage2D = dlsym(RTLD_DEFAULT, "glTexSubImage2D");
  static int n = 0;
  if (n < 30 && l == 0) { fprintf(stderr, "[tex] SUB fmt=0x%x type=0x%x %dx%d\n", fmt, type, w, h); n++; }
  if (real_glTexSubImage2D) real_glTexSubImage2D(t,l,xo,yo,w,h,fmt,type,px);
}
/* status do FBO (render-to-texture da roupa) — se INCOMPLETO no Mali, a textura
 * do corpo fica vazia -> camisa preta+discard. */
static void (*real_glBindFramebuffer)(unsigned, unsigned) = NULL;
unsigned long g_fbo_binds = 0; /* contador p/ medir RTT */
unsigned long g_frame_no = 0;  /* setado pelo loop de render (jni_shim) */
int g_in_fbo = 0;              /* >0 = dentro de um render-to-texture */
int g_rtt_draws = 0, g_rtt_clears = 0; /* trace: draws/clears no FBO atual */
unsigned g_rtt_tex = 0;        /* textura anexada ao FBO atual */
static int g_rtt_trace = 0;
static void my_glBindFramebuffer(unsigned t, unsigned fb) {
  if (!real_glBindFramebuffer) real_glBindFramebuffer = dlsym(RTLD_DEFAULT, "glBindFramebuffer");
  if (g_rtt_trace == 0) g_rtt_trace = getenv("BULLY_RTT_TRACE") ? 1 : 0;
  if (fb != 0) { g_in_fbo = 1; g_fbo_binds++; g_rtt_draws = 0; g_rtt_clears = 0; g_rtt_tex = 0; g_pending_clear = 0; }
  if (real_glBindFramebuffer) real_glBindFramebuffer(t, fb);
  /* ao SAIR do render-to-texture (roupa do Jimmy), o Mali Utgard não GRAVA o
   * render na textura sem sync -> modelo amostra vazio (roupa pisca e some).
   * glFinish ESPERA a GPU gravar antes de amostrar. Só DEPOIS do frame 300
   * (loading pesado já passou -> não satura o device). */
  if (fb == 0 && g_in_fbo) {
    g_in_fbo = 0;
    g_pending_clear = 0; /* pula clear-only (não apaga a roupa já composta) */
    if (g_rtt_trace && g_frame_no > 60) {
      static int tn = 0;
      if (tn < 400) { fprintf(stderr, "[rtt] composite tex=%u draws=%d clears=%d (frame %lu)\n", g_rtt_tex, g_rtt_draws, g_rtt_clears, g_frame_no); tn++; }
    }
    if (g_frame_no > 300) {
      /* glFinish trava a GPU Utgard ate gravar -> necessario p/ a ROUPA do Jimmy
       * (RTT pesado, ~163 draws) ser amostrada certa. MAS em CADA RTT (a cena
       * recompoe a cada movimento) satura o A53 e ESTOURA o audio (causa-raiz do
       * estouro em movimento; loading e limpo pq nao tem RTT). HEURISTICA: glFinish
       * so nos RTT PESADOS (>= MINDRAWS draws = a roupa); glFlush (nao-bloqueante)
       * nos leves (shadow/post) -> libera CPU p/ a mixagem -> audio limpo ao mover,
       * roupa preservada. BULLY_RTT_FINISH_MINDRAWS (default 0 = sempre glFinish). */
      static int mind = -1;
      if (mind < 0) { const char *e = getenv("BULLY_RTT_FINISH_MINDRAWS"); mind = e ? atoi(e) : 0; }
      if (g_rtt_draws >= mind) {
        static void (*fin)(void) = NULL;
        if (!fin) fin = dlsym(RTLD_DEFAULT, "glFinish");
        if (fin) fin();
      } else {
        static void (*fl)(void) = NULL;
        if (!fl) fl = dlsym(RTLD_DEFAULT, "glFlush");
        if (fl) fl();
      }
    } else {
      static void (*fl)(void) = NULL;
      if (!fl) fl = dlsym(RTLD_DEFAULT, "glFlush");
      if (fl) fl();
    }
  }
}
static void (*real_glFramebufferTexture2D)(unsigned,unsigned,unsigned,unsigned,int) = NULL;
static void my_glFramebufferTexture2D(unsigned t,unsigned att,unsigned tt,unsigned tex,int lvl) {
  extern unsigned g_rtt_tex;
  if (att == 0x8CE0) g_rtt_tex = tex;     /* trace: textura-cor anexada ao FBO atual */
  if (!real_glFramebufferTexture2D) real_glFramebufferTexture2D = dlsym(RTLD_DEFAULT, "glFramebufferTexture2D");
  if (real_glFramebufferTexture2D) real_glFramebufferTexture2D(t,att,tt,tex,lvl);
  static int n = 0;
  if (n < 14) {
    unsigned (*chk)(unsigned) = dlsym(RTLD_DEFAULT, "glCheckFramebufferStatus");
    unsigned s = chk ? chk(0x8D40) : 0;
    unsigned tb = (tex < RESMAP) ? g_texbytes[tex] : 0;  /* bytes da textura -> infere a resolucao do RT */
    fprintf(stderr, "[fbo] ATTACH att=0x%x tex=%u lvl=%d -> status=0x%x %s | RT~%u KB (%s)\n",
            att, tex, lvl, s, s == 0x8CD5 ? "OK" : "INCOMPLETO", tb/1024,
            tb > 2500000 ? "ALTA" : tb > 800000 ? "media" : "baixa");
    n++;
  }
}
static void (*real_glReadPixels)(int,int,int,int,unsigned,unsigned,void*) = NULL;
static void my_glReadPixels(int x,int y,int w,int h,unsigned fmt,unsigned type,void*px) {
  if (!real_glReadPixels) real_glReadPixels = dlsym(RTLD_DEFAULT, "glReadPixels");
  static int n = 0;
  if (n < 12) { fprintf(stderr, "[fbo] READPIXELS %dx%d fmt=0x%x type=0x%x\n", w, h, fmt, type); n++; }
  if (real_glReadPixels) real_glReadPixels(x,y,w,h,fmt,type,px);
}
static unsigned (*real_glCheckFramebufferStatus)(unsigned) = NULL;
static unsigned my_glCheckFramebufferStatus(unsigned t) {
  if (!real_glCheckFramebufferStatus) real_glCheckFramebufferStatus = dlsym(RTLD_DEFAULT, "glCheckFramebufferStatus");
  unsigned s = real_glCheckFramebufferStatus ? real_glCheckFramebufferStatus(t) : 0;
  static int n = 0;
  if (n < 20) { fprintf(stderr, "[fbo] CheckStatus=0x%x %s\n", s, s == 0x8CD5 ? "COMPLETE" : "INCOMPLETO!"); n++; }
  return s;
}
static void (*real_glClearColor)(float, float, float, float) = NULL;
static int g_ccdbg = 0;
static void my_glClearColor(float r, float g, float b, float a) {
  if (!real_glClearColor) real_glClearColor = dlsym(RTLD_DEFAULT, "glClearColor");
  if (g_ccdbg < 8) { fprintf(stderr, "[gl] glClearColor %.2f %.2f %.2f %.2f\n", r, g, b, a); g_ccdbg++; }
  if (real_glClearColor) real_glClearColor(r, g, b, a);
}
static int bpp_of(unsigned fmt, unsigned type) {
  if (type == 0x1401) return fmt == 0x1908 ? 4 : fmt == 0x1907 ? 3 : fmt == 0x190A ? 2 : 1;
  if (type == 0x8033 || type == 0x8034 || type == 0x8363) return 2; /* 4444/5551/565 */
  return 0; /* desconhecido -> não reduz */
}
static int g_tex_half = -1;
static void (*real_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) = NULL;
static void (*real_glGenerateMipmap)(unsigned) = NULL;
static void my_glTexImage2D(unsigned tgt, int lvl, int ifmt, int w, int h, int bord, unsigned fmt, unsigned type, const void *px) {
  if (!real_glTexImage2D) real_glTexImage2D = dlsym(RTLD_DEFAULT, "glTexImage2D");
  if (g_tex_half < 0) g_tex_half = (getenv("BULLY_TEX_HALF") && !bully_paging()) ? 1 : 0; /* FASE 2: paginando = NATIVO full-res (sem downscale) */
  /* GROUND-TRUTH p/ a sidetable ETC1: casa nome do .tex <-> upload real. */
  if (getenv("BULLY_TEXDIAG")) {
    static int td = 0;
    if (td < 400) { fprintf(stderr, "[texdiag] '%s' lvl=%d %dx%d ifmt=0x%x fmt=0x%x type=0x%x px=%d\n",
        bully_cur_tex_path[0] ? bully_cur_tex_path : "(none)", lvl, w, h, ifmt, fmt, type, px ? 1 : 0); td++; }
  }
  /* DUMP os pixels DECODIFICADOS pelo engine (ground-truth p/ crackear o .tex offline).
   * BULLY_DUMPTEX=substring -> grava /tmp/bullydump/<basename>_L<lvl>_<w>x<h>_t<type>.bin */
  { const char *want = getenv("BULLY_DUMPTEX");
    if (want && want[0] && px && bully_cur_tex_path[0] && strstr(bully_cur_tex_path, want)) {
      int bb = bpp_of(fmt, type); if (bb <= 0) bb = 4;
      const char *bn = strrchr(bully_cur_tex_path, '/'); bn = bn ? bn+1 : bully_cur_tex_path;
      char fn[256]; snprintf(fn, sizeof(fn), "/tmp/bullydump/%s_L%d_%dx%d_t%x.bin", bn, lvl, w, h, type);
      mkdir("/tmp/bullydump", 0777);
      FILE *df = fopen(fn, "wb");
      if (df) { fwrite(px, 1, (size_t)w*h*bb, df); fclose(df); fprintf(stderr, "[dumptex] %s (%d bytes)\n", fn, w*h*bb); }
    }
  }
  /* leak-track: bytes do nível 0 da textura 2D corrente (RTT geralmente vem por aqui c/ px=NULL) */
  if (lvl == 0 && g_cur_tex2d < RESMAP) {
    int bb = bpp_of(fmt, type); if (bb <= 0) bb = 4;
    long b = (long)(w > 0 ? w : 1) * (h > 0 ? h : 1) * bb;
    g_texbytes_live += b - g_texbytes[g_cur_tex2d]; g_texbytes[g_cur_tex2d] = b;
  }
  /* CACHE ETC1 offline: textura opaca 565 com ETC1 bakeado -> sobe ETC1 (4x menos VRAM)
   * e PULA o resto (conversoes/halving/mips). No bake, grava e segue o caminho normal. */
  if (!bully_paging() && bully_try_etc1(tgt, lvl, w, h, fmt, type, px)) { /* FASE 2: paginando = NATIVO (sem ETC1 lossy); textura cai no swap kind=2 */
    if (lvl == 0 && g_cur_tex2d < RESMAP)   /* registra a textura ETC1 como pageavel */
      bully_page_register(g_cur_tex2d, bully_cur_tex_path, w, h, (unsigned)((size_t)(w/4)*(h/4)*8));
    return;
  }
  /* BAKE-ALL: ja encodamos o ETC1 (565) acima; agora sobe um STUB 1x1 em vez da textura
   * cheia -> a GL fica minuscula (sem estourar a MMU) e NAO precisamos deletar GL textures
   * (era o que crashava por use-after-free no wrap do ring). O sprite desenha 1x1 (lixo,
   * tanto faz: e so um passe de conversao). */
  { static int g_bakeall = -1; if (g_bakeall < 0) g_bakeall = getenv("BULLY_BAKEALL") ? 1 : 0;
    if (g_bakeall) {
      if (lvl == 0) { static const unsigned char stub[4] = {0,0,0,255};
        if (real_glTexImage2D) real_glTexImage2D(tgt, 0, 0x1908, 1, 1, 0, 0x1908, 0x1401, stub); }
      return;
    } }
  if (ifmt == 0x8058) ifmt = 0x1908;       /* GL_RGBA8 -> GL_RGBA (GLES2 não aceita sized) */
  else if (ifmt == 0x8051) ifmt = 0x1907;  /* GL_RGB8 -> GL_RGB */
  /* pula mipmaps: como forço MIN_FILTER=LINEAR, os níveis >0 nunca são usados ->
   * só desperdiçam memória de textura da GPU (o Mali Utgard trava ao estourar). */
  if (g_tex_half && lvl > 0) return;
  /* LUMINANCE vazia (px=NULL) = alvo de render-to-texture da roupa; Mali não
   * renderiza p/ LUMINANCE -> aloca RGBA (renderável). Sem reduzir (é o alvo). */
  if ((fmt == 0x1909 || fmt == 0x190A) && type == 0x1401 && !px && w > 0 && h > 0) {
    if (real_glTexImage2D) real_glTexImage2D(tgt, lvl, 0x1908, w, h, bord, 0x1908, 0x1401, NULL);
    return;
  }
  /* monta os dados finais; converte LUMINANCE->RGBA (Mali lê L como (L,L,L,L)) */
  const unsigned char *data = px;
  unsigned ufmt = fmt, utype = type;
  unsigned char *conv = NULL;
  if ((fmt == 0x1909 || fmt == 0x190A) && type == 0x1401 && px && w > 0 && h > 0) {
    int la = (fmt == 0x190A);
    const unsigned char *src = px;
    conv = malloc((size_t)w * h * 4);
    if (conv) {
      for (int i = 0; i < w * h; i++) {
        unsigned char L = src[la ? i * 2 : i];
        conv[i*4] = L; conv[i*4+1] = L; conv[i*4+2] = L; conv[i*4+3] = la ? src[i*2+1] : 255;
      }
      data = conv; ufmt = 0x1908; utype = 0x1401; ifmt = 0x1908;
    }
  }
  /* HALVING SELETIVO (v8.3 qualidade): reduz pela metade so as texturas >=1024
   * (as MAIORES, grosso da memoria). As 512-range ficam em RES CHEIA (nitidas) ->
   * mundo mais nitido. Custo ~+42MB (cabe na folga; despejo segura excursoes).
   * Antes era >=512 (tudo reduzido). UV normalizado -> reduzir nao quebra coords.
   * Override: BULLY_TEX_HALF_MIN (default 1024; 512 = comportamento antigo). */
  /* TRILINEAR: detecta textura de RECORTE (alpha vazado: folhas/cercas/portoes/corrimaos).
   * Recorte NAO pode ter mipmap (o mip mistura o RGB preto dos texels transparentes -> HALO
   * PRETO FORTE). Detecta por SCAN do alpha (so recortes REAIS, nao todo RGBA -> nao quebra
   * o resto). is_cutout -> LINEAR (sem mip), setado AQUI no upload E no glTexParameteri
   * (robusto a ordem param/upload -- foi o que quebrou antes). [2026-06-20] */
  int is_cutout = 0;
  if (bully_trilinear() && lvl == 0 && data) {
    if (utype == 0x8033 || utype == 0x8034) is_cutout = 1;          /* 4444/5551 = formato com alpha */
    else if (ufmt == 0x1908 && utype == 0x1401 && w > 0 && h > 0) { /* RGBA8888: scaneia o alpha */
      int n = w * h, step = n > 4096 ? n / 4096 : 1, tr = 0;
      for (int i = 0; i < n; i += step) if (((const unsigned char *)data)[(size_t)i*4+3] < 250) { if (++tr > 8) break; }
      is_cutout = (tr > 8);
    }
  }
  if (lvl == 0 && g_cur_tex2d < RESMAP) g_tex_alpha[g_cur_tex2d] = is_cutout ? 1 : 0;
  /* DIAG (BULLY_TEXLOG): lista texturas com alpha -> achar as folhas/papeis do chao e ver
   * se sao detectadas como recorte. So qdo o env liga (sem custo no normal). */
  if (getenv("BULLY_TEXLOG") && lvl == 0 && data && (ufmt == 0x1908 || utype == 0x8033 || utype == 0x8034)) {
    static int lg = 0; if (lg < 300) { fprintf(stderr, "[texlog] %s %dx%d ufmt=%x type=%x cutout=%d\n",
      bully_cur_tex_path[0]?bully_cur_tex_path:"?", w, h, ufmt, utype, is_cutout); lg++; }
  }
  /* ALPHA BLEED: nos recortes (folhas/trepadeiras) os texels transparentes tem RGB PRETO;
   * o bilinear na borda mistura esse preto na parte visivel = CONTORNO PRETO FORTE. Preenche
   * o RGB dos transparentes com o do vizinho opaco (2 passes = 2 aneis). So muda RGB, alpha
   * intacto (alpha-test inalterado). RGBA8888 recorte. [2026-06-20] */
  if (is_cutout && data && w >= 2 && h >= 2 && lvl == 0) {
    int W = w, H = h;
    if (ufmt == 0x1908 && utype == 0x1401) {            /* RGBA8888 (4 bytes/texel) */
      unsigned char *d = malloc((size_t)W * H * 4);
      if (d) { memcpy(d, data, (size_t)W * H * 4);
        for (int p = 0; p < 2; p++) for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
          size_t o = ((size_t)y * W + x) * 4;  if (d[o+3] >= 128) continue;
          int nx[4] = {x-1,x+1,x,x}, ny[4] = {y,y,y-1,y+1};
          for (int k = 0; k < 4; k++) { if (nx[k]<0||nx[k]>=W||ny[k]<0||ny[k]>=H) continue; size_t n = ((size_t)ny[k]*W+nx[k])*4;
            if (d[n+3] >= 128) { d[o]=d[n]; d[o+1]=d[n+1]; d[o+2]=d[n+2]; break; } } }
        if (conv) free(conv); conv = d; data = d; }
    } else if (utype == 0x8033) {                        /* RGBA4444: u16, alpha=val&0xF, rgb=val&0xFFF0 */
      unsigned short *d = malloc((size_t)W * H * 2);
      if (d) { memcpy(d, data, (size_t)W * H * 2);
        for (int p = 0; p < 2; p++) for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
          size_t o = (size_t)y * W + x;  if ((d[o] & 0xF) >= 8) continue;
          int nx[4] = {x-1,x+1,x,x}, ny[4] = {y,y,y-1,y+1};
          for (int k = 0; k < 4; k++) { if (nx[k]<0||nx[k]>=W||ny[k]<0||ny[k]>=H) continue; size_t n = (size_t)ny[k]*W+nx[k];
            if ((d[n] & 0xF) >= 8) { d[o] = (unsigned short)((d[n] & 0xFFF0) | (d[o] & 0xF)); break; } } }
        if (conv) free(conv); conv = (unsigned char*)d; data = (unsigned char*)d; }
    } else if (utype == 0x8034) {                        /* RGBA5551: u16, alpha=val&1, rgb=val&0xFFFE */
      unsigned short *d = malloc((size_t)W * H * 2);
      if (d) { memcpy(d, data, (size_t)W * H * 2);
        for (int p = 0; p < 2; p++) for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
          size_t o = (size_t)y * W + x;  if (d[o] & 1) continue;
          int nx[4] = {x-1,x+1,x,x}, ny[4] = {y,y,y-1,y+1};
          for (int k = 0; k < 4; k++) { if (nx[k]<0||nx[k]>=W||ny[k]<0||ny[k]>=H) continue; size_t n = (size_t)ny[k]*W+nx[k];
            if (d[n] & 1) { d[o] = (unsigned short)((d[n] & 0xFFFE) | (d[o] & 1)); break; } } }
        if (conv) free(conv); conv = (unsigned char*)d; data = (unsigned char*)d; }
    }
  }
  int bpp = bpp_of(ufmt, utype);
  static int half_min = -1;
  if (half_min < 0) { const char *e = getenv("BULLY_TEX_HALF_MIN"); half_min = e ? atoi(e) : 1024; if (half_min < 256) half_min = 256; }
  if (g_tex_half && data && bpp > 0 && (w >= half_min || h >= half_min)) {
    /* tally p/ decidir halving seletivo (1x por id) */
    if (g_cur_tex2d < RESMAP && !g_half_seen[g_cur_tex2d]) {
      g_half_seen[g_cur_tex2d] = 1;
      long full = (long)w * h * bpp; int dim = (w > h) ? w : h;
      if (dim < 1024) { g_half512_cnt++; g_half512_full += full; }
      else { g_half1024_cnt++; g_half1024_full += full; }
    }
    int nw = w / 2, nh = h / 2;
    unsigned char *sm = (nw > 0 && nh > 0) ? malloc((size_t)nw * nh * bpp) : NULL;
    if (sm) {
      /* nearest (1 pixel a cada 2). NOTA: box-filter 2x2 foi testado e BUGOU a tela
       * (amacia o alpha das texturas de recorte/mascara -> bordas erradas). Mantido
       * nearest, que preserva valores exatos (alpha-test/cutout intactos). */
      for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++)
          memcpy(sm + ((size_t)y * nw + x) * bpp, data + ((size_t)(y*2) * w + x*2) * bpp, bpp);
      if (real_glTexImage2D) real_glTexImage2D(tgt, lvl, ifmt, nw, nh, bord, ufmt, utype, sm);
      if (lvl == 0 && g_cur_tex2d < RESMAP)   /* PAGINACAO: swap da versao halved */
        bully_page_write_swap(g_cur_tex2d, ifmt, nw, nh, ufmt, utype, sm);
      free(sm); free(conv);
      /* TRILINEAR: a textura halved tb precisa de mipmap, senao trilinear amostra niveis
       * que nao existem -> PRETO (era por isso que os personagens (atlas >=768, halved)
       * ficavam pretos e o mundo (tiles <768, nao-halved) ficava nitido). [teste 2026-06-20] */
      if (lvl == 0 && bully_trilinear()) {
        if (is_cutout) {  /* recorte: LINEAR (sem mip = sem halo preto) */
          if (!real_glTexParameteri) real_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
          if (real_glTexParameteri) real_glTexParameteri(tgt, 0x2801, 0x2601);
        } else {          /* opaca: trilinear+mip */
          if (!real_glGenerateMipmap) real_glGenerateMipmap = dlsym(RTLD_DEFAULT, "glGenerateMipmap");
          if (real_glGenerateMipmap) real_glGenerateMipmap(tgt);
        }
      }
      return;
    }
  }
  if (real_glTexImage2D) real_glTexImage2D(tgt, lvl, ifmt, w, h, bord, ufmt, utype, data);
  if (lvl == 0 && g_cur_tex2d < RESMAP)   /* PAGINACAO: swap da textura cheia (alpha/RGBA; RT px=NULL e pulado) */
    bully_page_write_swap(g_cur_tex2d, ifmt, w, h, ufmt, utype, data);
  /* kmsdrm (G310): gera a cadeia de mipmap completa p/ o trilinear funcionar (sem
   * isso, MIN_FILTER mipmap -> textura preta). So no nivel 0 com dados reais. */
  if (lvl == 0 && data && ((bully_is_kmsdrm() && !bully_etc1_force()) || bully_trilinear())) {
    if (bully_trilinear() && is_cutout && !bully_cutout_mip()) {  /* recorte: LINEAR (sem mip = sem halo preto) */
      if (!real_glTexParameteri) real_glTexParameteri = dlsym(RTLD_DEFAULT, "glTexParameteri");
      if (real_glTexParameteri) real_glTexParameteri(tgt, 0x2801, 0x2601);
    } else {                                /* opaca, ou recorte c/ CUTOUT_MIP, ou kmsdrm: trilinear+mip */
      if (!real_glGenerateMipmap) real_glGenerateMipmap = dlsym(RTLD_DEFAULT, "glGenerateMipmap");
      if (real_glGenerateMipmap) real_glGenerateMipmap(tgt);
    }
  }
  free(conv);
}

/* trace: conta os desenhos dentro de cada render-to-texture (roupa) */
static void (*real_glDrawElements)(unsigned, int, unsigned, const void *) = NULL;
static void my_glDrawElements(unsigned mode, int count, unsigned type, const void *idx) {
  extern int g_in_fbo, g_rtt_draws; extern void bully_flush_pending_clear(void);
  if (g_in_fbo) { bully_flush_pending_clear(); g_rtt_draws++; }
  if (!real_glDrawElements) real_glDrawElements = dlsym(RTLD_DEFAULT, "glDrawElements");
  if (real_glDrawElements) real_glDrawElements(mode, count, type, idx);
}
static void (*real_glDrawArrays)(unsigned, int, int) = NULL;
static void my_glDrawArrays(unsigned mode, int first, int count) {
  extern int g_in_fbo, g_rtt_draws; extern void bully_flush_pending_clear(void);
  if (g_in_fbo) { bully_flush_pending_clear(); g_rtt_draws++; }
  if (!real_glDrawArrays) real_glDrawArrays = dlsym(RTLD_DEFAULT, "glDrawArrays");
  if (real_glDrawArrays) real_glDrawArrays(mode, first, count);
}

void bully_imports_init(void) { ctype_init(); }

/* tabela de overrides (resolvida ANTES do fallback dlsym do so_resolve) */

/* KMSDRM: o eglSwapBuffers cru nao faz page-flip (so SDL_GL_SwapWindow faz).
 * fbdev (mali): mantem o raw (Amlogic-old intacto). */
extern void bully_swap_buffers(void);
extern int  bully_is_kmsdrm(void);
static unsigned (*real_eglSwapBuffers)(void*, void*) = NULL;
static unsigned my_eglSwapBuffers(void *dpy, void *surf) {
  /* durante o bake: pinta a tela "Convertendo texturas (PT/EN/RU) + barra/contador"
   * por cima, ANTES do swap -> some o jogo cru/preto, fica um loading limpo. */
  { extern int bully_bake_active, bully_bake_cur, bully_bake_total; extern void bully_bake_ui(int, int);
    if (bully_bake_active) { static int n=0; if(n<3){fprintf(stderr,"[swap] via my_eglSwapBuffers (bake)\n");n++;} bully_bake_ui(bully_bake_cur, bully_bake_total); } }
  if (bully_is_kmsdrm()) { bully_swap_buffers(); return 1; }
  { extern void bully_maybe_screenshot(void); bully_maybe_screenshot(); } /* fbdev: screenshot sob demanda (gameplay presenta por aqui) */
  if (!real_eglSwapBuffers) real_eglSwapBuffers = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
  return real_eglSwapBuffers ? real_eglSwapBuffers(dpy, surf) : 1;
}

/* VAO / MRT: o libGame importa as versoes CORE (glGenVertexArrays etc + glDrawBuffers)
 * que existem no Mali-G31 (GLES3.2) mas NAO como simbolo no Mali-450 Utgard (GLES2)
 * -> ficavam *** UNRESOLVED ***. Resolvemos via eglGetProcAddress tentando o CORE e
 * depois a EXTENSAO (OES p/ VAO, EXT p/ draw_buffers); no-op seguro se o device nao
 * tiver nenhuma (hoje o jogo nao chama esse caminho no GLES2 -> nunca executa, mas o
 * no-op evita slot invalido caso chame). No X5M/GLES3 pega a core real (VAO de verdade). */
static void *gl_proc2(const char *core, const char *ext) {
  void *(*gpa)(const char *) = dlsym(RTLD_DEFAULT, "eglGetProcAddress");
  void *p = gpa ? gpa(core) : NULL;
  if (!p && gpa && ext) p = gpa(ext);
  if (!p) p = dlsym(RTLD_DEFAULT, core);
  if (!p && ext) p = dlsym(RTLD_DEFAULT, ext);
  return p;
}
static void (*r_glGenVAO)(int, unsigned *) = NULL;
static void my_glGenVertexArrays(int n, unsigned *a) {
  if (!r_glGenVAO) r_glGenVAO = gl_proc2("glGenVertexArrays", "glGenVertexArraysOES");
  if (r_glGenVAO) r_glGenVAO(n, a);
  else if (a) for (int i = 0; i < n; i++) a[i] = 0;
}
static void (*r_glBindVAO)(unsigned) = NULL;
static void my_glBindVertexArray(unsigned a) {
  if (!r_glBindVAO) r_glBindVAO = gl_proc2("glBindVertexArray", "glBindVertexArrayOES");
  if (r_glBindVAO) r_glBindVAO(a);
}
static void (*r_glDelVAO)(int, const unsigned *) = NULL;
static void my_glDeleteVertexArrays(int n, const unsigned *a) {
  if (!r_glDelVAO) r_glDelVAO = gl_proc2("glDeleteVertexArrays", "glDeleteVertexArraysOES");
  if (r_glDelVAO) r_glDelVAO(n, a);
}
static void (*r_glDrawBuffers)(int, const unsigned *) = NULL;
static void my_glDrawBuffers(int n, const unsigned *b) {
  if (!r_glDrawBuffers) r_glDrawBuffers = gl_proc2("glDrawBuffers", "glDrawBuffersEXT");
  if (r_glDrawBuffers) r_glDrawBuffers(n, b);
}

/* __stack_chk_fail neutralizado (insurance): com o TLS pad do main.c a canary
 * bionic ja fica estavel e isto nunca dispara; mas se um path nao-coberto ler
 * tpidr+0x28 instavel, melhor logar do que abortar o jogo. */
static void b_stack_chk_fail(void) {
  static int n = 0;
  if (n++ < 3) fprintf(stderr, "[stack_chk_fail] FALSO-POSITIVO TLS ignorado\n");
}

DynLibFunction bully_stub_table[] = {
  {"__stack_chk_fail", (uintptr_t)b_stack_chk_fail},
  {"eglSwapBuffers", (uintptr_t)my_eglSwapBuffers},
  {"__errno", (uintptr_t)bionic___errno}, {"__assert2", (uintptr_t)b_assert2},
  {"__strlen_chk", (uintptr_t)b_strlen_chk}, {"__strrchr_chk", (uintptr_t)b_strrchr_chk},
  {"__strchr_chk", (uintptr_t)b_strchr_chk}, {"__strncpy_chk2", (uintptr_t)b_strncpy_chk2},
  {"__android_log_print", (uintptr_t)b_android_log},
  {"android_set_abort_message", (uintptr_t)b_set_abort_message},
  {"__system_property_get", (uintptr_t)b_system_property_get},
  {"__sF", (uintptr_t)bionic_sF},
  {"fprintf", (uintptr_t)w_fprintf}, {"vfprintf", (uintptr_t)w_vfprintf}, {"fwrite", (uintptr_t)w_fwrite},
  {"fputs", (uintptr_t)w_fputs}, {"fputc", (uintptr_t)w_fputc}, {"fflush", (uintptr_t)w_fflush},
  {"_ctype_", (uintptr_t)(ctype_tab + 1)},
  {"ANativeWindow_fromSurface", (uintptr_t)aw_fromSurface},
  {"ANativeWindow_setBuffersGeometry", (uintptr_t)aw_setBuffersGeometry},
  {"ANativeWindow_getWidth", (uintptr_t)aw_getWidth}, {"ANativeWindow_getHeight", (uintptr_t)aw_getHeight},
  {"ANativeWindow_release", (uintptr_t)aw_release},
  {"AAssetManager_fromJava", (uintptr_t)am_fromJava}, {"AAssetManager_open", (uintptr_t)aa_open},
  {"AAsset_read", (uintptr_t)aa_read}, {"AAsset_seek64", (uintptr_t)aa_seek64},
  {"AAsset_getLength64", (uintptr_t)aa_getLength64}, {"AAsset_getRemainingLength64", (uintptr_t)aa_getRemainingLength64},
  {"AAsset_close", (uintptr_t)aa_close},
  {"glGetString", (uintptr_t)w_glGetString},
  {"glShaderSource", (uintptr_t)my_glShaderSource},
  {"glTexParameteri", (uintptr_t)my_glTexParameteri},
  {"glTexImage2D", (uintptr_t)my_glTexImage2D},
  {"glClear", (uintptr_t)my_glClear},
  {"glClearColor", (uintptr_t)my_glClearColor},
  {"glCompileShader", (uintptr_t)my_glCompileShader},
  {"glLinkProgram", (uintptr_t)my_glLinkProgram},
  {"glCompressedTexImage2D", (uintptr_t)my_glCompressedTexImage2D},
  {"glEnable", (uintptr_t)my_glEnable},
  {"glTexStorage2D", (uintptr_t)my_glTexStorage2D},
  {"glTexSubImage2D", (uintptr_t)my_glTexSubImage2D},
  /* leak-track (medicao do vazamento de recursos GL) */
  {"glBindTexture", (uintptr_t)my_glBindTexture},
  {"glGenTextures", (uintptr_t)my_glGenTextures},
  {"glDeleteTextures", (uintptr_t)my_glDeleteTextures},
  {"glGenFramebuffers", (uintptr_t)my_glGenFramebuffers},
  {"glDeleteFramebuffers", (uintptr_t)my_glDeleteFramebuffers},
  {"glGenRenderbuffers", (uintptr_t)my_glGenRenderbuffers},
  {"glDeleteRenderbuffers", (uintptr_t)my_glDeleteRenderbuffers},
  {"glBindRenderbuffer", (uintptr_t)my_glBindRenderbuffer},
  {"glRenderbufferStorage", (uintptr_t)my_glRenderbufferStorage},
  {"glCheckFramebufferStatus", (uintptr_t)my_glCheckFramebufferStatus},
  {"glBindFramebuffer", (uintptr_t)my_glBindFramebuffer},
  {"glReadPixels", (uintptr_t)my_glReadPixels},
  {"glFramebufferTexture2D", (uintptr_t)my_glFramebufferTexture2D},
  {"glDrawElements", (uintptr_t)my_glDrawElements},
  {"glDrawArrays", (uintptr_t)my_glDrawArrays},
  /* VAO/MRT: core no Mali-G31(GLES3), via OES/EXT no Utgard, no-op se nao tiver */
  {"glGenVertexArrays", (uintptr_t)my_glGenVertexArrays},
  {"glBindVertexArray", (uintptr_t)my_glBindVertexArray},
  {"glDeleteVertexArrays", (uintptr_t)my_glDeleteVertexArrays},
  {"glDrawBuffers", (uintptr_t)my_glDrawBuffers},
  {"fopen", (uintptr_t)w_fopen},
  /* stat: ausentes como simbolo em glibc<2.33 -> via syscall (texto/fontes) */
  {"stat", (uintptr_t)my_stat}, {"lstat", (uintptr_t)my_lstat},
  {"fstat", (uintptr_t)my_fstat}, {"fstatat", (uintptr_t)my_fstatat},
  {"stat64", (uintptr_t)my_stat}, {"lstat64", (uintptr_t)my_lstat},
  {"fstat64", (uintptr_t)my_fstat}, {"fstatat64", (uintptr_t)my_fstatat},
  {"_ZTH7gString", (uintptr_t)tl_noop}, {"_ZTH8gString2", (uintptr_t)tl_noop},
  {"_ZTHN10ALCcontext13sLocalContextE", (uintptr_t)tl_noop},
  {"_Z24NVThreadGetCurrentJNIEnvv", (uintptr_t)NVThreadGetCurrentJNIEnv},
};
const int bully_stub_count = sizeof(bully_stub_table) / sizeof(bully_stub_table[0]);
