// audioout.c — saida de audio CUSTOM (hibrido) p/ SoR4 no Mali-450.
//
// A Wwise NATIVA roda (silenciosa internamente) mas faz toda a LOGICA de audio:
// decide QUAL musica/wem tocar por contexto (menu/fase/chefe) e ABRE o .wem streamed.
// Aqui interceptamos isso e tocamos o som de verdade via OpenAL+opusfile (dlopen):
//   - MUSICA: a Wwise abre o .wem streamed (gameassets/NNN.wem) -> ao_music_request(path)
//     -> thread de streaming decodifica o chunk Ogg Opus e toca em LOOP.
//   - SFX (gui/confirmacao/in-bank): post_event(name) -> FNV-1 -> manifest -> <id>.opus
//     -> toca via OpenAL (buffers em cache).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

// ---------------- OpenAL ----------------
typedef void ALCdevice; typedef void ALCcontext;
typedef int ALCboolean; typedef unsigned int ALuint; typedef int ALsizei; typedef int ALenum; typedef int ALint; typedef float ALfloat;
#define AL_FORMAT_MONO16   0x1101
#define AL_FORMAT_STEREO16 0x1103
#define AL_BUFFER          0x1009
#define AL_SOURCE_STATE    0x1010
#define AL_PLAYING         0x1012
#define AL_GAIN            0x100A
#define AL_LOOPING         0x1007
#define AL_BUFFERS_PROCESSED 0x1016
#define AL_BUFFERS_QUEUED    0x1015

static ALCdevice* (*p_alcOpenDevice)(const char*);
static ALCcontext*(*p_alcCreateContext)(ALCdevice*, const int*);
static ALCboolean (*p_alcMakeContextCurrent)(ALCcontext*);
static void (*p_alGenSources)(ALsizei, ALuint*);
static void (*p_alGenBuffers)(ALsizei, ALuint*);
static void (*p_alDeleteBuffers)(ALsizei, const ALuint*);
static void (*p_alBufferData)(ALuint, ALenum, const void*, ALsizei, ALsizei);
static void (*p_alSourcei)(ALuint, ALenum, ALint);
static void (*p_alSourcef)(ALuint, ALenum, ALfloat);
static void (*p_alSourcePlay)(ALuint);
static void (*p_alSourceStop)(ALuint);
static void (*p_alGetSourcei)(ALuint, ALenum, ALint*);
static void (*p_alSourceQueueBuffers)(ALuint, ALsizei, const ALuint*);
static void (*p_alSourceUnqueueBuffers)(ALuint, ALsizei, ALuint*);

// ---------------- opusfile ----------------
typedef void OggOpusFile;
static OggOpusFile* (*p_op_open_memory)(const unsigned char*, size_t, int*);
static int  (*p_op_read)(OggOpusFile*, int16_t*, int, int*);
static int  (*p_op_read_stereo)(OggOpusFile*, int16_t*, int);
static int  (*p_op_channel_count)(OggOpusFile*, int);
static int  (*p_op_pcm_seek)(OggOpusFile*, int64_t);
static void (*p_op_free)(OggOpusFile*);

static int g_ok = 0;
static char g_sfxdir[512] = "/storage/roms/sor4-test/audioout";

static void aolog(const char* s){
  const char* lp=getenv("WWISE_LOG"); if(!lp||!*lp) lp="/storage/roms/sor4-test/wwise.log";
  FILE* f=fopen(lp,"a"); if(f){fprintf(f,"%s\n",s); fclose(f);}
}

