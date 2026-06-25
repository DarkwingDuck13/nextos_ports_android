import subprocess, sys
LIB="$HOME/re4-build/lib/armeabi-v7a/libunity.so"
out = subprocess.check_output(["readelf","-W","--dyn-syms",LIB]).decode()
und=set()
for ln in out.splitlines():
    p=ln.split()
    if len(p)>=8 and p[6]=="UND":
        nm=p[7].split("@")[0]
        if nm and nm[0]!='_' or nm.startswith(("__","_ctype","_tolower","_toupper","_Unwind","_Znw","_Zna","_Zdl","_Zda")):
            und.add(nm)
        elif nm:
            und.add(nm)
und=sorted(x for x in und if x)
# egl_shim exporta egl_shim_<Rest>
def is_egl(s): return s.startswith("egl")
def egl_target(s): return "egl_shim_"+s[3:]
o=[]
o.append("// re4_imports.c -- GERADO. egl->egl_shim_*, resto->dlsym(RTLD_DEFAULT) em runtime + shims bionic.")
o.append('#include "imports.h"')
o.append('#include "so_util.h"')
o.append("#include <stdio.h>\n#include <stdint.h>\n#include <string.h>\n#include <dlfcn.h>\n#include <time.h>\n#include <pthread.h>\n#include <errno.h>")
# extern decls dos egl_shim usados
egls=[s for s in und if is_egl(s)]
for s in egls: o.append("extern void %s(void);"%egl_target(s))
# stub generico
o.append(r'''
static long stub_generic(void){ return 0; }
/* shims bionic */
static unsigned char g_ctype[384];
static short g_tolower[384], g_toupper[384];
static int g_ctype_init=0;
#include <ctype.h>
static void ctype_build(void){
  if(g_ctype_init) return; g_ctype_init=1;
  for(int c=0;c<256;c++){ unsigned char b=0;
    if(isupper(c))b|=0x01; if(islower(c))b|=0x02; if(isdigit(c))b|=0x04;
    if(isspace(c))b|=0x08; if(ispunct(c))b|=0x10; if(iscntrl(c))b|=0x20;
    if(isxdigit(c))b|=0x40; if(c==' ')b|=0x80;
    g_ctype[c+1]=b; g_tolower[c+1]=tolower(c); g_toupper[c+1]=toupper(c);
  }
}
/* pthread_cond_timedwait_relative_np (bionic): relativo -> absoluto */
static int cond_timedwait_rel(pthread_cond_t*c,pthread_mutex_t*m,const struct timespec*rel){
  if(!rel) return pthread_cond_wait(c,m);
  struct timespec abs; clock_gettime(CLOCK_REALTIME,&abs);
  abs.tv_sec+=rel->tv_sec; abs.tv_nsec+=rel->tv_nsec;
  if(abs.tv_nsec>=1000000000L){abs.tv_sec++;abs.tv_nsec-=1000000000L;}
  return pthread_cond_timedwait(c,m,&abs);
}
static long noop(void){ return 0; }
/* resolve um simbolo: bionic-shim > alias > dlsym > stub */
static void *re4_resolve(const char *nm){
  ctype_build();
  if(!strcmp(nm,"_ctype_")) return &g_ctype[1];
  if(!strcmp(nm,"_tolower_tab_")) return &g_tolower[1];
  if(!strcmp(nm,"_toupper_tab_")) return &g_toupper[1];
  if(!strcmp(nm,"__errno")) return dlsym(RTLD_DEFAULT,"__errno_location");
  if(!strcmp(nm,"setjmp")) return dlsym(RTLD_DEFAULT,"_setjmp");
  if(!strcmp(nm,"longjmp")) return dlsym(RTLD_DEFAULT,"_longjmp");
  if(!strcmp(nm,"__assert2")) return dlsym(RTLD_DEFAULT,"__assert_fail");
  if(!strcmp(nm,"pthread_cond_timedwait_relative_np")) return (void*)&cond_timedwait_rel;
  if(!strncmp(nm,"__google_potentially",20)) return (void*)&noop;
  if(!strcmp(nm,"ptrace")) return (void*)&noop;
  if(!strcmp(nm,"__sF")) return dlsym(RTLD_DEFAULT,"stdout"); /* best-effort */
  void *p=dlsym(RTLD_DEFAULT,nm);
  if(p) return p;
  /* Android-specific / nao-achado -> stub */
  return (void*)&stub_generic;
}
''')
# tabela
o.append("DynLibFunction dynlib_functions[] = {")
for s in und:
    if is_egl(s):
        o.append('  {"%s", (uintptr_t)&%s},'%(s,egl_target(s)))
    else:
        o.append('  {"%s", 0},'%s)
o.append("};")
o.append("size_t dynlib_numfunctions = sizeof(dynlib_functions)/sizeof(dynlib_functions[0]);")
o.append(r'''
/* preenche os func=0 via re4_resolve. Chamar ANTES de so_resolve. */
void re4_fill(void){
  int real=0,stub=0;
  for(size_t i=0;i<dynlib_numfunctions;i++){
    if(dynlib_functions[i].func) continue; /* egl ja setado */
    void *p=re4_resolve(dynlib_functions[i].symbol);
    dynlib_functions[i].func=(uintptr_t)p;
    if(p==(void*)&stub_generic){stub++; if(stub<=60) fprintf(stderr,"[STUB] %s\n",dynlib_functions[i].symbol);}
    else real++;
  }
  fprintf(stderr,"[re4_fill] %d resolvidos + %d stubs (de %zu)\n",real,stub,dynlib_numfunctions);
}
''')
open("src/imports.gen.c","w").write("\n".join(o)+"\n")
print("gerado: %d simbolos (%d egl)"%(len(und),len(egls)))
