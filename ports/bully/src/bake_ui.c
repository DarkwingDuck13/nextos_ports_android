/* bake_ui.c -- tela de SETUP (extração do APK + conversão ETC1) PROPRIA, GLES2, sem
 * love2d. Renderizador de texto dinamico via atlas de glifos (tools/fontatlas.rgba +
 * fontadv.bin): mostra mensagens do que esta fazendo + barra + contador, em PT/EN/RU,
 * adaptando a QUALQUER resolucao (pega do viewport) e display (fbdev/kmsdrm). Funcoes GL
 * via dlsym (libGLESv2 do device vem pre-carregada GLOBAL). */
#define _GNU_SOURCE
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int bully_bake_active = 0, bully_bake_cur = 0, bully_bake_total = 0;
extern char bully_cur_tex_path[256];

/* ---- GL via dlsym ---- */
static void (*p_GenTextures)(GLsizei, GLuint*);
static void (*p_BindTexture)(GLenum, GLuint);
static void (*p_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
static void (*p_TexParameteri)(GLenum, GLenum, GLint);
static GLuint (*p_CreateShader)(GLenum);
static void (*p_ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
static void (*p_CompileShader)(GLuint);
static GLuint (*p_CreateProgram)(void);
static void (*p_AttachShader)(GLuint, GLuint);
static void (*p_BindAttribLocation)(GLuint, GLuint, const GLchar*);
static void (*p_LinkProgram)(GLuint);
static void (*p_GenBuffers)(GLsizei, GLuint*);
static void (*p_BindBuffer)(GLenum, GLuint);
static void (*p_BufferData)(GLenum, GLsizeiptr, const void*, GLenum);
static void (*p_Scissor)(GLint, GLint, GLsizei, GLsizei);
static void (*p_ClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
static void (*p_Clear)(GLbitfield);
static void (*p_GetIntegerv)(GLenum, GLint*);
static void (*p_BindFramebuffer)(GLenum, GLuint);
static void (*p_Viewport)(GLint, GLint, GLsizei, GLsizei);
static void (*p_Disable)(GLenum);
static void (*p_Enable)(GLenum);
static void (*p_BlendFunc)(GLenum, GLenum);
static void (*p_UseProgram)(GLuint);
static void (*p_ActiveTexture)(GLenum);
static void (*p_Uniform1i)(GLint, GLint);
static void (*p_Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
static GLint (*p_GetUniformLocation)(GLuint, const GLchar*);
static void (*p_EnableVertexAttribArray)(GLuint);
static void (*p_VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
static void (*p_DrawArrays)(GLenum, GLint, GLsizei);
static void (*p_DisableVertexAttribArray)(GLuint);

static int gl_resolve(void) {
  #define R(f, s) p_##f = (void*)dlsym(RTLD_DEFAULT, s); if (!p_##f) return 0;
  R(GenTextures,"glGenTextures") R(BindTexture,"glBindTexture") R(TexImage2D,"glTexImage2D")
  R(TexParameteri,"glTexParameteri") R(CreateShader,"glCreateShader") R(ShaderSource,"glShaderSource")
  R(CompileShader,"glCompileShader") R(CreateProgram,"glCreateProgram") R(AttachShader,"glAttachShader")
  R(BindAttribLocation,"glBindAttribLocation") R(LinkProgram,"glLinkProgram") R(GenBuffers,"glGenBuffers")
  R(BindBuffer,"glBindBuffer") R(BufferData,"glBufferData") R(Scissor,"glScissor")
  R(ClearColor,"glClearColor") R(Clear,"glClear") R(GetIntegerv,"glGetIntegerv")
  R(BindFramebuffer,"glBindFramebuffer") R(Viewport,"glViewport") R(Disable,"glDisable")
  R(Enable,"glEnable") R(BlendFunc,"glBlendFunc") R(UseProgram,"glUseProgram") R(ActiveTexture,"glActiveTexture")
  R(Uniform1i,"glUniform1i") R(Uniform4f,"glUniform4f") R(GetUniformLocation,"glGetUniformLocation")
  R(EnableVertexAttribArray,"glEnableVertexAttribArray") R(VertexAttribPointer,"glVertexAttribPointer")
  R(DrawArrays,"glDrawArrays") R(DisableVertexAttribArray,"glDisableVertexAttribArray")
  #undef R
  return 1;
}

/* atlas: grid 16x18, celula 40x56 */
#define CELL_W 40
#define CELL_H 56
#define COLS 16
#define ROWS 18
static int s_init = -1, s_glok = 0;
static GLuint s_prog = 0, s_atlas = 0, s_vbo = 0;
static GLint s_uTex = -1, s_uColor = -1;
static unsigned char s_adv[288];
static int s_aw = COLS * CELL_W, s_ah = ROWS * CELL_H;

static GLuint be_sh(GLenum t, const char *s) { GLuint x=p_CreateShader(t); p_ShaderSource(x,1,&s,NULL); p_CompileShader(x); return x; }

static int ui_init(void) {
  if (!gl_resolve()) return 0;
  s_glok = 1;
  /* atlas */
  FILE *f = fopen("tools/fontatlas.rgba","rb"); unsigned char *img=NULL; long n=0;
  if (f){ fseek(f,0,SEEK_END); n=ftell(f); fseek(f,0,SEEK_SET); img=malloc(n>0?n:1); if(img&&fread(img,1,n,f)!=(size_t)n){free(img);img=NULL;} fclose(f);}
  if (!img || n < (long)s_aw*s_ah*4){ if(img)free(img); return 0; }
  p_GenTextures(1,&s_atlas); p_BindTexture(GL_TEXTURE_2D,s_atlas);
  p_TexImage2D(GL_TEXTURE_2D,0,GL_RGBA,s_aw,s_ah,0,GL_RGBA,GL_UNSIGNED_BYTE,img);
  p_TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
  p_TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  p_TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  p_TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  free(img);
  FILE *af=fopen("tools/fontadv.bin","rb"); if(af){ if(fread(s_adv,1,288,af)!=288){} fclose(af);} else { for(int i=0;i<288;i++) s_adv[i]=CELL_W/2; }
  const char *vs="attribute vec2 aPos;attribute vec2 aUV;varying vec2 vUV;void main(){vUV=aUV;gl_Position=vec4(aPos,0.0,1.0);}";
  /* uColor.rgb = cor; usa o ALPHA do atlas (glifo) * uColor.a -> texto colorido com AA */
  const char *fs="precision mediump float;varying vec2 vUV;uniform sampler2D uTex;uniform vec4 uColor;"
                 "void main(){float a=texture2D(uTex,vUV).a;gl_FragColor=vec4(uColor.rgb,uColor.a*a);}";
  GLuint v=be_sh(GL_VERTEX_SHADER,vs), fr=be_sh(GL_FRAGMENT_SHADER,fs);
  s_prog=p_CreateProgram(); p_AttachShader(s_prog,v); p_AttachShader(s_prog,fr);
  p_BindAttribLocation(s_prog,0,"aPos"); p_BindAttribLocation(s_prog,1,"aUV"); p_LinkProgram(s_prog);
  s_uTex=p_GetUniformLocation(s_prog,"uTex"); s_uColor=p_GetUniformLocation(s_prog,"uColor");
  p_GenBuffers(1,&s_vbo);
  return 1;
}

static int s_W=1280, s_H=720;
static int gidx(unsigned cp){ if(cp>=32&&cp<=255)return cp-32; if(cp>=0x410&&cp<=0x44F)return 224+(cp-0x410); return 0; }
/* decodifica 1 codepoint UTF-8, avanca *p */
static unsigned utf8(const char **p){ const unsigned char *s=(const unsigned char*)*p; unsigned c=*s;
  if(c<0x80){*p+=1;return c;}
  if((c>>5)==6&&s[1]){*p+=2;return ((c&0x1F)<<6)|(s[1]&0x3F);}
  if((c>>4)==14&&s[1]&&s[2]){*p+=3;return ((c&0x0F)<<12)|((s[1]&0x3F)<<6)|(s[2]&0x3F);}
  *p+=1;return c; }
/* desenha quad NDC com UV (via VBO dinamico) */
static void quad(float x0,float y0,float x1,float y1,float u0,float v0,float u1,float v1){
  /* y0=topo da tela (ndc maior) -> v0=topo do atlas; y1=base -> v1. (sem flip) */
  float q[]={x0,y0,u0,v0, x1,y0,u1,v0, x0,y1,u0,v1, x1,y1,u1,v1};
  p_BindBuffer(GL_ARRAY_BUFFER,s_vbo); p_BufferData(GL_ARRAY_BUFFER,sizeof(q),q,GL_STREAM_DRAW);
  p_EnableVertexAttribArray(0); p_VertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,16,(void*)0);
  p_EnableVertexAttribArray(1); p_VertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,16,(void*)8);
  p_DrawArrays(GL_TRIANGLE_STRIP,0,4);
}
/* largura do texto em px (p/ centralizar) */
static int text_w(const char *s, float scale){ int w=0; const char *p=s; while(*p){ unsigned c=utf8(&p); w+=(int)(s_adv[gidx(c)]*scale);} return w; }
/* desenha texto; (px,py)=canto sup-esq em pixels; size=altura em px; cor rgba */
static void text(const char *s, int px, int py, float size, float r,float g,float b,float a){
  float scale=size/CELL_H; p_Uniform4f(s_uColor,r,g,b,a);
  const char *p=s;
  while(*p){ unsigned c=utf8(&p); int i=gidx(c);
    float gw=CELL_W*scale, gh=CELL_H*scale;
    float x0=(float)px/s_W*2-1, x1=(float)(px+gw)/s_W*2-1;
    float y0=1-(float)py/s_H*2, y1=1-(float)(py+gh)/s_H*2;
    int gc=i%COLS, gr=i/COLS;   /* col,row no atlas */
    float u0=(gc*CELL_W+0.5f)/s_aw, u1=((gc+1)*CELL_W-0.5f)/s_aw;  /* inset 0.5px anti-bleed */
    float v0=(gr*CELL_H+0.5f)/s_ah, v1=((gr+1)*CELL_H-0.5f)/s_ah;
    quad(x0,y0,x1,y1,u0,v0,u1,v1);
    px += (int)(s_adv[i]*scale);
  }
}
static void text_c(const char *s, int cy, float size, float r,float g,float b){ text(s,(s_W-text_w(s,size/CELL_H))/2,cy,size,r,g,b,1.0f); }
static void rect(int x,int y,int w,int h,float r,float g,float b){ if(w<=0||h<=0)return; p_Scissor(x,s_H-y-h,w,h); p_ClearColor(r,g,b,1.0f); p_Clear(GL_COLOR_BUFFER_BIT); }

/* phase: 0=convert 1=extract 2=noapk. status=mensagem dinamica (pode ser NULL). */
void bully_setup_draw(int phase, const char *status, int cur, int total) {
  if (s_init < 0) s_init = ui_init();
  if (!s_glok) return;
  int vp[4]={0,0,0,0}; p_GetIntegerv(GL_VIEWPORT,vp);
  s_W = vp[2]>0?vp[2]:1280; s_H = vp[3]>0?vp[3]:720;
  p_BindFramebuffer(GL_FRAMEBUFFER,0); p_Viewport(0,0,s_W,s_H);
  p_Disable(GL_SCISSOR_TEST); p_Disable(GL_DEPTH_TEST); p_Disable(GL_CULL_FACE);
  p_ClearColor(0.055f,0.063f,0.102f,1.0f); p_Clear(GL_COLOR_BUFFER_BIT);
  if (s_init < 1) return;  /* sem atlas -> so fundo (nao deveria) */
  float U = s_H/720.0f;    /* escala relativa a 720p */
  /* barra (scissor, antes do texto) -- fase 2 (sem APK) e 3 (carregando jogo) = sem barra */
  if (phase != 2 && phase != 3) {
    int bw=s_W*60/100, bh=(int)(26*U), bx=(s_W-bw)/2, by=(int)(470*U);
    p_Enable(GL_SCISSOR_TEST);
    rect(bx-3,by-3,bw+6,bh+6, 0.30f,0.32f,0.40f);
    rect(bx,by,bw,bh, 0.10f,0.11f,0.16f);
    rect(bx,by,(int)((long)bw*cur/(total>0?total:1)),bh, 0.20f,0.78f,0.35f);
    p_Disable(GL_SCISSOR_TEST);
  }
  /* texto (shader + blend) */
  p_UseProgram(s_prog); p_ActiveTexture(GL_TEXTURE0); p_BindTexture(GL_TEXTURE_2D,s_atlas); p_Uniform1i(s_uTex,0);
  p_Enable(GL_BLEND); p_BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  text_c("Bully: Anniversary Edition", (int)(60*U), 44*U, 1.0f,0.78f,0.31f);
  const char *l1,*l2,*l3,*l4;
  if (phase==1){ l1="Extraindo do APK - AGUARDE"; l2="Extracting from APK - PLEASE WAIT"; l3="\xD0\x98\xD0\xB7\xD0\xB2\xD0\xBB\xD0\xB5\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5 - \xD0\x9F\xD0\x9E\xD0\x94\xD0\x9E\xD0\x96\xD0\x94\xD0\x98\xD0\xA2\xD0\x95"; l4="Preparando o jogo (1a vez) - Preparing - \xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBe\xD0\xB6\xD0\xB4\xD0\xB8\xD1\x82\xD0\xB5"; }
  else if (phase==2){ l1="Coloque o APK do Bully em ports/bully/"; l2="Put your Bully APK in ports/bully/"; l3="\xD0\x9F\xD0\xBE\xD0\xBC\xD0\xB5\xD1\x81\xD1\x82\xD0\xB8\xD1\x82\xD0\xB5 APK Bully \xD0\xB2 ports/bully/"; l4="Bully_1.4.311 (lib + assets/data_0-4.zip)"; }
  else if (phase==3){ l1="Carregando o jogo - AGUARDE"; l2="Loading the game - PLEASE WAIT"; l3="\xD0\x97\xD0\xB0\xD0\xB3\xD1\x80\xD1\x83\xD0\xB7\xD0\xBA\xD0\xB0 \xD0\xB8\xD0\xB3\xD1\x80\xD1\x8B - \xD0\x9F\xD0\x9E\xD0\x94\xD0\x9E\xD0\x96\xD0\x94\xD0\x98\xD0\xA2\xD0\x95"; l4="A 1a abertura demora mais - First launch is slower"; }
  else { l1="Convertendo texturas - AGUARDE"; l2="Converting textures - PLEASE WAIT"; l3="\xD0\x9A\xD0\xBE\xD0\xBD\xD0\xB2\xD0\xB5\xD1\x80\xD1\x82\xD0\xB0\xD1\x86\xD0\xB8\xD1\x8F - \xD0\x9F\xD0\x9E\xD0\x94\xD0\x9E\xD0\x96\xD0\x94\xD0\x98\xD0\xA2\xD0\x95"; l4="Isto acontece so na 1a vez - This only happens once"; }
  text_c(l1, (int)(190*U), 46*U, 0.92f,0.93f,0.96f);
  text_c(l2, (int)(255*U), 36*U, 0.59f,0.63f,0.71f);
  text_c(l3, (int)(312*U), 36*U, 0.59f,0.63f,0.71f);
  text_c(l4, (int)(390*U), 26*U, 0.55f,0.60f,0.70f);
  if (phase != 2) {
    char buf[160];
    if (status && status[0]) snprintf(buf,sizeof(buf),"%s", status);
    else if (total>0) snprintf(buf,sizeof(buf),"%d / %d  (%d%%)", cur, total, (int)((long)cur*100/(total>0?total:1)));
    else buf[0]='\0';
    if (buf[0]) text_c(buf, (int)(515*U), 28*U, 0.86f,0.89f,0.96f);
  }
  p_Disable(GL_BLEND); p_UseProgram(0);
}

/* chamado do hook de swap durante o BAKE (fase convert): mostra textura atual + contador. */
void bully_bake_ui(int cur, int total) {
  char st[160]; const char *bn = bully_cur_tex_path[0] ? (strrchr(bully_cur_tex_path,'/')?strrchr(bully_cur_tex_path,'/')+1:bully_cur_tex_path) : "";
  snprintf(st, sizeof(st), "%d / %d  (%d%%)   %s", cur, total, total>0?(int)((long)cur*100/total):0, bn);
  bully_setup_draw(0, st, cur, total);
}