static int load_libs(void){
  void* h = dlopen("libopenal.so.1", RTLD_NOW|RTLD_GLOBAL); if(!h) h=dlopen("libopenal.so",RTLD_NOW|RTLD_GLOBAL);
  if(!h){ aolog("[audioout] dlopen libopenal FALHOU"); return 0; }
  p_alcOpenDevice=dlsym(h,"alcOpenDevice"); p_alcCreateContext=dlsym(h,"alcCreateContext");
  p_alcMakeContextCurrent=dlsym(h,"alcMakeContextCurrent");
  p_alGenSources=dlsym(h,"alGenSources"); p_alGenBuffers=dlsym(h,"alGenBuffers");
  p_alDeleteBuffers=dlsym(h,"alDeleteBuffers");
  p_alBufferData=dlsym(h,"alBufferData"); p_alSourcei=dlsym(h,"alSourcei");
  p_alSourcef=dlsym(h,"alSourcef"); p_alSourcePlay=dlsym(h,"alSourcePlay");
  p_alSourceStop=dlsym(h,"alSourceStop"); p_alGetSourcei=dlsym(h,"alGetSourcei");
  p_alSourceQueueBuffers=dlsym(h,"alSourceQueueBuffers");
  p_alSourceUnqueueBuffers=dlsym(h,"alSourceUnqueueBuffers");
  void* o = dlopen("libopusfile.so.0", RTLD_NOW|RTLD_GLOBAL); if(!o) o=dlopen("libopusfile.so",RTLD_NOW|RTLD_GLOBAL);
  if(!o){ aolog("[audioout] dlopen libopusfile FALHOU"); return 0; }
  p_op_open_memory=dlsym(o,"op_open_memory"); p_op_read=dlsym(o,"op_read");
  p_op_read_stereo=dlsym(o,"op_read_stereo");
  p_op_channel_count=dlsym(o,"op_channel_count"); p_op_pcm_seek=dlsym(o,"op_pcm_seek");
  p_op_free=dlsym(o,"op_free");
  if(!p_alcOpenDevice||!p_alGenSources||!p_op_open_memory||!p_op_read){ aolog("[audioout] dlsym incompleto"); return 0; }
  return 1;
}

static float sfx_gain(void){ const char* e=getenv("SOR4_SFXGAIN"); return (e&&*e)?(float)atof(e):0.55f; }
static float music_gain(void){ const char* e=getenv("SOR4_MUSICGAIN"); return (e&&*e)?(float)atof(e):0.7f; }

// =================== SFX (manifest -> .opus) ===================
typedef struct { uint32_t id; int ids[8]; int nids; } Entry;
static Entry* g_ent=NULL; static int g_nent=0, g_cap=0;
typedef struct { int id; ALuint buf; int valid; } BufC;
static BufC* g_buf=NULL; static int g_nbuf=0, g_cbuf=0;
#define NSFX 28
static ALuint g_sfx_src[NSFX];

