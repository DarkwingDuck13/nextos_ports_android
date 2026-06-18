#include <unistd.h>
/*
 * recon_egl.c — liga o EGL/ANativeWindow do Unity ao nosso egl_shim (SDL2/Mali).
 * Override das entradas da import table (chamar ANTES de so_resolve).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

#include "egl_shim.h"
#include "so_util.h"
#include "imports.h"

/* bridge dlopen/dlsym -> modulos do so-loader (libil2cpp/libunity) */
#include <stdlib.h>
extern so_module *g_m_il2cpp, *g_m_unity;
extern int egl_shim_width(void), egl_shim_height(void);

/* fwd: wrappers GL (HK Mali-450), definidos antes de recon_wire_egl */
static const unsigned char *my_glGetString(unsigned);
static void my_glGetIntegerv(unsigned, int *);
static void my_glShaderSource(unsigned, int, const char *const *, const int *);

/* stubs p/ funcoes GLES3 ausentes no Mali-450 (devolver STUB != NULL evita crash/loop) */
static long stub_gl_noop(void) { return 0; }
static void stub_glGetInternalformativ(unsigned t, unsigned f, unsigned pn, int n, int *p) {
  (void)t; (void)f; (void)pn;
  if (p) for (int i = 0; i < n; i++) p[i] = 0;  /* 0 samples = sem MSAA -> quebra o loop */
}
/* relogio monotonico em segundos desde o 1o uso (timestamps de log) */
#include <time.h>
static long hk_secs(void) {
  static long t0 = -1;
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  if (t0 < 0) t0 = ts.tv_sec;
  return ts.tv_sec - t0;
}
/* ---- ANTI-WEDGE Utgard (Bully: glFinish satura/wedge; Cuphead: tex 2048² trava) ----
   glFinish vira no-op (religa: HK_ALLOWFINISH=1). HK_TEXCAP=N limita textura a NxN:
   aloca reduzida (shift) + downsample point-sample no TexSubImage. */
static void noop_glFinish(void) {}
static int g_texcap = -1;
static int texcap(void) {
  if (g_texcap < 0) { const char *e = getenv("HK_TEXCAP"); g_texcap = e ? atoi(e) : 0; }
  return g_texcap;
}
static unsigned g_bound_tex = 0;                 /* textura GL_TEXTURE_2D bound */
#define TEXSHIFT_SLOTS 8192
static unsigned char g_tex_shift[TEXSHIFT_SLOTS]; /* halvings por texture id */
static void (*real_glBindTexture)(unsigned, unsigned) = 0;
static void my_glBindTexture(unsigned target, unsigned tex) {
  if (!real_glBindTexture)
    real_glBindTexture = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "glBindTexture");
  if (target == 0x0DE1) g_bound_tex = tex;        /* GL_TEXTURE_2D */
  if (real_glBindTexture) real_glBindTexture(target, tex);
}
static int tex_shift_for(int w, int h) {
  int cap = texcap(), sh = 0;
  if (cap <= 0) return 0;
  if (w != h) return 0;  /* so atlases QUADRADOS; render targets (1280x720 etc.) ficam intactos */
  while (((w >> sh) > cap || (h >> sh) > cap) && sh < 6) sh++;
  return sh;
}
static int fmt_bpp(unsigned fmt) {
  switch (fmt) {
    case 0x1908: case 0x80E1: return 4;  /* RGBA / BGRA */
    case 0x1907: return 3;               /* RGB */
    case 0x190A: case 0x8227: return 2;  /* LUMINANCE_ALPHA / RG */
    case 0x1903: case 0x1909: case 0x1906: return 1;  /* RED / LUMINANCE / ALPHA */
    default: return 0;
  }
}
static int type_bpc(unsigned type) {
  switch (type) {
    case 0x1401: return 1;               /* GL_UNSIGNED_BYTE */
    case 0x140B: case 0x8D61: return 2;  /* GL_HALF_FLOAT / GL_HALF_FLOAT_OES */
    case 0x1406: return 4;               /* GL_FLOAT */
    default: return 0;
  }
}
static float half_to_float_u01(unsigned short h) {
  int sign = (h >> 15) & 1, exp = (h >> 10) & 31, frac = h & 1023;
  float v;
  if (exp == 0) {
    v = frac ? ((float)frac / 1024.0f) : 0.0f;
    for (int i = 0; i < 14; i++) v *= 0.5f;
  } else if (exp == 31) {
    v = 1.0f;
  } else {
    v = 1.0f + (float)frac / 1024.0f;
    int e = exp - 15;
    while (e > 0) { v *= 2.0f; e--; }
    while (e < 0) { v *= 0.5f; e++; }
  }
  return sign ? -v : v;
}
static unsigned char float_to_u8(float v) {
  if (!(v > 0.0f)) return 0;
  if (v >= 1.0f) return 255;
  return (unsigned char)(v * 255.0f + 0.5f);
}
static int translate_upload_format(unsigned fmt, unsigned type,
                                   unsigned *out_fmt, unsigned *out_type) {
  *out_fmt = fmt;
  *out_type = type;
  if (fmt == 0x1903) *out_fmt = 0x1909;       /* GL_RED -> GL_LUMINANCE */
  else if (fmt == 0x8227) *out_fmt = 0x190A;  /* GL_RG  -> GL_LUMINANCE_ALPHA */
  if (type == 0x140B || type == 0x8D61 || type == 0x1406)
    *out_type = 0x1401;                       /* float/half -> byte */
  return (*out_fmt != fmt) || (*out_type != type);
}
static void sample_upload_pixel(unsigned char *dst, const unsigned char *src,
                                int comps, unsigned type) {
  if (type == 0x1401) {
    memcpy(dst, src, (size_t)comps);
    return;
  }
  for (int c = 0; c < comps; c++) {
    if (type == 0x140B || type == 0x8D61) {
      unsigned short h;
      memcpy(&h, src + c * 2, sizeof h);
      dst[c] = float_to_u8(half_to_float_u01(h));
    } else if (type == 0x1406) {
      float f;
      memcpy(&f, src + c * 4, sizeof f);
      dst[c] = float_to_u8(f);
    } else {
      dst[c] = 0;
    }
  }
}
static int hk_int_env(const char *name, int def) {
  const char *e = getenv(name);
  return (e && *e) ? atoi(e) : def;
}
/* glTexStorage2D (GLES3, storage imutavel) -> aloca via glTexImage2D (GLES2). SEM isso a
   textura nao existe -> glTexSubImage2D falha -> UI amostra vazio -> TELA PRETA. */
static int (*real_glGetError)(void) = 0;
static void (*real_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) = 0;
static void stub_glTexStorage2D(unsigned target, int levels, unsigned ifmt, int w, int h) {
  if (!real_glTexImage2D)
    real_glTexImage2D = (void (*)(unsigned, int, int, int, int, int, unsigned, unsigned,
                                  const void *))dlsym(RTLD_DEFAULT, "glTexImage2D");
  if (!real_glGetError) real_glGetError = (int (*)(void))dlsym(RTLD_DEFAULT, "glGetError");
  unsigned fmt = 0x1908, type = 0x1401;  /* GL_RGBA, GL_UNSIGNED_BYTE */
  switch (ifmt) {
    case 0x8051: case 0x1907: fmt = 0x1907; break;             /* RGB8/RGB -> GL_RGB */
    case 0x8D62: fmt = 0x1907; type = 0x8363; break;           /* RGB565 -> GL_RGB + 5_6_5 */
    case 0x8056: fmt = 0x1908; type = 0x8033; break;           /* RGBA4 -> GL_RGBA + 4_4_4_4 */
    case 0x8057: fmt = 0x1908; type = 0x8034; break;           /* RGB5_A1 -> GL_RGBA + 5_5_5_1 */
    case 0x8229: case 0x822D: case 0x822E: fmt = 0x1909; break;/* R8/R16F/R32F -> GL_LUMINANCE */
    case 0x822B: case 0x822F: case 0x8230: fmt = 0x190A; break;/* RG8/RG16F/RG32F -> GL_LUMINANCE_ALPHA */
    case 0x881B: case 0x8815: fmt = 0x1907; break;             /* RGB16F/RGB32F -> GL_RGB */
    case 0x881A: case 0x8814: fmt = 0x1908; break;             /* RGBA16F/RGBA32F -> GL_RGBA */
    default: break;                                            /* RGBA8/RGBA -> GL_RGBA */
  }
  int sh = tex_shift_for(w, h);                  /* HK_TEXCAP: aloca reduzida */
  if (sh && g_bound_tex < TEXSHIFT_SLOTS) g_tex_shift[g_bound_tex] = (unsigned char)sh;
  int aw = w >> sh, ah = h >> sh; if (aw < 1) aw = 1; if (ah < 1) ah = 1;
  static int tn = 0;
  if (tn < 25) { fprintf(stderr, "[TEXSTOR] %dx%d ifmt=0x%x -> fmt=0x%x lv=%d shift=%d\n", w, h, ifmt, fmt, levels, sh); tn++; }
  if (levels < 1) levels = 1;
  if (!real_glTexImage2D) return;
  for (int i = 0; i < levels; i++) {
    int lw = aw >> i, lh = ah >> i; if (lw < 1) lw = 1; if (lh < 1) lh = 1;
    int faces = target == 0x8513 ? 6 : 1;  /* GL_TEXTURE_CUBE_MAP -> 6 faces */
    for (int f = 0; f < faces; f++) {
      unsigned t = target == 0x8513 ? (0x8515u + (unsigned)f) : target;
      if (real_glGetError) for (int k = 0; k < 8 && real_glGetError(); k++) {}
      real_glTexImage2D(t, i, (int)fmt, lw, lh, 0, fmt, type, 0);
      if (real_glGetError) {
        int e = real_glGetError();
        static int en = 0;
        if (e && en < 20) {
          fprintf(stderr, "[TEXSTOR-ERR] target=0x%x face=0x%x ifmt=0x%x fmt=0x%x type=0x%x level=%d %dx%d err=0x%x\n",
                  target, t, ifmt, fmt, type, i, lw, lh, e);
          en++;
        }
      }
    }
  }
}
/* SYNC objects (GLES3) ausentes no Mali-450: fingir "ja sinalizado" p/ a engine NAO
   esperar pra sempre (senao trava ANTES do eglSwapBuffers -> tela preta). */
