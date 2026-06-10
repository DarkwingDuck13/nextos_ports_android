/* android_ndk_shims.c — stubs das funcs Android NDK (F1: passar do init_array).
 * ANativeWindow* vira janela Mali real na F2; AAsset* vira fopen(data1) na F3.
 * Por ora: ASensor/ALooper = no-op; ANativeWindow = stub; AAsset = NULL. Exportados (-rdynamic). */
#include <stdio.h>
#include <stdint.h>

/* ---- ASensorManager / ASensor (sem sensores) ---- */
void *ASensorManager_getInstance(void){ return (void*)1; }
void *ASensorManager_getSensorList(void *m, void ***list){ (void)m; if(list)*list=0; return 0; }
void *ASensorManager_createEventQueue(void *m, void *looper, int id, void *cb, void *data){ (void)m;(void)looper;(void)id;(void)cb;(void)data; return (void*)1; }
int   ASensorManager_destroyEventQueue(void *m, void *q){ (void)m;(void)q; return 0; }
const char *ASensor_getName(void *s){ (void)s; return "none"; }
int   ASensor_getType(void *s){ (void)s; return 0; }
int   ASensor_getMinDelay(void *s){ (void)s; return 0; }
int   ASensorEventQueue_enableSensor(void *q, void *s){ (void)q;(void)s; return -1; }
int   ASensorEventQueue_disableSensor(void *q, void *s){ (void)q;(void)s; return 0; }
int   ASensorEventQueue_setEventRate(void *q, void *s, int us){ (void)q;(void)s;(void)us; return 0; }
int   ASensorEventQueue_getEvents(void *q, void *ev, size_t n){ (void)q;(void)ev;(void)n; return 0; }

/* ---- ALooper (não bloqueia) ---- */
void *ALooper_prepare(int opts){ (void)opts; return (void*)1; }
int   ALooper_pollOnce(int timeoutMs, int *outFd, int *outEvents, void **outData){ (void)timeoutMs;(void)outFd;(void)outEvents;(void)outData; return -3; /* ALOOPER_POLL_TIMEOUT */ }
void  ALooper_wake(void *l){ (void)l; }

/* ---- ANativeWindow (F2): a "janela nativa" passada ao eglCreateWindowSurface
 * do Mali fbdev (Utgard) é um fbdev_window {u16 width; u16 height;}. Retornamos
 * um ponteiro p/ essa struct; o blob Mali lê w/h dela. ---- */
typedef struct { unsigned short width, height; } fbdev_window;
static fbdev_window g_fbwin = { 1280, 720 };
void *ANativeWindow_fromSurface(void *env, void *surface){ (void)env; (void)surface; return &g_fbwin; }
int   ANativeWindow_setBuffersGeometry(void *w, int width, int height, int format){
  (void)format; if(w && width>0 && height>0){ ((fbdev_window*)w)->width=(unsigned short)width; ((fbdev_window*)w)->height=(unsigned short)height; } return 0; }
int   ANativeWindow_getWidth(void *w){ return w?((fbdev_window*)w)->width:1280; }
int   ANativeWindow_getHeight(void *w){ return w?((fbdev_window*)w)->height:720; }
int   ANativeWindow_getFormat(void *w){ (void)w; return 1; /* RGBA_8888 */ }
void  ANativeWindow_release(void *w){ (void)w; }

/* ---- AAssetManager / AAsset (F3): lê arquivos REAIS sob asset-root.
 * APK assets/ extraído p/ $DUSK_ASSETS (default abaixo). Engine pede caminhos
 * tipo "res/gamecontrollerdb.txt" e "data1" (relativos a assets/). ---- */
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static const char *asset_root(void){
  const char *e = getenv("DUSK_ASSETS");
  return e ? e : "/storage/roms/dusklight-recon/assets";
}

typedef struct { FILE *fp; long size; } FakeAsset;
typedef struct { DIR *dir; char ret[256]; } FakeAssetDir;

void *AAssetManager_fromJava(void *env, void *obj){ (void)env;(void)obj; return (void*)1; }

void *AAssetManager_open(void *mgr, const char *fn, int mode){
  (void)mgr;(void)mode;
  if(!fn) return 0;
  char path[1024];
  snprintf(path,sizeof path,"%s/%s",asset_root(),fn);
  FILE *fp = fopen(path,"rb");
  if(!fp){ fprintf(stderr,"[AAsset] open %s -> MISS (%s)\n",fn,path); return 0; }
  FakeAsset *a = (FakeAsset*)malloc(sizeof *a);
  a->fp = fp;
  fseek(fp,0,SEEK_END); a->size = ftell(fp); fseek(fp,0,SEEK_SET);
  fprintf(stderr,"[AAsset] open %s -> OK (%ld bytes)\n",fn,a->size);
  return a;
}
int   AAsset_read(void *a, void *buf, size_t count){
  if(!a) return -1;
  size_t n = fread(buf,1,count,((FakeAsset*)a)->fp);
  return (int)n;
}
long  AAsset_getLength64(void *a){ return a ? ((FakeAsset*)a)->size : 0; }
long  AAsset_seek64(void *a, long off, int whence){
  if(!a) return -1;
  FILE *fp = ((FakeAsset*)a)->fp;
  if(fseek(fp,off,whence)!=0) return -1;
  return ftell(fp);
}
void  AAsset_close(void *a){ if(a){ fclose(((FakeAsset*)a)->fp); free(a); } }

void *AAssetManager_openDir(void *mgr, const char *dir){
  (void)mgr;
  char path[1024];
  snprintf(path,sizeof path,"%s/%s",asset_root(),dir?dir:"");
  DIR *d = opendir(path);
  if(!d) return 0;
  FakeAssetDir *fd = (FakeAssetDir*)malloc(sizeof *fd);
  fd->dir = d; fd->ret[0]=0;
  return fd;
}
const char *AAssetDir_getNextFileName(void *d){
  if(!d) return 0;
  FakeAssetDir *fd = (FakeAssetDir*)d;
  struct dirent *de;
  while((de = readdir(fd->dir))){
    if(de->d_name[0]=='.') continue;          /* pula . .. ocultos */
    if(de->d_type==DT_DIR) continue;          /* AAsset só lista arquivos */
    strncpy(fd->ret,de->d_name,sizeof fd->ret-1);
    fd->ret[sizeof fd->ret-1]=0;
    return fd->ret;
  }
  return 0;
}
void  AAssetDir_close(void *d){ if(d){ closedir(((FakeAssetDir*)d)->dir); free(d); } }
