/* fmv.h -- decodificador de FMV (VP8 via libvpx, container IVF) p/ driblar o
 * samplerExternalOES (que o Mali fbdev nao alimenta). main.c sobe o RGBA numa
 * textura 2D normal e forca enableOES=0 -> o engine renderiza o filme pelo
 * proprio pipeline (sampler2D). */
#ifndef FF7_FMV_H
#define FF7_FMV_H
#include <stdint.h>

/* Seta o filme atual a partir do path .webm aberto (my_fopen detecta). So'
 * guarda o nome; a abertura do .ivf e' lazy (no 1o frame pedido). */
void fmv_set_movie_from_webm(const char *webm_path);
/* Seta o filme a partir do nome passado ao AVI_open do engine (hook em main.c). */
void fmv_set_movie_by_name(const char *name);

/* Garante o .ivf aberto p/ o filme atual + decodifica o PROXIMO frame -> RGBA.
 * Retorna 1 se ha' um frame novo pronto (fmv_rgba valido), 0 se sem frame/fim. */
int fmv_next_frame(void);

const uint8_t *fmv_rgba(void); /* buffer RGBA w*h*4 do ultimo frame */
int fmv_w(void);
int fmv_h(void);
int fmv_has_movie(void);
int fmv_eof(void);                        /* o filme corrente chegou ao fim */
const char *fmv_current_name(void);       /* ha' um filme corrente setado/aberto */
void fmv_reset(void);          /* fecha o filme corrente (fim do FMV) */

#endif