static uint32_t fnv1_32(const char* s){
  uint32_t h=2166136261u;
  for(; *s; ++s){ unsigned char c=(unsigned char)*s; if(c>='A'&&c<='Z') c+=32; h*=16777619u; h^=c; }
  return h;
}
static void load_manifest(void){
  char path[600]; snprintf(path,sizeof(path),"%s/manifest.txt",g_sfxdir);
  FILE* f=fopen(path,"r"); if(!f){ aolog("[audioout] manifest nao encontrado"); return; }
  char line[4096];
  while(fgets(line,sizeof(line),f)){
    char* tab=strchr(line,'\t'); if(!tab) continue; *tab=0;
    uint32_t eid=(uint32_t)strtoul(line,NULL,10); char* ids=tab+1;
    if(g_nent>=g_cap){ g_cap=g_cap?g_cap*2:1024; g_ent=realloc(g_ent,g_cap*sizeof(Entry)); }
    Entry* e=&g_ent[g_nent]; e->id=eid; e->nids=0;
    char* p=ids; while(*p && e->nids<8){ int v=atoi(p); if(v>0) e->ids[e->nids++]=v; char* c=strchr(p,','); if(!c)break; p=c+1; }
    if(e->nids>0) g_nent++;
  }
  fclose(f);
  char b[128]; snprintf(b,sizeof(b),"[audioout] manifest: %d eventos SFX",g_nent); aolog(b);
}
// decodifica <id>.opus -> AL buffer (cache).
static ALuint sfx_buffer(int wem_id){
  for(int i=0;i<g_nbuf;i++) if(g_buf[i].id==wem_id) return g_buf[i].valid? g_buf[i].buf : 0;
  ALuint outbuf=0; int valid=0;
  char path[600]; snprintf(path,sizeof(path),"%s/%d.opus",g_sfxdir,wem_id);
  FILE* f=fopen(path,"rb");
  if(f){
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz>0){
      unsigned char* fil=malloc(sz);
      if(fread(fil,1,sz,f)==(size_t)sz){
        int err=0; OggOpusFile* of=p_op_open_memory(fil,sz,&err);
        if(of){
          int ch=p_op_channel_count(of,-1); if(ch<1)ch=2; if(ch>2)ch=2;
          int cap=48000*4, n=0; int16_t* pcm=malloc(cap*sizeof(int16_t));
          for(;;){
            if(n+11520*ch > cap){ cap*=2; pcm=realloc(pcm,cap*sizeof(int16_t)); }
            int li=0; int r=p_op_read(of,pcm+n,cap-n,&li);
            if(r<=0) break; n += r*ch;
          }
          p_op_free(of);
          if(n>0){ p_alGenBuffers(1,&outbuf);
            p_alBufferData(outbuf, ch==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16, pcm, n*(int)sizeof(int16_t), 48000); valid=1; }
          free(pcm);
        }
      }
      free(fil);
    }
    fclose(f);
  }
  if(g_nbuf>=g_cbuf){ g_cbuf=g_cbuf?g_cbuf*2:512; g_buf=realloc(g_buf,g_cbuf*sizeof(BufC)); }
  g_buf[g_nbuf].id=wem_id; g_buf[g_nbuf].buf=outbuf; g_buf[g_nbuf].valid=valid; g_nbuf++;
  return valid? outbuf : 0;
}
static ALuint sfx_free_source(void){
  for(int i=0;i<NSFX;i++){ ALint st=0; p_alGetSourcei(g_sfx_src[i],AL_SOURCE_STATE,&st); if(st!=AL_PLAYING) return g_sfx_src[i]; }
  return g_sfx_src[0];
}
void ao_post_event(const char* name){
  if(!g_ok||!name) return;
  uint32_t eid=fnv1_32(name);
  for(int i=0;i<g_nent;i++){
    if(g_ent[i].id==eid){
      Entry* e=&g_ent[i];
      for(int k=0;k<e->nids;k++){
        ALuint buf=sfx_buffer(e->ids[k]);
        if(buf){ ALuint src=sfx_free_source();
          p_alSourceStop(src); p_alSourcei(src,AL_BUFFER,(ALint)buf);
          p_alSourcef(src,AL_GAIN,sfx_gain()); p_alSourcei(src,AL_LOOPING,0); p_alSourcePlay(src);
          if(getenv("WWISE_TRACE")){ char b[160]; snprintf(b,sizeof(b),"[audioout] SFX '%s' wem=%d",name,e->ids[k]); aolog(b); }
          return; }
      }
      return;
    }
  }
}

// =================== MUSICA (wem streamed -> loop) ===================
// Acha o chunk "data" (Ogg Opus) dentro do .wem (RIFF/WAVE codec 0x3040).
static int wem_find_data(const unsigned char* buf, long sz, long* off, long* len){
  if(sz<44 || memcmp(buf,"RIFF",4)!=0 || memcmp(buf+8,"WAVE",4)!=0) return 0;
  long p=12;
  while(p+8<=sz){
    const unsigned char* c=buf+p;
    uint32_t csz = c[4]|(c[5]<<8)|(c[6]<<16)|((uint32_t)c[7]<<24);
    if(memcmp(c,"data",4)==0){ *off=p+8; *len=(long)csz; if(*off+*len>sz)*len=sz-*off; return 1; }
    p += 8 + csz + (csz&1);
  }
  return 0;
}