static void *stub_glFenceSync(unsigned cond, unsigned flags) { (void)cond; (void)flags; return (void *)1; }
static unsigned stub_glClientWaitSync(void *s, unsigned f, unsigned long long t) {
  (void)s; (void)f; (void)t; return 0x911A; /* GL_ALREADY_SIGNALED */
}
static void stub_glGetSynciv(void *s, unsigned pname, int bufSize, int *length, int *values) {
  (void)s; (void)bufSize;
  if (pname == 0x9114 && values) values[0] = 0x9119; /* GL_SYNC_STATUS -> GL_SIGNALED */
  if (length) *length = 1;
}
/* DIAGNOSTICO de textura (caca a tela preta): o upload casa com a alocacao? ha erro GL? */
static void (*real_glTexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) = 0;
static void diag_glTexSubImage2D(unsigned t, int lv, int xo, int yo, int w, int h,
                                 unsigned fmt, unsigned type, const void *px) {
  if (!real_glTexSubImage2D) real_glTexSubImage2D = (void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *))dlsym(RTLD_DEFAULT, "glTexSubImage2D");
  if (!real_glGetError) real_glGetError = (int (*)(void))dlsym(RTLD_DEFAULT, "glGetError");
  /* HK_TEXCAP: textura alocada reduzida -> downsample point-sample do upload */
  int sh = (g_bound_tex < TEXSHIFT_SLOTS) ? g_tex_shift[g_bound_tex] : 0;
  unsigned upload_fmt = fmt, upload_type = type;
  int convert = translate_upload_format(fmt, type, &upload_fmt, &upload_type);
  if ((sh || convert) && px) {
    int bpp = fmt_bpp(fmt), bpc = type_bpc(type);
    int dw = w >> sh, dh = h >> sh;
    static long long capb = 0;
    int src_bpp = (bpp && bpc) ? bpp * bpc : 4;
    capb += (long long)w * h * src_bpp;
    int skip_after = hk_int_env("HK_TEXSKIP_AFTER_MB", 0);
    if (skip_after > 0 && (capb >> 20) > skip_after && w >= 1024 && h >= 1024) {
      static int sn = 0;
      if (sn < 20 || (sn % 25) == 0)
        fprintf(stderr, "[TEXSUB-SKIP] #%d t=%lds %dx%d>>%d total=%lldMB limit=%dMB\n",
                sn, hk_secs(), w, h, sh, capb >> 20, skip_after);
      sn++;
      return;
    }
    if (bpp && bpc && dw >= 1 && dh >= 1) {
      unsigned char *out = (unsigned char *)malloc((size_t)dw * dh * bpp);
      int step = 1 << sh;
      if (!convert && bpp == 4 && type == 0x1401) {  /* caminho rapido: copia por palavra (RGBA) */
        unsigned *o32 = (unsigned *)out;
        for (int y = 0; y < dh; y++) {
          const unsigned *row = (const unsigned *)((const unsigned char *)px + (size_t)(y * step) * w * 4);
          for (int x = 0; x < dw; x++) o32[(size_t)y * dw + x] = row[(size_t)x << sh];
        }
      } else {
        const unsigned char *in = (const unsigned char *)px;
        for (int y = 0; y < dh; y++)
          for (int x = 0; x < dw; x++)
            sample_upload_pixel(out + ((size_t)y * dw + x) * bpp,
                                in + ((size_t)(y * step) * w + (x * step)) * src_bpp,
                                bpp, type);
      }
      if (real_glGetError) for (int k = 0; k < 8 && real_glGetError(); k++) {}
      if (real_glTexSubImage2D)
        real_glTexSubImage2D(t, lv, xo >> sh, yo >> sh, dw, dh, upload_fmt, upload_type, out);
      if (real_glGetError) {
        int e = real_glGetError();
        static int en = 0;
        if (e && en < 20) {
          fprintf(stderr, "[TEXSUB-CAP-ERR] target=0x%x level=%d %dx%d>>%d fmt=0x%x->0x%x type=0x%x->0x%x err=0x%x\n",
                  t, lv, w, h, sh, fmt, upload_fmt, type, upload_type, e);
          en++;
        }
      }
      /* Utgard pode ADIAR a leitura da textura -> free imediato = risco UAF no driver.
         Anel de 8 buffers adia o free. glFlush por upload TESTADO E REPROVADO (run8:
         wedge ainda mais cedo). Pacing usleep da um respiro pra fila da GPU. */
      static void *ring[8]; static int ri = 0;
      if (ring[ri]) free(ring[ri]);
      ring[ri] = out; ri = (ri + 1) & 7;
      static int ts = -1;
      if (ts < 0) ts = hk_int_env("HK_TEXSLEEP_MS", 10);
      if (ts > 0) usleep((unsigned)ts * 1000);
    } /* sub-regiao menor que o fator: pula (1 texel de borda, inofensivo) */
    static int dn = 0;
    if (dn < 10 || (dn % 25) == 0)
      fprintf(stderr, "[TEXSUB-CAP] #%d t=%lds %dx%d>>%d fmt=0x%x->0x%x type=0x%x->0x%x total=%lldMB\n",
              dn, hk_secs(), w, h, sh, fmt, upload_fmt, type, upload_type, capb >> 20);
    dn++;
    return;
  }
  if (real_glGetError) for (int k = 0; k < 8 && real_glGetError(); k++) {}
  if (real_glTexSubImage2D) real_glTexSubImage2D(t, lv, xo, yo, w, h, upload_fmt, upload_type, px);
  static int n = 0;
  if (n < 25) { int e = real_glGetError ? real_glGetError() : 0;
    fprintf(stderr, "[TEXSUB] target=0x%x %dx%d fmt=0x%x->0x%x type=0x%x->0x%x px=%p err=0x%x\n",
            t, w, h, fmt, upload_fmt, type, upload_type, px, e); n++; }
}
static void (*real_glCompressedTexImage2D)(unsigned, int, unsigned, int, int, int, int, const void *) = 0;
static void diag_glCompressedTexImage2D(unsigned t, int lv, unsigned ifmt, int w, int h, int b, int sz, const void *d) {
  if (!real_glCompressedTexImage2D) real_glCompressedTexImage2D = (void (*)(unsigned, int, unsigned, int, int, int, int, const void *))dlsym(RTLD_DEFAULT, "glCompressedTexImage2D");
  static int n = 0;
  if (n < 25) { fprintf(stderr, "[CTEX] %dx%d ifmt=0x%x sz=%d\n", w, h, ifmt, sz); n++; }
  if (real_glCompressedTexImage2D) real_glCompressedTexImage2D(t, lv, ifmt, w, h, b, sz, d);
}
/* FBO: a render target esta COMPLETA? (incompleta = render-to-texture preto) */
static unsigned (*real_glCheckFramebufferStatus)(unsigned) = 0;
static unsigned diag_glCheckFramebufferStatus(unsigned target) {
  if (!real_glCheckFramebufferStatus) real_glCheckFramebufferStatus = (unsigned (*)(unsigned))dlsym(RTLD_DEFAULT, "glCheckFramebufferStatus");
  unsigned s = real_glCheckFramebufferStatus ? real_glCheckFramebufferStatus(target) : 0x8CD5;
  static int n = 0;
  if (n < 20) { fprintf(stderr, "[FBO] CheckStatus -> 0x%x %s\n", s, s == 0x8CD5 ? "COMPLETE" : "INCOMPLETE!"); n++; }
  return s;
}
/* glMapBufferRange/glUnmapBuffer (GLES3) -> EMULA com malloc + glBufferSubData no unmap.
   E assim que a Unity escreve GEOMETRIA DINAMICA; no-op = buffers vazios = TELA PRETA. */
static struct { unsigned target; long offset; long length; void *ptr; } g_mapbuf[16];
static void (*real_glBufferSubData)(unsigned, long, long, const void *) = 0;
static void (*real_glBufferData)(unsigned, long, const void *, unsigned) = 0;
static unsigned char *ubo_shadow(unsigned id, long need);  /* fwd (emulacao UBO) */
static unsigned g_bound_ubo_fwd(void);
static void *emul_glMapBufferRange(unsigned target, long offset, long length, unsigned access) {
  (void)access;
  if (target == 0x8A11) {  /* GL_UNIFORM_BUFFER: escreve DIRETO no shadow CPU */
    unsigned char *d = ubo_shadow(g_bound_ubo_fwd(), offset + length);
    static int un = 0; if (un < 6) { fprintf(stderr, "[MAPBUF] UBO map off=%ld len=%ld\n", offset, length); un++; }
    return d ? d + offset : 0;
  }
  void *p = malloc(length > 0 ? (size_t)length : 1);
  for (int i = 0; i < 16; i++)
    if (!g_mapbuf[i].ptr) { g_mapbuf[i].target = target; g_mapbuf[i].offset = offset;
                            g_mapbuf[i].length = length; g_mapbuf[i].ptr = p; break; }
  static int n = 0; if (n < 8) { fprintf(stderr, "[MAPBUF] map t=0x%x off=%ld len=%ld\n", target, offset, length); n++; }
  return p;
}
static unsigned char emul_glUnmapBuffer(unsigned target) {
  if (!real_glBufferSubData) real_glBufferSubData = (void (*)(unsigned, long, long, const void *))dlsym(RTLD_DEFAULT, "glBufferSubData");
  for (int i = 0; i < 16; i++)
    if (g_mapbuf[i].ptr && g_mapbuf[i].target == target) {
      static int dn = 0;
      if (target == 0x8892 && dn < 4 && g_mapbuf[i].length >= 256) { /* ARRAY_BUFFER: dump vertices */
        float *fp = (float *)g_mapbuf[i].ptr;
        int nf = g_mapbuf[i].length / 4, nz = 0;
        for (int k = 0; k < nf; k++) if (fp[k] != 0.0f) nz++;
        int m = nf / 2;
        fprintf(stderr, "[VTX] len=%ld nao-zero=%d/%d | f0-5: %.2f %.2f %.2f %.2f %.2f %.2f | meio[%d]: %.2f %.2f %.2f %.2f\n",
                g_mapbuf[i].length, nz, nf, fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], m, fp[m], fp[m+1], fp[m+2], fp[m+3]);
        dn++;
      }
      if (!real_glBufferData) real_glBufferData = (void (*)(unsigned, long, const void *, unsigned))dlsym(RTLD_DEFAULT, "glBufferData");
      if (!real_glGetError) real_glGetError = (int (*)(void))dlsym(RTLD_DEFAULT, "glGetError");
      if (real_glGetError) real_glGetError(); /* limpa */
      /* glBufferData (re-aloca + sobe) p/ offset 0 -- robusto se a Unity nao pre-alocou via glBufferData */
      if (g_mapbuf[i].offset == 0 && real_glBufferData)
        real_glBufferData(target, g_mapbuf[i].length, g_mapbuf[i].ptr, 0x88E8); /* GL_DYNAMIC_DRAW */
      else if (real_glBufferSubData)
        real_glBufferSubData(target, g_mapbuf[i].offset, g_mapbuf[i].length, g_mapbuf[i].ptr);
      static int ue = 0;
      if (ue < 6 && real_glGetError) { int e = real_glGetError();
        fprintf(stderr, "[UNMAP] t=0x%x off=%ld len=%ld upload_err=0x%x\n", target, g_mapbuf[i].offset, g_mapbuf[i].length, e); ue++; }
      free(g_mapbuf[i].ptr); g_mapbuf[i].ptr = 0;
      return 1; /* GL_TRUE */
    }
  return 1;
}
/* ---- EMULACAO UBO (GL_UNIFORM_BUFFER 0x8A11) + INSTANCING GLES3->GLES2 ----
   Os sprites do HK usam instancing: matrizes por-instancia vao num UBO
   (UnityInstancing_PerDraw0/PerDrawSprite) + glDrawElementsInstanced. Em GLES2
   nada disso existe -> shadow CPU do UBO + uniforms planos (tradutor desliga
   HLSLCC_ENABLE_UNIFORM_BUFFERS) + loop de draws com u_hk_instID. */
