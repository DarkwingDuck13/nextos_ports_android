/* atc.h -- decode de texturas ATC (Adreno) para RGB565/RGBA8888 no upload */
#ifndef ATC_H
#define ATC_H

#include <GLES/gl.h>

/* retorna 1 se ifmt era ATC (imagem decodificada e subida via glTexImage2D) */
int atc_upload(GLenum target, GLint level, GLenum ifmt,
               GLsizei w, GLsizei h, GLsizei size, const void *data);

#endif
