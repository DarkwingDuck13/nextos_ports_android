#ifndef TEXT_RENDER_H
#define TEXT_RENDER_H

/* Renderizador de texto do sistema (substitui Android Paint/Canvas do plandroid).
 * Fonte: Roboto (equivalente ao sans padrao do Android). */

/* metricas em px para 'text' no tamanho 'size' (px). */
int som_text_width(const char *utf8, int size);
int som_text_height(int size);

/* desenha 'text' (cor r,g,b, alpha = cobertura do glifo) no buffer ARGB
 * 'buf' de dimensoes bufW x bufH, na posicao (dstX,dstY).
 * Formato de cada int: 0xAARRGGBB (igual Android getPixels ARGB_8888). */
void som_text_draw(const char *utf8, int size, unsigned int *buf, int bufW,
                   int bufH, int dstX, int dstY, int r, int g, int b);

#endif