static unsigned g_cur_program = 0;
static unsigned g_cur_fbo = 0;  /* 0 = tela (default framebuffer) */
static void (*real_glBindFramebuffer)(unsigned, unsigned) = 0;
static void my_glBindFramebuffer(unsigned target, unsigned fb) {
  if (!real_glBindFramebuffer) real_glBindFramebuffer = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "glBindFramebuffer");
  if (target == 0x8D40 || target == 0x8CA9) g_cur_fbo = fb;  /* FRAMEBUFFER / DRAW_FRAMEBUFFER */
  if (real_glBindFramebuffer) real_glBindFramebuffer(target == 0x8CA9 ? 0x8D40 : target, fb);
}
static void (*real_glUseProgram)(unsigned) = 0;
static void my_glUseProgram(unsigned p) {
  if (!real_glUseProgram) real_glUseProgram = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glUseProgram");
  g_cur_program = p;
  if (real_glUseProgram) real_glUseProgram(p);
}
#define UBON 256
static struct { unsigned id; unsigned char *data; long size; } g_ubostore[UBON];
static unsigned g_bound_ubo = 0;
static unsigned g_bound_ubo_fwd(void) { return g_bound_ubo; }
static struct { unsigned buf; long offset; } g_ubo_bindpt[8];
static unsigned char *ubo_shadow(unsigned id, long need) {
  if (!id) return 0;
  int free_i = -1;
  for (int i = 0; i < UBON; i++) {
    if (g_ubostore[i].id == id) {
      if (need > g_ubostore[i].size) {
        g_ubostore[i].data = (unsigned char *)realloc(g_ubostore[i].data, (size_t)need);
        memset(g_ubostore[i].data + g_ubostore[i].size, 0, (size_t)(need - g_ubostore[i].size));
        g_ubostore[i].size = need;
      }
      return g_ubostore[i].data;
    }
    if (free_i < 0 && !g_ubostore[i].id) free_i = i;
  }
  if (free_i < 0) return 0;
  g_ubostore[free_i].id = id;
  g_ubostore[free_i].size = need > 0 ? need : 16384;
  g_ubostore[free_i].data = (unsigned char *)calloc(1, (size_t)g_ubostore[free_i].size);
  return g_ubostore[free_i].data;
}
static void (*real_glBindBuffer)(unsigned, unsigned) = 0;
static unsigned g_bound_arraybuf = 0;
static void my_glBindBuffer(unsigned target, unsigned buf) {
  if (target == 0x8A11) { g_bound_ubo = buf; return; } /* driver GLES2 nao conhece */
  if (target == 0x8892) g_bound_arraybuf = buf;         /* GL_ARRAY_BUFFER */
  if (!real_glBindBuffer) real_glBindBuffer = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "glBindBuffer");
  if (real_glBindBuffer) real_glBindBuffer(target, buf);
}
static void (*real_glBufferData2)(unsigned, long, const void *, unsigned) = 0;
static void my_glBufferData(unsigned target, long size, const void *data, unsigned usage) {
  if (target == 0x8A11) {
    unsigned char *d = ubo_shadow(g_bound_ubo, size);
    if (d && data) memcpy(d, data, (size_t)size);
    return;
  }
  if (!real_glBufferData2) real_glBufferData2 = (void (*)(unsigned, long, const void *, unsigned))dlsym(RTLD_DEFAULT, "glBufferData");
  if (real_glBufferData2) real_glBufferData2(target, size, data, usage);
}
static void (*real_glBufferSubData2)(unsigned, long, long, const void *) = 0;
static void my_glBufferSubData(unsigned target, long off, long size, const void *data) {
  if (target == 0x8A11) {
    unsigned char *d = ubo_shadow(g_bound_ubo, off + size);
    if (d && data) memcpy(d + off, data, (size_t)size);
    return;
  }
  if (!real_glBufferSubData2) real_glBufferSubData2 = (void (*)(unsigned, long, long, const void *))dlsym(RTLD_DEFAULT, "glBufferSubData");
  if (real_glBufferSubData2) real_glBufferSubData2(target, off, size, data);
}
static void emul_glBindBufferRange(unsigned target, unsigned index, unsigned buf, long offset, long size) {
  (void)size;
  if (target == 0x8A11 && index < 8) { g_ubo_bindpt[index].buf = buf; g_ubo_bindpt[index].offset = offset; }
}
static void emul_glBindBufferBase(unsigned target, unsigned index, unsigned buf) {
  emul_glBindBufferRange(target, index, buf, 0, 0);
}
/* FBO COLOR -> textura: o HK renderiza em render target e depois faz composite por
   triangulo para a tela. No Mali-450 atual esse composite cola em 2 pixels; este
   rastreio permite testar/provar e substituir o ultimo passo sem mexer no resto. */