static pthread_mutex_t g_mus_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_mus_pending[1024] = "";   // path pedido (ou "" p/ parar)
static char g_mus_current[1024] = "";   // path tocando agora
static int  g_mus_run = 0;
static ALuint g_mus_src = 0;

// MUSICA por STREAMING (buffer-queue OpenAL) com FOLGA GRANDE p/ aguentar picos de
// CPU dos loads sem stutter. A musica faz loop re-abrindo/seekando o Opus no fim.
#define MUS_NBUF 16        // ~5.5s bufferizado (16 x 16384/48000) -> cobre loads pesados
#define MUS_FRAMES 16384   // samples/canal por buffer

// le ate MUS_FRAMES frames; no EOF, loopa (op_pcm_seek 0). retorna frames lidos.
static int mus_fill(OggOpusFile* of, int16_t* tmp, int ch, unsigned char* filbuf, long off, long len, OggOpusFile** pof){
  int got=0;
  while(got<MUS_FRAMES){
    int li=0; int r=p_op_read(of,tmp+got*ch,(MUS_FRAMES-got)*ch,&li);
    if(r<=0){ if(p_op_pcm_seek){ if(p_op_pcm_seek(of,0)!=0) break; } else { p_op_free(of); int e=0; of=p_op_open_memory(filbuf+off,len,&e); *pof=of; if(!of) break; } continue; }
    got+=r;
  }
  return got;
}

static void* music_thread(void* a){ (void)a;
  unsigned char* filbuf=NULL; OggOpusFile* of=NULL; long dataoff=0,datalen=0; int ch=2;
  ALuint bufs[MUS_NBUF]; int bufs_gen=0;
  int16_t* tmp=malloc(MUS_FRAMES*2*sizeof(int16_t));
  char playing[1024]="";
  for(;;){
    char want[1024];
    pthread_mutex_lock(&g_mus_mtx); strncpy(want,g_mus_pending,sizeof(want)-1); want[sizeof(want)-1]=0; pthread_mutex_unlock(&g_mus_mtx);
    if(strcmp(want,playing)!=0){
      if(g_mus_src){ p_alSourceStop(g_mus_src); p_alSourcei(g_mus_src,AL_BUFFER,0); }
      if(of){ p_op_free(of); of=NULL; }
      if(filbuf){ free(filbuf); filbuf=NULL; }
      playing[0]=0;
      pthread_mutex_lock(&g_mus_mtx); g_mus_current[0]=0; pthread_mutex_unlock(&g_mus_mtx);
      if(want[0]){
        FILE* f=fopen(want,"rb");
        if(f){ fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
          if(sz>0){ filbuf=malloc(sz);
            if(filbuf && fread(filbuf,1,sz,f)==(size_t)sz && wem_find_data(filbuf,sz,&dataoff,&datalen)){
              int err=0; of=p_op_open_memory(filbuf+dataoff,datalen,&err);
              if(of){ ch=p_op_channel_count(of,-1); if(ch<1)ch=1; if(ch>2)ch=2;
                if(!bufs_gen){ p_alGenBuffers(MUS_NBUF,bufs); bufs_gen=1; }
                if(!g_mus_src) p_alGenSources(1,&g_mus_src);
                p_alSourcei(g_mus_src,AL_LOOPING,0); p_alSourcef(g_mus_src,AL_GAIN,music_gain());
                int queued=0;
                for(int b=0;b<MUS_NBUF;b++){
                  int got=mus_fill(of,tmp,ch,filbuf,dataoff,datalen,&of); if(got<=0) break;
                  p_alBufferData(bufs[b], ch==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16, tmp, got*ch*(int)sizeof(int16_t), 48000);
                  p_alSourceQueueBuffers(g_mus_src,1,&bufs[b]); queued++;
                }
                if(queued>0){ p_alSourcePlay(g_mus_src); strncpy(playing,want,sizeof(playing)-1);
                  pthread_mutex_lock(&g_mus_mtx); strncpy(g_mus_current,want,sizeof(g_mus_current)-1); pthread_mutex_unlock(&g_mus_mtx);
                  { char b[1100]; snprintf(b,sizeof(b),"[audioout] MUSICA tocando %s (ch=%d, streaming %d bufs)",want,ch,queued); aolog(b); }
                } else { p_op_free(of); of=NULL; free(filbuf); filbuf=NULL; }
              } else { free(filbuf); filbuf=NULL; }
            } else { if(filbuf){free(filbuf);filbuf=NULL;} }
          }
          fclose(f);
        }
      }
    }
    // streaming: recicla buffers ja tocados (refill)
    if(of && playing[0] && g_mus_src){
      ALint proc=0; p_alGetSourcei(g_mus_src,AL_BUFFERS_PROCESSED,&proc);
      while(proc-->0){
        ALuint b=0; p_alSourceUnqueueBuffers(g_mus_src,1,&b);
        int got=mus_fill(of,tmp,ch,filbuf,dataoff,datalen,&of); if(got<=0){ break; }
        p_alBufferData(b, ch==1?AL_FORMAT_MONO16:AL_FORMAT_STEREO16, tmp, got*ch*(int)sizeof(int16_t), 48000);
        p_alSourceQueueBuffers(g_mus_src,1,&b);
      }
      ALint st=0; p_alGetSourcei(g_mus_src,AL_SOURCE_STATE,&st);
      ALint q=0; p_alGetSourcei(g_mus_src,AL_BUFFERS_QUEUED,&q);
      if(st!=AL_PLAYING && q>0) p_alSourcePlay(g_mus_src);
    }
    usleep(12000);   // ~80Hz: refila bem antes de esvaziar (5.5s de folga)
  }
  free(tmp); return NULL;
}

