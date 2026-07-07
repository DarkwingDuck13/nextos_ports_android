/* dxt.h -- decode de texturas DXT/S3TC para RGB565/RGBA8888 no upload */
#ifndef DXT_H
#define DXT_H

#include <GLES2/gl2.h>

/* retorna 1 se ifmt era DXT (imagem decodificada e subida via glTexImage2D) */
int dxt_upload(GLenum target, GLint level, GLenum ifmt,
               GLsizei w, GLsizei h, GLsizei size, const void *data);

#endif