#define HK_FBOS 128
static struct { unsigned fb; unsigned color_tex; } g_fbotex[HK_FBOS];
static unsigned g_last_color_tex = 0;
static void (*real_glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int) = 0;
static unsigned fbo_color_tex(unsigned fb) {
  for (int i = 0; i < HK_FBOS; i++)
    if (g_fbotex[i].fb == fb) return g_fbotex[i].color_tex;
  return 0;
}
static void record_fbo_color_tex(unsigned fb, unsigned tex) {
  if (!fb) return;
  int free_i = -1;
  for (int i = 0; i < HK_FBOS; i++) {
    if (g_fbotex[i].fb == fb) { g_fbotex[i].color_tex = tex; break; }
    if (free_i < 0 && !g_fbotex[i].fb) free_i = i;
    if (i == HK_FBOS - 1 && free_i >= 0) {
      g_fbotex[free_i].fb = fb;
      g_fbotex[free_i].color_tex = tex;
    }
  }
  if (tex) g_last_color_tex = tex;
}
static void diag_glFramebufferTexture2D(unsigned target, unsigned attachment,
                                        unsigned textarget, unsigned tex, int level) {
  if (!real_glFramebufferTexture2D)
    real_glFramebufferTexture2D = (void (*)(unsigned, unsigned, unsigned, unsigned, int))dlsym(RTLD_DEFAULT, "glFramebufferTexture2D");
  if ((target == 0x8D40 || target == 0x8CA9) && attachment == 0x8CE0) { /* COLOR_ATTACHMENT0 */
    record_fbo_color_tex(g_cur_fbo, tex);
    static int n = 0;
    if (n < 16)
      fprintf(stderr, "[FBO-TEX] fb=%u color_tex=%u textarget=0x%x level=%d\n",
              g_cur_fbo, tex, textarget, level);
    n++;
  }
  if (real_glFramebufferTexture2D)
    real_glFramebufferTexture2D(target == 0x8CA9 ? 0x8D40 : target, attachment, textarget, tex, level);
}
static int hk_env_on(const char *name) {
  const char *e = getenv(name);
  return e && *e && strcmp(e, "0") != 0;
}
static unsigned (*p_glCreateShader)(unsigned) = 0;
static void (*p_glDeleteShader)(unsigned) = 0;
static void (*p_glShaderSource)(unsigned, int, const char *const *, const int *) = 0;
static void (*p_glCompileShader)(unsigned) = 0;
static unsigned (*p_glCreateProgram)(void) = 0;
static void (*p_glAttachShader)(unsigned, unsigned) = 0;
static void (*p_glLinkProgram)(unsigned) = 0;
static void (*p_glGetShaderiv)(unsigned, unsigned, int *) = 0;
static void (*p_glGetProgramiv)(unsigned, unsigned, int *) = 0;
static void (*p_glGetShaderInfoLog)(unsigned, int, int *, char *) = 0;
static void (*p_glGetProgramInfoLog)(unsigned, int, int *, char *) = 0;
static int (*p_glGetAttribLocation)(unsigned, const char *) = 0;
static int (*p_glGetUniformLocation)(unsigned, const char *) = 0;
static void (*p_glUniform1i)(int, int) = 0;
static void (*p_glVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *) = 0;
static void (*p_glEnableVertexAttribArray)(unsigned) = 0;
static void (*p_glDisableVertexAttribArray)(unsigned) = 0;
static void (*p_glActiveTexture)(unsigned) = 0;
static void (*p_glDisable)(unsigned) = 0;
static void (*p_glViewport)(int, int, int, int) = 0;
static void (*real_glDrawArrays)(unsigned, int, int) = 0;
static struct {
  int enabled, size, stride;
  unsigned type, norm, buf;
  const void *ptr;
} g_attr[16];
#define HK_SHMAP 512
static struct { unsigned sh; int dump_id; int is_frag; } g_shmap[HK_SHMAP];
#define HK_PROGMAP 128
static struct { unsigned prog; unsigned sh[8]; int n; } g_progmap[HK_PROGMAP];
typedef struct { unsigned sh[8]; int n; } hk_prog_shaders;
static unsigned g_force_red_prog = 0;
static unsigned g_force_blit_prog = 0;
static void force_resolve_gl(void) {
  if (!p_glCreateShader) p_glCreateShader = (unsigned (*)(unsigned))dlsym(RTLD_DEFAULT, "glCreateShader");
  if (!p_glDeleteShader) p_glDeleteShader = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glDeleteShader");
  if (!p_glShaderSource) p_glShaderSource = (void (*)(unsigned, int, const char *const *, const int *))dlsym(RTLD_DEFAULT, "glShaderSource");
  if (!p_glCompileShader) p_glCompileShader = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glCompileShader");
  if (!p_glCreateProgram) p_glCreateProgram = (unsigned (*)(void))dlsym(RTLD_DEFAULT, "glCreateProgram");
  if (!p_glAttachShader) p_glAttachShader = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "glAttachShader");
  if (!p_glLinkProgram) p_glLinkProgram = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glLinkProgram");
  if (!p_glGetShaderiv) p_glGetShaderiv = (void (*)(unsigned, unsigned, int *))dlsym(RTLD_DEFAULT, "glGetShaderiv");
  if (!p_glGetProgramiv) p_glGetProgramiv = (void (*)(unsigned, unsigned, int *))dlsym(RTLD_DEFAULT, "glGetProgramiv");
  if (!p_glGetShaderInfoLog) p_glGetShaderInfoLog = (void (*)(unsigned, int, int *, char *))dlsym(RTLD_DEFAULT, "glGetShaderInfoLog");
  if (!p_glGetProgramInfoLog) p_glGetProgramInfoLog = (void (*)(unsigned, int, int *, char *))dlsym(RTLD_DEFAULT, "glGetProgramInfoLog");
  if (!p_glGetAttribLocation) p_glGetAttribLocation = (int (*)(unsigned, const char *))dlsym(RTLD_DEFAULT, "glGetAttribLocation");
  if (!p_glGetUniformLocation) p_glGetUniformLocation = (int (*)(unsigned, const char *))dlsym(RTLD_DEFAULT, "glGetUniformLocation");
  if (!p_glUniform1i) p_glUniform1i = (void (*)(int, int))dlsym(RTLD_DEFAULT, "glUniform1i");
  if (!p_glVertexAttribPointer) p_glVertexAttribPointer = (void (*)(unsigned, int, unsigned, unsigned char, int, const void *))dlsym(RTLD_DEFAULT, "glVertexAttribPointer");
  if (!p_glEnableVertexAttribArray) p_glEnableVertexAttribArray = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glEnableVertexAttribArray");
  if (!p_glDisableVertexAttribArray) p_glDisableVertexAttribArray = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glDisableVertexAttribArray");
  if (!p_glActiveTexture) p_glActiveTexture = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glActiveTexture");
  if (!p_glDisable) p_glDisable = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glDisable");
  if (!p_glViewport) p_glViewport = (void (*)(int, int, int, int))dlsym(RTLD_DEFAULT, "glViewport");
  if (!real_glDrawArrays) real_glDrawArrays = (void (*)(unsigned, int, int))dlsym(RTLD_DEFAULT, "glDrawArrays");
  if (!real_glBindTexture) real_glBindTexture = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "glBindTexture");
  if (!real_glUseProgram) real_glUseProgram = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glUseProgram");
  if (!real_glBindBuffer) real_glBindBuffer = (void (*)(unsigned, unsigned))dlsym(RTLD_DEFAULT, "glBindBuffer");
  if (!real_glGetError) real_glGetError = (int (*)(void))dlsym(RTLD_DEFAULT, "glGetError");
}
static void shmap_set(unsigned sh, int dump_id, int is_frag) {
  int free_i = -1;
  for (int i = 0; i < HK_SHMAP; i++) {
    if (g_shmap[i].sh == sh) { g_shmap[i].dump_id = dump_id; g_shmap[i].is_frag = is_frag; return; }
    if (free_i < 0 && !g_shmap[i].sh) free_i = i;
  }
  if (free_i >= 0) { g_shmap[free_i].sh = sh; g_shmap[free_i].dump_id = dump_id; g_shmap[free_i].is_frag = is_frag; }
}
static int shmap_dump(unsigned sh) {
  for (int i = 0; i < HK_SHMAP; i++) if (g_shmap[i].sh == sh) return g_shmap[i].dump_id;
  return -1;
}
static void prog_attach(unsigned prog, unsigned sh) {
  int free_i = -1;
  for (int i = 0; i < HK_PROGMAP; i++) {
    if (g_progmap[i].prog == prog) {
      if (g_progmap[i].n < 8) g_progmap[i].sh[g_progmap[i].n++] = sh;
      return;
    }
    if (free_i < 0 && !g_progmap[i].prog) free_i = i;
  }
  if (free_i >= 0) {
    g_progmap[free_i].prog = prog;
    g_progmap[free_i].sh[0] = sh;
    g_progmap[free_i].n = 1;
  }
}
static hk_prog_shaders prog_shaders(unsigned prog) {
  hk_prog_shaders r; memset(&r, 0, sizeof r);
  for (int i = 0; i < HK_PROGMAP; i++)
    if (g_progmap[i].prog == prog) {
      r.n = g_progmap[i].n;
      for (int k = 0; k < r.n && k < 8; k++) r.sh[k] = g_progmap[i].sh[k];
      break;
    }
  return r;
}
static void diag_glAttachShader(unsigned prog, unsigned sh) {
  force_resolve_gl();
  prog_attach(prog, sh);
  if (p_glAttachShader) p_glAttachShader(prog, sh);
}
static void diag_glLinkProgram(unsigned prog) {
  force_resolve_gl();
  if (p_glLinkProgram) p_glLinkProgram(prog);
  int ok = 0;
  if (p_glGetProgramiv) p_glGetProgramiv(prog, 0x8B82, &ok); /* LINK_STATUS */
  hk_prog_shaders ps = prog_shaders(prog);
  static int n = 0;
  if (n < 80 || !ok) {
    fprintf(stderr, "[LINK] prog=%u ok=%d shaders=", prog, ok);
    for (int i = 0; i < ps.n; i++)
      fprintf(stderr, "%s%u(dump=%d)", i ? "," : "", ps.sh[i], shmap_dump(ps.sh[i]));
    if (!ok && p_glGetProgramInfoLog) {
      char log[1024]; int len = 0;
      p_glGetProgramInfoLog(prog, sizeof log, &len, log);
      fprintf(stderr, " log=%.*s", len, log);
    }
    fprintf(stderr, "\n");
    n++;
  }
}
static unsigned make_force_program(int textured) {
  force_resolve_gl();
  if (!p_glCreateShader || !p_glShaderSource || !p_glCompileShader || !p_glCreateProgram ||
      !p_glAttachShader || !p_glLinkProgram) return 0;
  const char *vs =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_uv;\n"
      "varying vec2 v_uv;\n"
      "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
  const char *fs_red =
      "precision mediump float;\n"
      "void main(){ gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";
  const char *fs_tex =
      "precision mediump float;\n"
      "varying vec2 v_uv;\n"
      "uniform sampler2D u_tex;\n"
      "void main(){ gl_FragColor = texture2D(u_tex, v_uv); }\n";
  unsigned shv = p_glCreateShader(0x8B31), shf = p_glCreateShader(0x8B30);
  unsigned prog = p_glCreateProgram();
  p_glShaderSource(shv, 1, &vs, 0);
  p_glShaderSource(shf, 1, textured ? &fs_tex : &fs_red, 0);
  p_glCompileShader(shv);
  p_glCompileShader(shf);
  diag_glAttachShader(prog, shv);
  diag_glAttachShader(prog, shf);
  diag_glLinkProgram(prog);
  if (p_glGetProgramiv) {
    int ok = 0; p_glGetProgramiv(prog, 0x8B82, &ok); /* LINK_STATUS */
    if (!ok) {
      char log[512]; int len = 0;
      if (p_glGetProgramInfoLog) p_glGetProgramInfoLog(prog, sizeof log, &len, log);
      fprintf(stderr, "[FORCE-DRAW] link failed textured=%d: %.*s\n", textured, len, log);
    }
  }
  return prog;
}
static void force_screen_draw(unsigned tex) {
  int textured = tex != 0;
  unsigned *prog_slot = textured ? &g_force_blit_prog : &g_force_red_prog;
  if (!*prog_slot) *prog_slot = make_force_program(textured);
  if (!*prog_slot) return;
  force_resolve_gl();
  static const float v[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
  };
  unsigned save_prog = g_cur_program;
  unsigned save_array = g_bound_arraybuf;
  if (real_glBindFramebuffer) real_glBindFramebuffer(0x8D40, 0);
  if (p_glViewport) p_glViewport(0, 0, egl_shim_width(), egl_shim_height());
  if (p_glDisable) { p_glDisable(0x0C11); p_glDisable(0x0B71); p_glDisable(0x0BE2); } /* scissor/depth/blend */
  if (real_glUseProgram) real_glUseProgram(*prog_slot);
  if (real_glBindBuffer) real_glBindBuffer(0x8892, 0);
  if (textured) {
    int loc = p_glGetUniformLocation ? p_glGetUniformLocation(*prog_slot, "u_tex") : -1;
    if (p_glActiveTexture) p_glActiveTexture(0x84C0); /* TEXTURE0 */
    if (real_glBindTexture) real_glBindTexture(0x0DE1, tex);
    if (loc >= 0 && p_glUniform1i) p_glUniform1i(loc, 0);
  }
  int lp = p_glGetAttribLocation ? p_glGetAttribLocation(*prog_slot, "a_pos") : 0;
  int lu = p_glGetAttribLocation ? p_glGetAttribLocation(*prog_slot, "a_uv") : 1;
  if (lp >= 0 && p_glVertexAttribPointer && p_glEnableVertexAttribArray) {
    p_glEnableVertexAttribArray((unsigned)lp);
    p_glVertexAttribPointer((unsigned)lp, 2, 0x1406, 0, 4 * (int)sizeof(float), v);
  }
  if (lu >= 0 && p_glVertexAttribPointer && p_glEnableVertexAttribArray) {
    p_glEnableVertexAttribArray((unsigned)lu);
    p_glVertexAttribPointer((unsigned)lu, 2, 0x1406, 0, 4 * (int)sizeof(float), v + 2);
  }
  if (real_glDrawArrays) real_glDrawArrays(0x0004, 0, 6); /* TRIANGLES */
  if (lp >= 0 && p_glDisableVertexAttribArray) p_glDisableVertexAttribArray((unsigned)lp);
  if (lu >= 0 && p_glDisableVertexAttribArray) p_glDisableVertexAttribArray((unsigned)lu);
  if (real_glUseProgram) real_glUseProgram(save_prog);
  if (real_glBindBuffer) real_glBindBuffer(0x8892, save_array);
  if (real_glGetError) {
    static int n = 0;
    if (n < 12)
      fprintf(stderr, "[FORCE-DRAW] t=%lds textured=%d tex=%u err=0x%x\n",
              hk_secs(), textured, tex, real_glGetError());
    n++;
  }
}
static int diag_glGetAttribLocation(unsigned prog, const char *name) {
  force_resolve_gl();
  int loc = p_glGetAttribLocation ? p_glGetAttribLocation(prog, name) : -1;
  if (name && (!strcmp(name, "vertex") || !strcmp(name, "in_POSITION0") ||
               !strcmp(name, "a_pos") || !strcmp(name, "a_uv"))) {
    static int n = 0;
    if (n < 40) { fprintf(stderr, "[ATTRLOC] prog=%u %s -> %d\n", prog, name, loc); n++; }
  }
  return loc;
}
static int diag_glGetUniformLocation(unsigned prog, const char *name) {
  force_resolve_gl();
  int loc = p_glGetUniformLocation ? p_glGetUniformLocation(prog, name) : -1;
  if (name && (!strcmp(name, "tex") || !strcmp(name, "uvOffsetAndScale") ||
               !strcmp(name, "_MainTex") || !strcmp(name, "_MainTex_ST"))) {
    static int n = 0;
    if (n < 60) { fprintf(stderr, "[UNILOC] prog=%u %s -> %d\n", prog, name, loc); n++; }
  }
  return loc;
}
static void diag_glVertexAttribPointer(unsigned idx, int size, unsigned type,
                                       unsigned char norm, int stride, const void *ptr) {
  if (!p_glVertexAttribPointer)
    p_glVertexAttribPointer = (void (*)(unsigned, int, unsigned, unsigned char, int, const void *))dlsym(RTLD_DEFAULT, "glVertexAttribPointer");
  if (idx < 16) {
    g_attr[idx].size = size; g_attr[idx].type = type; g_attr[idx].norm = norm;
    g_attr[idx].stride = stride; g_attr[idx].ptr = ptr; g_attr[idx].buf = g_bound_arraybuf;
  }
  if (p_glVertexAttribPointer) p_glVertexAttribPointer(idx, size, type, norm, stride, ptr);
}
static void diag_glEnableVertexAttribArray(unsigned idx) {
  if (!p_glEnableVertexAttribArray)
    p_glEnableVertexAttribArray = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glEnableVertexAttribArray");
  if (idx < 16) g_attr[idx].enabled = 1;
  if (p_glEnableVertexAttribArray) p_glEnableVertexAttribArray(idx);
}
static void diag_glDisableVertexAttribArray(unsigned idx) {
  if (!p_glDisableVertexAttribArray)
    p_glDisableVertexAttribArray = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "glDisableVertexAttribArray");
  if (idx < 16) g_attr[idx].enabled = 0;
  if (p_glDisableVertexAttribArray) p_glDisableVertexAttribArray(idx);
}
static void force_native_final_draw(void) {
  force_resolve_gl();
  if (!g_cur_program || !p_glGetAttribLocation || !p_glVertexAttribPointer ||
      !p_glEnableVertexAttribArray || !real_glDrawArrays) return;
  int loc = p_glGetAttribLocation(g_cur_program, "vertex");
  if (loc < 0) return;
  static const float v[] = {
    -1.0f, -1.0f, 0.0f, 0.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 1.0f,
  };
  unsigned save_array = g_bound_arraybuf;
  if (real_glBindFramebuffer) real_glBindFramebuffer(0x8D40, 0);
  if (p_glViewport) p_glViewport(0, 0, egl_shim_width(), egl_shim_height());
  if (p_glDisable) { p_glDisable(0x0C11); p_glDisable(0x0B71); p_glDisable(0x0BE2); }
  if (real_glBindBuffer) real_glBindBuffer(0x8892, 0);
  p_glEnableVertexAttribArray((unsigned)loc);
  p_glVertexAttribPointer((unsigned)loc, 4, 0x1406, 0, 4 * (int)sizeof(float), v);
  real_glDrawArrays(0x0004, 0, 6);
  if (real_glBindBuffer) real_glBindBuffer(0x8892, save_array);
  if (real_glGetError) {
    static int n = 0;
    if (n < 12)
      fprintf(stderr, "[FIXFINAL] prog=%u vertex_loc=%d err=0x%x\n",
              g_cur_program, loc, real_glGetError());
    n++;
  }
}
/* DIAG: os draws acontecem e passam? (render preto = ou nao desenha ou erra) */
static void (*real_glDrawElements)(unsigned, int, unsigned, const void *) = 0;
static void diag_glDrawElements(unsigned mode, int count, unsigned type, const void *idx) {
  if (!real_glDrawElements) real_glDrawElements = (void (*)(unsigned, int, unsigned, const void *))dlsym(RTLD_DEFAULT, "glDrawElements");
  if (!real_glGetError) real_glGetError = (int (*)(void))dlsym(RTLD_DEFAULT, "glGetError");
  unsigned pre = real_glGetError ? (unsigned)real_glGetError() : 0; /* limpa + captura acumulado */
  if (real_glDrawElements) real_glDrawElements(mode, count, type, idx);
  if (g_cur_fbo == 0 && count == 3) {
    force_resolve_gl();
    int vloc = p_glGetAttribLocation ? p_glGetAttribLocation(g_cur_program, "vertex") : -1;
    int tloc = p_glGetUniformLocation ? p_glGetUniformLocation(g_cur_program, "tex") : -1;
    int uloc = p_glGetUniformLocation ? p_glGetUniformLocation(g_cur_program, "uvOffsetAndScale") : -1;
    static int fn = 0;
    if (fn < 12) {
      int ai = (vloc >= 0 && vloc < 16) ? vloc : 0;
      fprintf(stderr, "[FINAL] prog=%u vloc=%d texloc=%d uvloc=%d attr%d en=%d size=%d type=0x%x stride=%d buf=%u ptr=%p\n",
              g_cur_program, vloc, tloc, uloc, ai, g_attr[ai].enabled, g_attr[ai].size,
              g_attr[ai].type, g_attr[ai].stride, g_attr[ai].buf, g_attr[ai].ptr);
      fn++;
    }
    if (hk_env_on("HK_FIXFINAL")) force_native_final_draw();
  }
  if (g_cur_fbo == 0 && (hk_env_on("HK_FORCEFINAL") || hk_env_on("HK_FORCEBLIT"))) {
    unsigned tex = hk_env_on("HK_FORCEBLIT") ? (fbo_color_tex(1) ? fbo_color_tex(1) : g_last_color_tex) : 0;
    force_screen_draw(tex);
  }
  static int n = 0;
  /* periodico: 25 no inicio + rajadas de 8 a cada 5000 draws (ver o ESTADO ESTAVEL) */
  if (n < 25 || (n % 5000) < 8) { unsigned post = real_glGetError ? (unsigned)real_glGetError() : 0;
    fprintf(stderr, "[DRAW] #%d t=%lds mode=0x%x count=%d type=0x%x pre=0x%x draw_err=0x%x prog=%u fbo=%u\n",
            n, hk_secs(), mode, count, type, pre, post, g_cur_program, g_cur_fbo); }
  n++;
}
/* INSTANCING: sobe o shadow dos UBOs como uniforms planos + loop de draws */
static int (*real_glGetUniformLocation)(unsigned, const char *) = 0;
static void (*real_glUniform1i)(int, int) = 0;
static void (*real_glUniform4fvI)(int, int, const float *) = 0;
static void (*real_glUniform2fvI)(int, int, const float *) = 0;
static void inst_resolve(void) {
  if (!real_glGetUniformLocation) real_glGetUniformLocation = (int (*)(unsigned, const char *))dlsym(RTLD_DEFAULT, "glGetUniformLocation");
  if (!real_glUniform1i) real_glUniform1i = (void (*)(int, int))dlsym(RTLD_DEFAULT, "glUniform1i");
  if (!real_glUniform4fvI) real_glUniform4fvI = (void (*)(int, int, const float *))dlsym(RTLD_DEFAULT, "glUniform4fv");
  if (!real_glUniform2fvI) real_glUniform2fvI = (void (*)(int, int, const float *))dlsym(RTLD_DEFAULT, "glUniform2fv");
  if (!real_glDrawElements) real_glDrawElements = (void (*)(unsigned, int, unsigned, const void *))dlsym(RTLD_DEFAULT, "glDrawElements");
  if (!real_glDrawArrays) real_glDrawArrays = (void (*)(unsigned, int, int))dlsym(RTLD_DEFAULT, "glDrawArrays");
}
/* layout std140 dos blocos hlslcc do HK:
   binding 0 UnityInstancing_PerDraw0: unity_Builtins0Array[i] = ObjectToWorld(64B)+WorldToObject(64B)
   binding 1 UnityInstancing_PerDrawSprite: PerDrawSpriteArray[i] = vec4 color(16B)+vec2 flip(+pad=16B) */