// pede p/ a thread de musica tocar este .wem (ou "" p/ parar)
void ao_music_request(const char* path){
  if(!g_ok) return;
  pthread_mutex_lock(&g_mus_mtx);
  strncpy(g_mus_pending, path?path:"", sizeof(g_mus_pending)-1); g_mus_pending[sizeof(g_mus_pending)-1]=0;
  pthread_mutex_unlock(&g_mus_mtx);
}
// a Wwise FECHOU este .wem -> a musica acabou/trocou no fluxo original. Para SE for a
// que esta pedida agora (se ja foi pedida outra, e' troca em andamento: nao mexe).
void ao_music_close(const char* path){
  if(!g_ok||!path) return;
  pthread_mutex_lock(&g_mus_mtx);
  // so para se o que fechou e' EXATAMENTE o que esta tocando agora E ainda e' o pedido
  // (se ja pediram outra musica, e' troca em andamento: nao mexe). Um probe open/close
  // no boot (antes de tocar, current vazio) nao para nada.
  if(g_mus_current[0] && strcmp(path,g_mus_current)==0 && strcmp(path,g_mus_pending)==0){ g_mus_pending[0]=0; }
  pthread_mutex_unlock(&g_mus_mtx);
}

// =================== init ===================
void ao_init(void){
  if(g_ok) return;
  const char* d=getenv("SOR4_AUDIO"); if(d&&*d) strncpy(g_sfxdir,d,sizeof(g_sfxdir)-1);
  if(!load_libs()){ aolog("[audioout] libs FALHOU -> sem audio custom"); return; }
  ALCdevice* dev=p_alcOpenDevice(NULL); if(!dev){ aolog("[audioout] alcOpenDevice NULL"); return; }
  ALCcontext* ctx=p_alcCreateContext(dev,NULL); if(!ctx){ aolog("[audioout] ctx NULL"); return; }
  p_alcMakeContextCurrent(ctx);
  p_alGenSources(NSFX,g_sfx_src);
  load_manifest();
  g_ok=1;
  if(!g_mus_run){ g_mus_run=1; pthread_t th; if(pthread_create(&th,NULL,music_thread,NULL)==0){ pthread_detach(th); } }
  aolog("[audioout] init OK (OpenAL+opusfile, SFX manifest + musica streaming)");
}