static void upload_hk_instancing(void) {
  inst_resolve();
  if (!real_glGetUniformLocation || !real_glUniform4fvI) return;
  unsigned char *d0 = g_ubo_bindpt[0].buf ? ubo_shadow(g_ubo_bindpt[0].buf, 0) : 0;
  if (d0) {
    d0 += g_ubo_bindpt[0].offset;
    for (int i = 0; i < 8; i++) {
      char nm[96]; int loc;
      snprintf(nm, sizeof nm, "unity_Builtins0Array[%d].hlslcc_mtx4x4unity_ObjectToWorldArray", i);
      loc = real_glGetUniformLocation(g_cur_program, nm);
      if (loc < 0 && i > 0) break;
      if (loc >= 0) real_glUniform4fvI(loc, 4, (const float *)(d0 + (size_t)i * 128));
      snprintf(nm, sizeof nm, "unity_Builtins0Array[%d].hlslcc_mtx4x4unity_WorldToObjectArray", i);
      loc = real_glGetUniformLocation(g_cur_program, nm);
      if (loc >= 0) real_glUniform4fvI(loc, 4, (const float *)(d0 + (size_t)i * 128 + 64));
    }
  }
  unsigned char *d1 = g_ubo_bindpt[1].buf ? ubo_shadow(g_ubo_bindpt[1].buf, 0) : 0;
  if (d1) {
    d1 += g_ubo_bindpt[1].offset;
    for (int i = 0; i < 8; i++) {
      char nm[96]; int loc;
      snprintf(nm, sizeof nm, "PerDrawSpriteArray[%d].unity_SpriteRendererColorArray", i);
      loc = real_glGetUniformLocation(g_cur_program, nm);
      if (loc < 0 && i > 0) break;
      if (loc >= 0) real_glUniform4fvI(loc, 1, (const float *)(d1 + (size_t)i * 32));
      snprintf(nm, sizeof nm, "PerDrawSpriteArray[%d].unity_SpriteFlipArray", i);
      loc = real_glGetUniformLocation(g_cur_program, nm);
      if (loc >= 0 && real_glUniform2fvI) real_glUniform2fvI(loc, 1, (const float *)(d1 + (size_t)i * 32 + 16));
    }
  }
}
static void emul_glDrawElementsInstanced(unsigned mode, int count, unsigned type, const void *idx, int n) {
  inst_resolve();
  upload_hk_instancing();
  int loc = real_glGetUniformLocation ? real_glGetUniformLocation(g_cur_program, "u_hk_instID") : -1;
  static int iln = 0;
  if (iln < 12) { fprintf(stderr, "[INST] DrawElementsInstanced n=%d count=%d prog=%u instloc=%d\n", n, count, g_cur_program, loc); iln++; }
  for (int i = 0; i < n; i++) {
    if (loc >= 0 && real_glUniform1i) real_glUniform1i(loc, i);
    if (real_glDrawElements) real_glDrawElements(mode, count, type, idx);
  }
}
static void emul_glDrawArraysInstanced(unsigned mode, int first, int count, int n) {
  inst_resolve();
  upload_hk_instancing();
  int loc = real_glGetUniformLocation ? real_glGetUniformLocation(g_cur_program, "u_hk_instID") : -1;
  static int iln = 0;
  if (iln < 12) { fprintf(stderr, "[INST] DrawArraysInstanced n=%d count=%d prog=%u instloc=%d\n", n, count, g_cur_program, loc); iln++; }
  for (int i = 0; i < n; i++) {
    if (loc >= 0 && real_glUniform1i) real_glUniform1i(loc, i);
    if (real_glDrawArrays) real_glDrawArrays(mode, first, count);
  }
}
/* DIAG: as matrizes (projecao/modelview) sao setadas? (nao setadas -> gl_Position degenerado) */
static void (*real_glUniform4fv)(int, int, const float *) = 0;
static void diag_glUniform4fv(int loc, int count, const float *v) {
  if (!real_glUniform4fv) real_glUniform4fv = (void (*)(int, int, const float *))dlsym(RTLD_DEFAULT, "glUniform4fv");
  static int n = 0;
  /* periodico: 20 no inicio + rajadas de 10 a cada 8000 (matrizes no estado estavel) */
  if ((n < 20 || (n % 8000) < 10) && v) {
    if (count == 4)
      fprintf(stderr, "[U4FV] #%d t=%lds loc=%d MAT [%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]\n",
              n, hk_secs(), loc, v[0],v[1],v[2],v[3], v[4],v[5],v[6],v[7], v[8],v[9],v[10],v[11], v[12],v[13],v[14],v[15]);
    else fprintf(stderr, "[U4FV] #%d t=%lds loc=%d count=%d v=%.2f %.2f %.2f %.2f\n", n, hk_secs(), loc, count, v[0], v[1], v[2], v[3]);
  }
  n++;
  if (real_glUniform4fv) real_glUniform4fv(loc, count, v);
}
static void (*real_glUniformMatrix4fv)(int, int, unsigned char, const float *) = 0;
static void diag_glUniformMatrix4fv(int loc, int count, unsigned char tr, const float *v) {
  if (!real_glUniformMatrix4fv) real_glUniformMatrix4fv = (void (*)(int, int, unsigned char, const float *))dlsym(RTLD_DEFAULT, "glUniformMatrix4fv");
  static int n = 0;
  if (n < 12 && v) { fprintf(stderr, "[UMTX] loc=%d count=%d m0-3=%.2f %.2f %.2f %.2f\n", loc, count, v[0], v[1], v[2], v[3]); n++; }
  if (real_glUniformMatrix4fv) real_glUniformMatrix4fv(loc, count, tr, v);
}
/* DIAG: viewport/scissor pequeno = recorta os draws -> render esparso */
static void (*real_glViewport)(int, int, int, int) = 0;
static void diag_glViewport(int x, int y, int w, int h) {
  if (!real_glViewport) real_glViewport = (void (*)(int, int, int, int))dlsym(RTLD_DEFAULT, "glViewport");
  static int n = 0; if (n < 15) { fprintf(stderr, "[VIEWPORT] %d %d %d %d\n", x, y, w, h); n++; }
  if (real_glViewport) real_glViewport(x, y, w, h);
}
static void (*real_glScissor)(int, int, int, int) = 0;
static void diag_glScissor(int x, int y, int w, int h) {
  if (!real_glScissor) real_glScissor = (void (*)(int, int, int, int))dlsym(RTLD_DEFAULT, "glScissor");
  static int n = 0; if (n < 15) { fprintf(stderr, "[SCISSOR] %d %d %d %d\n", x, y, w, h); n++; }
  if (real_glScissor) real_glScissor(x, y, w, h);
}

static void *my_dlopen(const char *name, int flag) {
  fprintf(stderr, "[dlopen] \"%s\"\n", name ? name : "(self)");
  if (name && strstr(name, "libil2cpp")) return (void *)g_m_il2cpp;
  if (name && strstr(name, "libunity"))  return (void *)g_m_unity;
  /* dlopen("")/NULL = escopo GLOBAL: no Android il2cpp esta no global (RTLD_GLOBAL).
     Unity faz dlopen("") p/ achar os simbolos il2cpp_* -> devolver g_m_il2cpp. */
  if (!name || name[0] == '\0') return (void *)g_m_il2cpp;
  return dlopen(name, flag); /* real p/ libdl/libc/etc */
}
static void *my_dlsym(void *h, const char *sym) {
  /* egl* -> NOSSOS shims (Unity faz dlopen(libEGL)+dlsym; o Mali real falha com
     nossos handles fake -> eglMakeCurrent FALSE -> "[EGL] Unable to acquire" smash) */
  if (sym && sym[0] == 'e' && sym[1] == 'g' && sym[2] == 'l') {
    void *p = egl_shim_GetProcAddress(sym);
    if (p) { fprintf(stderr, "[dlsym egl] %s -> shim %p\n", sym, p); return p; }
  }
  if ((h == (void *)g_m_il2cpp || h == (void *)g_m_unity) && g_m_il2cpp) {
    so_module *save = so_save();      /* salva modulo ativo */
    so_use((so_module *)h);
    uintptr_t a = so_find_addr(sym);
    so_use(save);                     /* restaura */
    free(save);
    if (!a) fprintf(stderr, "[dlsym modulo] %s -> NULL\n", sym ? sym : "?");
    return (void *)a;
  }
  /* HK Mali-450: a Unity pega o GL via dlsym -> intercepta AQUI.
     glGetString mente "GLES 3.0" (Unity carrega shaders GLES3); glShaderSource captura. */
  if (sym) {
    if (!getenv("HK_NOSPOOF") && strcmp(sym, "glGetString") == 0)   return (void *)my_glGetString;
    if (!getenv("HK_NOSPOOF") && strcmp(sym, "glGetIntegerv") == 0) return (void *)my_glGetIntegerv;
    if (strcmp(sym, "glShaderSource") == 0)                          return (void *)my_glShaderSource;
    if (strcmp(sym, "glTexSubImage2D") == 0)                         return (void *)diag_glTexSubImage2D;
    if (strcmp(sym, "glCompressedTexImage2D") == 0)                  return (void *)diag_glCompressedTexImage2D;
    if (strcmp(sym, "glCheckFramebufferStatus") == 0)               return (void *)diag_glCheckFramebufferStatus;
    if (strcmp(sym, "glDrawElements") == 0)                          return (void *)diag_glDrawElements;
    if (strcmp(sym, "glUniform4fv") == 0)                            return (void *)diag_glUniform4fv;
    if (strcmp(sym, "glUniformMatrix4fv") == 0)                      return (void *)diag_glUniformMatrix4fv;
    if (strcmp(sym, "glViewport") == 0)                              return (void *)diag_glViewport;
    if (strcmp(sym, "glScissor") == 0)                               return (void *)diag_glScissor;
    if (strcmp(sym, "glGetAttribLocation") == 0)                     return (void *)diag_glGetAttribLocation;
    if (strcmp(sym, "glGetUniformLocation") == 0)                    return (void *)diag_glGetUniformLocation;
    if (strcmp(sym, "glVertexAttribPointer") == 0)                   return (void *)diag_glVertexAttribPointer;
    if (strcmp(sym, "glEnableVertexAttribArray") == 0)               return (void *)diag_glEnableVertexAttribArray;
    if (strcmp(sym, "glDisableVertexAttribArray") == 0)              return (void *)diag_glDisableVertexAttribArray;
    if (strcmp(sym, "glAttachShader") == 0)                          return (void *)diag_glAttachShader;
    if (strcmp(sym, "glLinkProgram") == 0)                            return (void *)diag_glLinkProgram;
    /* ANTI-WEDGE: glFinish satura o Utgard (Bully) -> no-op (religa: HK_ALLOWFINISH=1) */
    if (!getenv("HK_ALLOWFINISH") && strcmp(sym, "glFinish") == 0)   return (void *)noop_glFinish;
    if (texcap() && strcmp(sym, "glBindTexture") == 0)               return (void *)my_glBindTexture;
    /* UBO shadow + instancing (sprites): rastreia program/buffers */
    if (strcmp(sym, "glBindFramebuffer") == 0)                       return (void *)my_glBindFramebuffer;
    if (strcmp(sym, "glFramebufferTexture2D") == 0)                  return (void *)diag_glFramebufferTexture2D;
    if (strcmp(sym, "glUseProgram") == 0)                            return (void *)my_glUseProgram;
    if (strcmp(sym, "glBindBuffer") == 0)                            return (void *)my_glBindBuffer;
    if (strcmp(sym, "glBufferData") == 0)                            return (void *)my_glBufferData;
    if (strcmp(sym, "glBufferSubData") == 0)                         return (void *)my_glBufferSubData;
  }
  void *r = dlsym(h, sym);
  /* HK Mali-450: funcao GLES3 ausente -> devolve STUB (nao NULL) p/ evitar crash/loop.
     glGetInternalformativ zera params (sem MSAA, quebra o loop); demais = no-op retorna 0. */
  if (!r && sym && sym[0] == 'g' && sym[1] == 'l') {
    if (strcmp(sym, "glGetInternalformativ") == 0)   r = (void *)stub_glGetInternalformativ;
    else if (strcmp(sym, "glTexStorage2D") == 0)     r = (void *)stub_glTexStorage2D;
    else if (strcmp(sym, "glFenceSync") == 0)        r = (void *)stub_glFenceSync;
    else if (strcmp(sym, "glClientWaitSync") == 0)   r = (void *)stub_glClientWaitSync;
    else if (strcmp(sym, "glGetSynciv") == 0)        r = (void *)stub_glGetSynciv;
    else if (strcmp(sym, "glMapBufferRange") == 0)   r = (void *)emul_glMapBufferRange;
    else if (strcmp(sym, "glUnmapBuffer") == 0)      r = (void *)emul_glUnmapBuffer;
    else if (strcmp(sym, "glBindBufferRange") == 0)  r = (void *)emul_glBindBufferRange;
    else if (strcmp(sym, "glBindBufferBase") == 0)   r = (void *)emul_glBindBufferBase;
    else if (strcmp(sym, "glDrawElementsInstanced") == 0) r = (void *)emul_glDrawElementsInstanced;
    else if (strcmp(sym, "glDrawArraysInstanced") == 0)   r = (void *)emul_glDrawArraysInstanced;
    else                                             r = (void *)stub_gl_noop;
    static int n = 0;
    if (n++ < 60) fprintf(stderr, "[gl stub] %s\n", sym);
  }
  return r;
}

void *recon_gl_override(const char *sym) {
  if (!sym) return 0;
  if (!getenv("HK_NOSPOOF") && strcmp(sym, "glGetString") == 0)   return (void *)my_glGetString;
  if (!getenv("HK_NOSPOOF") && strcmp(sym, "glGetIntegerv") == 0) return (void *)my_glGetIntegerv;
  if (strcmp(sym, "glShaderSource") == 0)                         return (void *)my_glShaderSource;
  if (strcmp(sym, "glTexSubImage2D") == 0)                        return (void *)diag_glTexSubImage2D;
  if (strcmp(sym, "glCompressedTexImage2D") == 0)                 return (void *)diag_glCompressedTexImage2D;
  if (strcmp(sym, "glCheckFramebufferStatus") == 0)               return (void *)diag_glCheckFramebufferStatus;
  if (strcmp(sym, "glDrawElements") == 0)                         return (void *)diag_glDrawElements;
  if (strcmp(sym, "glUniform4fv") == 0)                           return (void *)diag_glUniform4fv;
  if (strcmp(sym, "glUniformMatrix4fv") == 0)                     return (void *)diag_glUniformMatrix4fv;
  if (strcmp(sym, "glViewport") == 0)                             return (void *)diag_glViewport;
  if (strcmp(sym, "glScissor") == 0)                              return (void *)diag_glScissor;
  if (strcmp(sym, "glGetAttribLocation") == 0)                    return (void *)diag_glGetAttribLocation;
  if (strcmp(sym, "glGetUniformLocation") == 0)                   return (void *)diag_glGetUniformLocation;
  if (strcmp(sym, "glVertexAttribPointer") == 0)                  return (void *)diag_glVertexAttribPointer;
  if (strcmp(sym, "glEnableVertexAttribArray") == 0)              return (void *)diag_glEnableVertexAttribArray;
  if (strcmp(sym, "glDisableVertexAttribArray") == 0)             return (void *)diag_glDisableVertexAttribArray;
  if (strcmp(sym, "glAttachShader") == 0)                         return (void *)diag_glAttachShader;
  if (strcmp(sym, "glLinkProgram") == 0)                           return (void *)diag_glLinkProgram;
  if (!getenv("HK_ALLOWFINISH") && strcmp(sym, "glFinish") == 0)  return (void *)noop_glFinish;
  if (texcap() && strcmp(sym, "glBindTexture") == 0)              return (void *)my_glBindTexture;
  if (strcmp(sym, "glBindFramebuffer") == 0)                      return (void *)my_glBindFramebuffer;
  if (strcmp(sym, "glFramebufferTexture2D") == 0)                 return (void *)diag_glFramebufferTexture2D;
  if (strcmp(sym, "glUseProgram") == 0)                           return (void *)my_glUseProgram;
  if (strcmp(sym, "glBindBuffer") == 0)                           return (void *)my_glBindBuffer;
  if (strcmp(sym, "glBufferData") == 0)                           return (void *)my_glBufferData;
  if (strcmp(sym, "glBufferSubData") == 0)                        return (void *)my_glBufferSubData;
  if (strcmp(sym, "glGetInternalformativ") == 0)                  return (void *)stub_glGetInternalformativ;
  if (strcmp(sym, "glTexStorage2D") == 0)                         return (void *)stub_glTexStorage2D;
  if (strcmp(sym, "glFenceSync") == 0)                            return (void *)stub_glFenceSync;
  if (strcmp(sym, "glClientWaitSync") == 0)                       return (void *)stub_glClientWaitSync;
  if (strcmp(sym, "glGetSynciv") == 0)                            return (void *)stub_glGetSynciv;
  if (strcmp(sym, "glMapBufferRange") == 0)                       return (void *)emul_glMapBufferRange;
  if (strcmp(sym, "glUnmapBuffer") == 0)                          return (void *)emul_glUnmapBuffer;
  if (strcmp(sym, "glBindBufferRange") == 0)                      return (void *)emul_glBindBufferRange;
  if (strcmp(sym, "glBindBufferBase") == 0)                       return (void *)emul_glBindBufferBase;
  if (strcmp(sym, "glDrawElementsInstanced") == 0)                return (void *)emul_glDrawElementsInstanced;
  if (strcmp(sym, "glDrawArraysInstanced") == 0)                  return (void *)emul_glDrawArraysInstanced;
  return 0;
}

/* ---- __android_log_* REAL (revela mensagens de erro do il2cpp/unity) ---- */
#include <stdarg.h>
static int my_alog_print(int prio, const char *tag, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  return 0;
}
static int my_alog_write(int prio, const char *tag, const char *text) {
  fprintf(stderr, "[ALOG:%d %s] %s\n", prio, tag ? tag : "?", text ? text : "");
  return 0;
}
static int my_alog_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  fprintf(stderr, "[ALOG:%d %s] ", prio, tag ? tag : "?");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  return 0;
}
static void my_abort_msg(const char *msg) {
  fprintf(stderr, "[ABORT_MSG] %s\n", msg ? msg : "(null)");
}

/* Intercepta __stack_chk_fail p/ revelar a funcao que estourou (return addr). */
static void my_stack_chk_fail(void) {
  void *ra = __builtin_return_address(0);
  uintptr_t r = (uintptr_t)ra;
  fprintf(stderr, "[STACK_CHK_FAIL] smash em ra=%p unity+0x%lx il2+0x%lx\n",
          ra, (unsigned long)(r - 0x540000000UL), (unsigned long)(r - 0x500000000UL));
  /* dump da pilha do frame que estourou (ASCII) -> revela a string/dado */
  unsigned char *fp = (unsigned char *)__builtin_frame_address(0);
  fprintf(stderr, "[STACK_CHK_FAIL] frame dump (ASCII):\n");
  for (int row = 0; row < 24; row++) {
    char line[80]; int p = 0;
    p += snprintf(line + p, sizeof line - p, "  +0x%03x: ", row * 32);
    for (int i = 0; i < 32; i++) {
      unsigned char c = fp[row * 32 + i];
      line[p++] = (c >= 32 && c < 127) ? c : '.';
    }
    line[p++] = '\n';
    if (write(2, line, p) < 0) {}
  }
  _exit(66);
}

/* sigaction bionic-safe. RAIZ DO CRASH: Unity (bionic) passa oldact = buffer de
   32 bytes (struct sigaction bionic). O sigaction do glibc escreve 152 bytes ->
   ESTOURA a pilha e corrompe o x30 salvo -> ret p/ lixo. Aqui escrevemos SO 32
   bytes (SIG_DFL) e nao instalamos o handler do Unity (evita mascarar crashes). */
#include <string.h>
#include <signal.h>
/* struct sigaction BIONIC (aarch64): flags@0, handler@8, mask@16(8B), restorer@24 (32B).
   GLIBC: handler@0, mask@8(128B), flags@136, restorer@144 (~152B). */
extern int sigaction(int, const struct sigaction *, struct sigaction *);
static int my_sigaction(int sig, const void *act, void *oldact) {
  fprintf(stderr, "[sigaction] sig=%d act=%p\n", sig, act);
  /* sinais de CRASH: mantemos NOSSO handler (nao deixa Unity instalar o dele) */
  if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGABRT ||
      sig == SIGFPE) {
    if (oldact) memset(oldact, 0, 32);
    return 0;
  }
  /* demais sinais (GC do il2cpp: suspensao de thread etc.): TRADUZ bionic->glibc */
  struct sigaction g; memset(&g, 0, sizeof g);
  struct sigaction og; memset(&og, 0, sizeof og);
  if (act) {
    const unsigned char *b = (const unsigned char *)act;
    unsigned int bflags = *(const unsigned int *)(b + 0);
    void *bhandler = *(void *const *)(b + 8);
    unsigned long bmask = *(const unsigned long *)(b + 16);
    g.sa_flags = (int)(bflags & ~0x04000000u); /* tira SA_RESTORER (glibc usa o seu) */
    if (bflags & 0x00000004u /*SA_SIGINFO*/) g.sa_sigaction = (void (*)(int, siginfo_t *, void *))bhandler;
    else g.sa_handler = (void (*)(int))bhandler;
    memcpy(&g.sa_mask, &bmask, sizeof bmask); /* expande 8B->128B (resto 0) */
  }
  int r = sigaction(sig, act ? &g : NULL, oldact ? &og : NULL);
  if (oldact) { /* glibc->bionic */
    unsigned char *b = (unsigned char *)oldact;
    memset(b, 0, 32);
    *(unsigned int *)(b + 0) = (unsigned int)og.sa_flags;
    *(void **)(b + 8) = (og.sa_flags & 0x00000004) ? (void *)og.sa_sigaction : (void *)og.sa_handler;
    memcpy(b + 16, &og.sa_mask, 8);
  }
  return r;
}
static void *my_signal(int sig, void *handler) {
  (void)sig; (void)handler;
  return (void *)0; /* SIG_DFL */
}

/* ---- ANativeWindow shim (Unity usa no Surface) ---- */
static void *ANW_fromSurface(void *env, void *surface) {
  (void)env; (void)surface;
  void *w = egl_shim_get_window();
  return w ? w : (void *)0x57; /* nao-nulo */
}
extern int egl_shim_width(void), egl_shim_height(void);
static int ANW_getWidth(void *w)  { (void)w; return egl_shim_width(); }
static int ANW_getHeight(void *w) { (void)w; return egl_shim_height(); }
static int ANW_setBuffersGeometry(void *w, int a, int b, int c) {
  (void)w; (void)a; (void)b; (void)c; return 0;
}
static void ANW_acquire(void *w) { (void)w; }
static void ANW_release(void *w) { (void)w; }

static void set_import(const char *name, void *fn) {
  for (size_t i = 0; i < dynlib_numfunctions; i++)
    if (strcmp(dynlib_functions[i].symbol, name) == 0) {
      dynlib_functions[i].func = (uintptr_t)fn;
      return;
    }
}

/* ---- HK Mali-450: spoof de versao GLES + captura de shaders ----
   O Mali-450 e GLES2; o blob do HK so tem shaders GLES3 (platform 5 = GLES2
   "not available in shader blob"). Mentindo GL_VERSION="OpenGL ES 3.0", a Unity
   carrega os shaders GLES3 e os manda pro glShaderSource -> capturamos e (depois)
   traduzimos GLES3->GLES2. Desliga com HK_NOSPOOF=1. */
static const unsigned char *(*real_glGetString)(unsigned) = 0;
static const unsigned char *my_glGetString(unsigned name) {
  if (!real_glGetString)
    real_glGetString = (const unsigned char *(*)(unsigned))dlsym(RTLD_DEFAULT, "glGetString");
  if (!getenv("HK_NOSPOOF")) {
    if (name == 0x1F02) return (const unsigned char *)"OpenGL ES 3.0";           /* GL_VERSION */
    if (name == 0x8B8C) return (const unsigned char *)"OpenGL ES GLSL ES 3.00";  /* GL_SHADING_LANGUAGE_VERSION */
  }
  return real_glGetString ? real_glGetString(name) : (const unsigned char *)"";
}
static void (*real_glGetIntegerv)(unsigned, int *) = 0;
static void my_glGetIntegerv(unsigned pname, int *p) {
  if (!real_glGetIntegerv)
    real_glGetIntegerv = (void (*)(unsigned, int *))dlsym(RTLD_DEFAULT, "glGetIntegerv");
  if (!getenv("HK_NOSPOOF")) {
    if (pname == 0x821B) { if (p) p[0] = 3; return; }  /* GL_MAJOR_VERSION */
    if (pname == 0x821C) { if (p) p[0] = 0; return; }  /* GL_MINOR_VERSION */
    if (pname == 0x88FF) { if (p) p[0] = hk_int_env("HK_MAX_ARRAY_LAYERS", 256); return; } /* GL_MAX_ARRAY_TEXTURE_LAYERS */
    if (pname == 0x8073) { if (p) p[0] = hk_int_env("HK_MAX_3D_TEX", 256); return; }       /* GL_MAX_3D_TEXTURE_SIZE */
  }
  if (real_glGetIntegerv) real_glGetIntegerv(pname, p);
}
static void (*real_glShaderSource)(unsigned, int, const char *const *, const int *) = 0;
static int g_shdump = 0;
/* substitui TODAS as ocorrencias de `from` por `to` -> novo buffer malloc'd */
static char *str_rep(const char *s, const char *from, const char *to) {
  size_t fl = strlen(from), tl = strlen(to), cnt = 0;
  for (const char *p = s; (p = strstr(p, from)) != NULL; p += fl) cnt++;
  char *out = (char *)malloc(strlen(s) + (tl > fl ? (tl - fl) * cnt : 0) + 1);
  char *o = out;
  for (const char *p = s; *p;) {
    if (!strncmp(p, from, fl)) { memcpy(o, to, tl); o += tl; p += fl; }
    else *o++ = *p++;
  }
  *o = 0;
  return out;
}

/* TRADUTOR: shader Unity GLES3 (#version 300 es) -> GLES2 (#version 100). Mali-450. */
static char *translate_gles3_gles2(const char *src) {
  int is_frag = (strstr(src, "gl_Position") == NULL); /* sem gl_Position = fragment */
  char *s = str_rep(src, "#version 300 es", "#version 100");
  char *t = str_rep(s, "texture(", "texture2D("); free(s); s = t;
  /* Shaders internos de blit/composite da Unity usam macros abstratas em vez de
     `in/out/texture` direto. Se elas ficam como GLES3, o shader ES2 nao linka e
     o composite final vira 1-2 pixels. */
  t = str_rep(s, "#define ATTRIBUTE_IN in", "#define ATTRIBUTE_IN attribute"); free(s); s = t;
  t = str_rep(s, "#define VARYING_IN in", "#define VARYING_IN varying"); free(s); s = t;
  t = str_rep(s, "#define VARYING_OUT out", "#define VARYING_OUT varying"); free(s); s = t;
  t = str_rep(s, "#define DECLARE_FRAG_COLOR out vec4 fragColor", "/* DECLARE_FRAG_COLOR removed for GLES2 */"); free(s); s = t;
  t = str_rep(s, "#define FRAG_COLOR fragColor", "#define FRAG_COLOR gl_FragColor"); free(s); s = t;
  t = str_rep(s, "#define SAMPLE_TEXTURE_2D texture", "#define SAMPLE_TEXTURE_2D texture2D"); free(s); s = t;
  /* INSTANCING (sprites do HK): o boilerplate hlslcc tem #if HLSLCC_ENABLE_UNIFORM_BUFFERS
     em volta dos blocos UBO -> flag 0 = membros viram uniforms PLANOS (struct array, ES2 ok).
     gl_InstanceID nao existe em ES2 -> uniform u_hk_instID setado pelo emul_DrawInstanced.
     Shifts << sao proibidos em GLSL ES 100 -> multiplicacao. */
  if (strstr(s, "HLSLCC_ENABLE_UNIFORM_BUFFERS 1")) {
    t = str_rep(s, "#define HLSLCC_ENABLE_UNIFORM_BUFFERS 1",
                   "#define HLSLCC_ENABLE_UNIFORM_BUFFERS 0"); free(s); s = t;
  }
  if (strstr(s, "gl_InstanceID")) {
    t = str_rep(s, "gl_InstanceID", "u_hk_instID"); free(s); s = t;
    t = str_rep(s, "#version 100", "#version 100\nuniform int u_hk_instID;"); free(s); s = t;
  }
  t = str_rep(s, "<< int(1)", "* 2"); free(s); s = t;
  t = str_rep(s, "<< int(2)", "* 4"); free(s); s = t;
  t = str_rep(s, "<< int(3)", "* 8"); free(s); s = t;
  t = str_rep(s, "<< int(4)", "* 16"); free(s); s = t;
  if (is_frag) {
    /* GLES2 nao tem `out` de fragment: SV_Target0 -> gl_FragColor (built-in) */
    t = str_rep(s, "layout(location = 0) out mediump vec4 SV_Target0;", ""); free(s); s = t;
    t = str_rep(s, "layout(location = 0) out highp vec4 SV_Target0;", ""); free(s); s = t;
    t = str_rep(s, "SV_Target0", "gl_FragColor"); free(s); s = t;
    t = str_rep(s, "\nin ", "\nvarying "); free(s); s = t; /* entradas de fragment */
    if (getenv("HK_FORCERED")) { /* DEBUG: forca saida vermelha -> testa geometria vs textura */
      t = str_rep(s, "return;", "gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); return;"); free(s); s = t;
    } else if (getenv("HK_FORCETEX") && strstr(s, "_MainTex") && strstr(s, "vs_TEXCOORD0")) {
      /* DEBUG: saida = textura crua -> testa se a textura subiu (preta=upload falhou) */
      t = str_rep(s, "return;", "gl_FragColor = texture2D(_MainTex, vs_TEXCOORD0.xy); return;"); free(s); s = t;
    } else if (getenv("HK_FORCECOL") && strstr(s, "vs_COLOR0")) {
      /* DEBUG: saida = cor do vertice -> testa se o atributo COLOR chega (preto=atributo zerado) */
      t = str_rep(s, "return;", "gl_FragColor = vec4(vs_COLOR0.xyz, 1.0); return;"); free(s); s = t;
    }
  } else {
    t = str_rep(s, "\nin ", "\nattribute "); free(s); s = t;  /* entradas de vertex */
    t = str_rep(s, "\nout ", "\nvarying "); free(s); s = t;   /* saidas de vertex */
    if (getenv("HK_VTXRAW")) { /* DEBUG: bypassa as matrizes -> gl_Position = posicao crua */
      t = str_rep(s, "return;", "gl_Position = in_POSITION0; return;"); free(s); s = t;
    } else if (getenv("HK_VTXZOOM")) { /* DEBUG: zoom-out 2x -> conteudo "logo fora da tela" aparece */
      t = str_rep(s, "return;", "gl_Position.xy *= 0.5; return;"); free(s); s = t;
    }
  }
  return s;
}

static void my_glShaderSource(unsigned sh, int count, const char *const *str,
                              const int *len) {
  if (!real_glShaderSource)
    real_glShaderSource = (void (*)(unsigned, int, const char *const *,
                                    const int *))dlsym(RTLD_DEFAULT, "glShaderSource");
  /* concatena os pedacos num buffer unico */
  size_t total = 0;
  for (int i = 0; i < count; i++)
    if (str[i]) total += (len && len[i] > 0) ? (size_t)len[i] : strlen(str[i]);
  char *src = (char *)malloc(total + 1);
  size_t o = 0;
  for (int i = 0; i < count; i++)
    if (str[i]) {
      size_t l = (len && len[i] > 0) ? (size_t)len[i] : strlen(str[i]);
      memcpy(src + o, str[i], l); o += l;
    }
  src[o] = 0;
  char *tr = translate_gles3_gles2(src);
  if (g_shdump < 80) {
    char p[160]; int id = g_shdump++;
    shmap_set(sh, id, strstr(src, "gl_Position") == NULL);
    snprintf(p, sizeof p, "/storage/hollow-recon/shdump/sh_%03d.glsl", id);
    FILE *f = fopen(p, "w"); if (f) { fputs(src, f); fclose(f); }
    snprintf(p, sizeof p, "/storage/hollow-recon/shdump/sh_%03d.gles2", id);
    f = fopen(p, "w"); if (f) { fputs(tr, f); fclose(f); }
  }
  if (real_glShaderSource) {
    const char *one = tr; int onelen = (int)strlen(tr);
    real_glShaderSource(sh, 1, &one, &onelen);
  }
  free(src); free(tr);
}

void recon_wire_egl(void) {
  set_import("eglGetDisplay", (void *)egl_shim_GetDisplay);
  set_import("eglInitialize", (void *)egl_shim_Initialize);
  set_import("eglTerminate", (void *)egl_shim_Terminate);
  set_import("eglChooseConfig", (void *)egl_shim_ChooseConfig);
  set_import("eglCreateWindowSurface", (void *)egl_shim_CreateWindowSurface);
  set_import("eglCreatePbufferSurface", (void *)egl_shim_CreatePbufferSurface);
  set_import("eglCreateContext", (void *)egl_shim_CreateContext);
  set_import("eglMakeCurrent", (void *)egl_shim_MakeCurrent);
  set_import("eglSwapBuffers", (void *)egl_shim_SwapBuffers);
  set_import("eglDestroySurface", (void *)egl_shim_DestroySurface);
  set_import("eglDestroyContext", (void *)egl_shim_DestroyContext);
  set_import("eglQuerySurface", (void *)egl_shim_QuerySurface);
  set_import("eglGetConfigAttrib", (void *)egl_shim_GetConfigAttrib);
  set_import("eglGetError", (void *)egl_shim_GetError);
  set_import("eglGetProcAddress", (void *)egl_shim_GetProcAddress);
  set_import("eglQueryString", (void *)egl_shim_QueryString);
  set_import("eglSwapInterval", (void *)egl_shim_SwapInterval);
  set_import("eglGetCurrentContext", (void *)egl_shim_GetCurrentContext);
  set_import("eglGetCurrentSurface", (void *)egl_shim_GetCurrentSurface);
  set_import("eglSurfaceAttrib", (void *)egl_shim_SurfaceAttrib);
  /* ANativeWindow */
  set_import("ANativeWindow_fromSurface", (void *)ANW_fromSurface);
  set_import("ANativeWindow_getWidth", (void *)ANW_getWidth);
  set_import("ANativeWindow_getHeight", (void *)ANW_getHeight);
  set_import("ANativeWindow_setBuffersGeometry", (void *)ANW_setBuffersGeometry);
  set_import("ANativeWindow_acquire", (void *)ANW_acquire);
  set_import("ANativeWindow_release", (void *)ANW_release);
  /* __android_log_* REAL — revela erros do il2cpp_init */
  set_import("__android_log_print", (void *)my_alog_print);
  set_import("__android_log_write", (void *)my_alog_write);
  set_import("__android_log_vprint", (void *)my_alog_vprint);
  set_import("android_set_abort_message", (void *)my_abort_msg);
  /* bloqueia o crash handler do Unity (revela o crash real no nosso on_segv) */
  set_import("sigaction", (void *)my_sigaction);
  set_import("signal", (void *)my_signal);
  set_import("__stack_chk_fail", (void *)my_stack_chk_fail);
  /* dlopen/dlsym logados (ver libil2cpp carregar) */
  set_import("dlopen", (void *)my_dlopen);
  set_import("dlsym", (void *)my_dlsym);
  /* HK Mali-450: mente versao GLES3 (Unity carrega shaders GLES3) + captura shaders */
  set_import("glGetString", (void *)my_glGetString);
  set_import("glGetIntegerv", (void *)my_glGetIntegerv);
  set_import("glShaderSource", (void *)my_glShaderSource);
  /* shim pthread bionic<->glibc (SIGBUS no glibc NOVO do Amlogic-no).
     Amlogic-old (.87, Mali-450) tem glibc antigo -> NAO precisa e QUEBRA o GL setup.
     So liga com HK_PTHREAD_SHIM=1 (Amlogic-no). */
  if (getenv("HK_PTHREAD_SHIM")) {
    void recon_wire_pthread(void (*)(const char *, void *));
    recon_wire_pthread(set_import);
  }
  fprintf(stderr, "[egl] wired: 20 EGL + 6 ANativeWindow + dlopen/dlsym -> egl_shim\n");
  /* DEBUG: confirmar func de eglMakeCurrent vs eglCreateWindowSurface na tabela */
  for (size_t i = 0; i < dynlib_numfunctions; i++) {
    if (!strcmp(dynlib_functions[i].symbol, "eglMakeCurrent") ||
        !strcmp(dynlib_functions[i].symbol, "eglCreateWindowSurface"))
      fprintf(stderr, "[egl-dbg] %s -> %p (shim_MC=%p shim_WS=%p)\n",
              dynlib_functions[i].symbol, (void *)dynlib_functions[i].func,
              (void *)egl_shim_MakeCurrent, (void *)egl_shim_CreateWindowSurface);
  }
}
