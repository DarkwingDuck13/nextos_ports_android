/* katana_jni.c — driver JNI estático do Katana ZERO (GameMaker Studio 2 / YYC).
 * Fabrica JNIEnv/JavaVM falsos e dirige o runner YoYo:
 *   JNI_OnLoad -> RunnerJNILib_Startup -> Resume -> loop RunnerJNILib_Process.
 * Modelo: ports/sonicmania/src/sonic_jni.c (build Netflix, mesma família).
 * Os métodos do JNIEnv começam em ret0; a gente adiciona os que o engine
 * chamar (RE iterativo: run->crash->add). */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include "so_util.h"
#include "util.h" /* ret0 */
#include "opensles_shim.h"

#define JNI_PKG "Java_com_yoyogames_runner_RunnerJNILib_"

/* sentinelas p/ resultado de extension que finge ser java.lang.Double/String */
#define EXT_DOUBLE_SENTINEL ((void *)0x22220000)
#define EXT_STRING_SENTINEL ((void *)0x22220001)
static void *g_double_class = 0, *g_string_class = 0;
static int g_showui_called = 0;
static char fake_env[0x1000];
static char fake_vm[0x100];
static SDL_Window *g_win;
static SDL_GLContext g_ctx;
static char fake_thiz[8192];
static char fake_assetmgr[256];
static const char *g_datapath = "/storage/roms/ports/katanazero/";
static const char *g_methods[1024];
static int g_nmethods;
static int method_id(const char *n) {
  for (int i = 0; i < g_nmethods; i++) if (strcmp(g_methods[i], n) == 0) return i + 1;
  g_methods[g_nmethods] = strdup(n); return ++g_nmethods;
}
static const char *method_name(int id) { return (id >= 1 && id <= g_nmethods) ? g_methods[id - 1] : "?"; }

/* ---- métodos JNIEnv que o engine chama de volta (stubs; logam p/ a RE) ---- */
static void *j_FindClass(void *e, const char *n) {
  (void)e; fprintf(stderr, "[jni] FindClass %s\n", n ? n : "?");
  void *cls = (void *)(uintptr_t)(0x1000 + method_id(n ? n : "?"));
  if (n && strcmp(n, "java/lang/Double") == 0) g_double_class = cls;
  if (n && strcmp(n, "java/lang/String") == 0) g_string_class = cls;
  return cls;
}
static int j_GetMethodID(void *e, void *c, const char *n, const char *s) {
  (void)e; (void)c; (void)s; return method_id(n ? n : "?");
}
static int j_GetStaticMethodID(void *e, void *c, const char *n, const char *s) {
  (void)e; (void)c; fprintf(stderr, "[jni] GetStaticMethodID %s %s\n", n ? n : "?", s ? s : "?"); return method_id(n ? n : "?");
}
static int j_GetFieldID(void *e, void *c, const char *n, const char *s) {
  (void)e; (void)c; (void)s; return method_id(n ? n : "?");
}
static void j_CallVoidMethod(void *e, void *o, int id, ...) {
  (void)e; (void)o; const char *n = method_name(id);
  static int c = 0; if (c++ < 200) fprintf(stderr, "[jni] CallVoid %s\n", n);
}
static int j_CallIntMethod(void *e, void *o, int id, ...) {
  (void)e; (void)o; const char *n = method_name(id);
  if (strstr(n, "Width")) return 1280;
  if (strstr(n, "Height")) return 720;
  /* DynamicAssetExists(path) -> LoadSave::BundleFileExists usa isso. Se 0, o jogo
   * chama YYError fatal (Audio_SoundPlay no room_title, musica streamed song_*.ogg).
   * Checa existencia REAL no disco (cwd=port dir): strip "assets/" + basename. */
  if (strcmp(n, "DynamicAssetExists") == 0) {
    va_list ap; va_start(ap, id);
    const char *path = va_arg(ap, const char *);
    va_end(ap);
    int ex = 0;
    if ((uintptr_t)path > 0x100000) {
      ex = (access(path, F_OK) == 0);
      if (!ex && strncmp(path, "assets/", 7) == 0) ex = (access(path + 7, F_OK) == 0);
      if (!ex) { const char *b = strrchr(path, '/'); if (b) ex = (access(b + 1, F_OK) == 0); }
    }
    static int dc = 0; if (dc++ < 60)
      fprintf(stderr, "[jni] DynamicAssetExists '%s' -> %d\n", (uintptr_t)path > 0x100000 ? path : "?", ex);
    return ex;
  }
  static int c = 0; if (c++ < 200) fprintf(stderr, "[jni] CallInt %s\n", n);
  return 0;
}
static int j_CallBooleanMethod(void *e, void *o, int id, ...) {
  (void)e; (void)o; const char *n = method_name(id);
  int r = 0;
  if (strstr(n, "isRestartRequired")) r = 0;
  static int c = 0; if (c++ < 200) fprintf(stderr, "[jni] CallBool %s -> %d\n", n, r);
  return r;
}
static void *j_CallObjectMethod(void *e, void *o, int id, ...) {
  (void)e; (void)o; const char *n = method_name(id);
  if (strcmp(n, "CallExtensionFunction") == 0) {
    va_list ap; va_start(ap, id);
    const char *ext = va_arg(ap, const char *);
    const char *fn = va_arg(ap, const char *);
    va_end(ap);
    static int c = 0; if (c++ < 400)
      fprintf(stderr, "[ext] CallExtensionFunction ext='%s' fn='%s'\n",
              ((uintptr_t)ext > 0x100000) ? ext : "?", ((uintptr_t)fn > 0x100000) ? fn : "?");
    if ((uintptr_t)fn > 0x100000 && strstr(fn, "ShowUI")) g_showui_called = 1;
    if ((uintptr_t)fn > 0x100000) {
      if (strstr(fn, "PlayerID") || strstr(fn, "Profile") || strstr(fn, "Token") ||
          strstr(fn, "Locale") || strstr(fn, "Name"))
        return EXT_STRING_SENTINEL;                                /* string valida */
      /* resto das Nfxa* -> double sucesso (!=0) p/ qualquer check de retorno */
      return EXT_DOUBLE_SENTINEL;
    }
    return (void *)0x1;
  }
  static int c = 0; if (c++ < 200) fprintf(stderr, "[jni] CallObject %s\n", n);
  if (strcmp(n, "getClassLoader") == 0) return (void *)0x100;
  if (strcmp(n, "loadClass") == 0 || strcmp(n, "findClass") == 0) return (void *)0x200;
  return (void *)0x1;
}
static double g_ext_double = 1.0; /* NfxaInit success = qualquer != 0 */
static int j_IsInstanceOf(void *e, void *obj, void *cls) {
  (void)e;
  if (obj == EXT_DOUBLE_SENTINEL && (cls == g_double_class || !g_double_class)) return 1;
  if (obj == EXT_STRING_SENTINEL && (cls == g_string_class || !g_string_class)) return 1;
  return 0;
}
static double j_CallDoubleMethod(void *e, void *o, int id, ...) {
  (void)e; const char *n = method_name(id);
  if (o == EXT_DOUBLE_SENTINEL) { fprintf(stderr, "[ext] doubleValue() -> %f\n", g_ext_double); return g_ext_double; }
  (void)n; return 0.0;
}
static const char *j_GetStringUTFChars(void *e, void *str, void *isCopy) {
  (void)e; if (isCopy) *(char *)isCopy = 0;
  if (str == EXT_STRING_SENTINEL) return "katanaplayer1"; /* NfxaGetPlayerID -> ID valido */
  /* sentinelas de ext (0x2222xxxx) NAO sao char* -> string segura (evita strlen lixo) */
  if (((uintptr_t)str & ~0xFFFFul) == 0x22220000ul) return "0";
  /* jstrings falsos (CallObject/FindClass = ponteiros pequenos) NAO sao char*;
   * devolve "" p/ eles. Os args reais (cstr nossos, >0x100000) passam direto. */
  if ((uintptr_t)str < 0x100000) return "";
  return (const char *)str;
}
static void j_ReleaseStringUTFChars(void *e, void *str, const char *c) { (void)e; (void)str; (void)c; }
static void *j_NewStringUTF(void *e, const char *s) { (void)e; return (void *)s; }
static int j_GetEnv(void *vm, void **env, int v) { (void)vm; (void)v; *env = fake_env; return 0; }
/* GetJavaVM (idx 219): COggThread::StartThread faz env->GetJavaVM(&m_pJavaVM) p/ guardar o VM
 * e usar na thread de streaming. Sem isso m_pJavaVM fica null -> sem musica .ogg (silencio). */
static int j_GetJavaVM(void *env, void **vm) { (void)env; if (vm) *vm = fake_vm; return 0; }
static int j_AttachCurrentThread(void *vm, void **env, void *a) { (void)vm; (void)a; *env = fake_env; return 0; }
static int j_DetachCurrentThread(void *vm) { (void)vm; return 0; }
static void *j_NewRef(void *e, void *obj) { (void)e; return obj; }
static void *j_GetObjectClass(void *e, void *obj) { (void)e; (void)obj; return (void *)0x1; }
static int j_GetArrayLength(void *e, void *arr) { (void)e; (void)arr; return 0; }
static int j_ExceptionCheck(void *e) { (void)e; return 0; }
static int j_RegisterNatives(void *e, void *c, const void *m, int n) {
  (void)e; (void)c; (void)m; fprintf(stderr, "[jni] RegisterNatives n=%d\n", n); return 0;
}

static void build_env(void) {
  extern long (*g_jlog[256])(void);
  for (unsigned i = 0; i < 256; i++)
    ((uintptr_t *)fake_env)[i] = (uintptr_t)g_jlog[i];
  *(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env;
#define SET(off, fn) *(uintptr_t *)(fake_env + (off)) = (uintptr_t)(fn)
  SET(0x30, j_FindClass);            /* idx 6  FindClass */
  SET(0x70, j_ExceptionCheck);       /* idx 14 (aprox) */
  SET(0xA8, j_NewRef);               /* idx 21 NewGlobalRef */
  SET(0xB0, ret0);                   /* idx 22 DeleteGlobalRef */
  SET(0xB8, ret0);                   /* idx 23 DeleteLocalRef */
  SET(0xC8, j_NewRef);               /* idx 25 NewLocalRef */
  SET(0x100, j_IsInstanceOf);        /* idx 32 IsInstanceOf */
  SET(0xF8, j_GetObjectClass);       /* idx 31 GetObjectClass */
  SET(0x1d0, j_CallDoubleMethod);    /* idx 58 CallDoubleMethod */
  SET(0x1d8, j_CallDoubleMethod);    /* idx 59 CallDoubleMethodV */
  SET(0x1e0, j_CallDoubleMethod);    /* idx 60 CallDoubleMethodA */
  SET(0x108, j_GetMethodID);         /* idx 33 GetMethodID */
  SET(0x110, j_CallObjectMethod);    /* idx 34 CallObjectMethod */
  SET(0x118, j_CallObjectMethod);    /* idx 35 CallObjectMethodV */
  SET(0x188, j_CallIntMethod);       /* idx 49 CallIntMethod */
  SET(0x190, j_CallIntMethod);       /* idx 50 CallIntMethodV */
  SET(0x1C8, j_CallBooleanMethod);   /* CallBooleanMethod aprox */
  SET(0x1D0, j_CallBooleanMethod);   /* CallBooleanMethodV aprox */
  SET(0x1E8, j_CallVoidMethod);      /* idx 61 CallVoidMethod */
  SET(0x1F0, j_CallVoidMethod);      /* idx 62 CallVoidMethodV */
  SET(0x1C0, j_GetStaticMethodID);   /* aprox */
  SET(0x2F0, j_GetFieldID);          /* idx 94 GetFieldID aprox */
  SET(0x388, j_GetStaticMethodID);   /* idx 113 GetStaticMethodID */
  SET(0x390, j_CallObjectMethod);    /* idx 114 CallStaticObjectMethod */
  SET(0x398, j_CallObjectMethod);    /* idx 115 CallStaticObjectMethodV */
  SET(0x3a8, j_CallBooleanMethod);   /* idx 117 CallStaticBooleanMethod */
  SET(0x3b0, j_CallBooleanMethod);   /* idx 118 CallStaticBooleanMethodV */
  SET(0x408, j_CallIntMethod);       /* idx 129 CallStaticIntMethod */
  SET(0x410, j_CallIntMethod);       /* idx 130 CallStaticIntMethodV */
  SET(0x418, j_CallIntMethod);       /* idx 131 CallStaticIntMethodA */
  SET(0x468, j_CallVoidMethod);      /* idx 141 CallStaticVoidMethod */
  SET(0x470, j_CallVoidMethod);      /* idx 142 CallStaticVoidMethodV */
  SET(0x528, j_GetStaticMethodID);   /* GetStaticFieldID aprox */
  SET(0x538, j_NewStringUTF);        /* idx 167 NewStringUTF */
  SET(0x548, j_GetStringUTFChars);   /* idx 169 GetStringUTFChars */
  SET(0x550, j_ReleaseStringUTFChars);/* idx 170 ReleaseStringUTFChars */
  SET(0x558, j_GetArrayLength);      /* idx 171 GetArrayLength */
  SET(0x6A0, j_RegisterNatives);     /* idx 215 RegisterNatives */
  SET(0x6D8, j_GetJavaVM);           /* idx 219 GetJavaVM -> *vm = fake_vm (thread OGG) */
  SET(0x6B0, j_DetachCurrentThread); /* aprox */
#undef SET
}

/* ---- input GameMaker: KeyEvent(env,clazz, p0,p1,p2,p3,p4) = 5 ints ----
 * Mapeamento provável (YoYo): (keyboardId, action, keycode, unichar, meta).
 * action: 0=down 1=up. keycode = Android keycode. */
static void (*g_keyevent)(void *, void *, int, int, int, int, int) = NULL;
static void key(int android_kc, int down) {
  /* KeyEvent(env,clazz, action, keycode, ?, unicode, ?) -> RegisterAndroidKeyEvent(action=w0,keycode=w1).
   * action: 1=down(pressed), 0=up. */
  if (getenv("KZ_INLOG")) fprintf(stderr, "[kbd] KeyEvent kc=%d %s\n", android_kc, down ? "down" : "up");
  if (g_keyevent) g_keyevent(fake_env, fake_thiz, down ? 1 : 0, android_kc, 0, 0, 0);
}
static int sdl_key_to_android(int sc) {
  /* Android KEYCODE_* reais (path de TECLADO do GameMaker). gptokeyb->teclado->aqui. */
  switch (sc) {
    case SDL_SCANCODE_UP: return 19; case SDL_SCANCODE_DOWN: return 20;     /* DPAD_UP/DOWN */
    case SDL_SCANCODE_LEFT: return 21; case SDL_SCANCODE_RIGHT: return 22;
    case SDL_SCANCODE_RETURN: return 66; case SDL_SCANCODE_ESCAPE: return 111; /* ENTER / ESCAPE */
    case SDL_SCANCODE_SPACE: return 62;                                      /* SPACE */
    case SDL_SCANCODE_Z: return 54; case SDL_SCANCODE_X: return 52;          /* Z / X */
    case SDL_SCANCODE_C: return 31; case SDL_SCANCODE_V: return 50;          /* C / V */
    case SDL_SCANCODE_RSHIFT: case SDL_SCANCODE_LSHIFT: return 59;           /* SHIFT */
    default: return 0;
  }
}
/* KeyEvent (path TECLADO): o GAMEPLAY le keyboard_check (vk), nao o gamepad. O prompt
 * in-level "[X]" = tecla X (attack). Mapeia cada botao SDL p/ o KEYCODE ANDROID DE TECLADO
 * correspondente (Z=54 X=52 C=31 V=50 setas=19-22 ENTER=66 ESC=111 SHIFT=59 SPACE=62).
 * A tabela g_AndroidKeyCode do jogo traduz p/ o vk certo. (Alimentamos AMBOS: gamepad nativo
 * p/ menus + teclado p/ gameplay.) Layout casa com o gptk/console: X->X(attack). */
static int sdl_btn_to_android(int b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 19; case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 20;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 21; case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 22;
    case SDL_CONTROLLER_BUTTON_A: return 54;  /* Z */  case SDL_CONTROLLER_BUTTON_B: return 31;  /* C */
    case SDL_CONTROLLER_BUTTON_X: return 52;  /* X (attack - casa com o prompt [X]) */
    case SDL_CONTROLLER_BUTTON_Y: return 50;  /* V */
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 59;  /* SHIFT (dodge) */
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 62; /* SPACE */
    case SDL_CONTROLLER_BUTTON_START: return 66;  /* ENTER */
    case SDL_CONTROLLER_BUTTON_BACK: return 111;  /* ESC */
    default: return 0;
  }
}

/* ---- GAMEPAD NATIVO (GameMaker AndroidGamepad) ----
 * O menu do titulo (obj_titlemenu) usa o sistema de gamepad NATIVO (gamepad_button_check),
 * NAO o teclado (KeyEvent). Precisamos: (1) REGISTRAR um device via AndroidGamepadConnected
 * (id 0) p/ F_GamepadConnected==true, e (2) alimentar AndroidGamepadOnButtonDown/Up/Hat/Axis.
 * SDL GameController = layout Xbox padrao -> adapta a QUALQUER controle via gamecontrollerdb. */
static void (*g_gp_connected)(int, const char *, const char *, int, int, int, int, int, int);
static void (*g_gp_btndown)(int, int);
static void (*g_gp_btnup)(int, int);
static void (*g_gp_hat)(int, int, float, float);
static void (*g_gp_axis)(int, int, float);
static void (*g_gp_change)(void);
static int g_gp_id = 0;     /* device id que registramos (OnButtonDown casa por esse id) */
static int g_gp_ready = 0;
/* botao Android gamepad (keycode) p/ as faces/ombros/start-select; dpad vai por OnHat */
static int sdl_btn_to_gpkc(int b) {
  switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return 96;  case SDL_CONTROLLER_BUTTON_B: return 97;
    case SDL_CONTROLLER_BUTTON_X: return 99;  case SDL_CONTROLLER_BUTTON_Y: return 100;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 102; case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 103;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return 106; case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return 107;
    case SDL_CONTROLLER_BUTTON_START: return 108; case SDL_CONTROLLER_BUTTON_BACK: return 109;
    case SDL_CONTROLLER_BUTTON_GUIDE: return 110;
    default: return 0;
  }
}
/* L2/R2 sao EIXOS no SDL (triggers analogicos) mas o jogo espera BOTOES (KEYCODE_BUTTON_L2=104/R2=105).
 * Converte axis->botao com histerese (threshold) p/ nao repetir. */
static int g_lt_down, g_rt_down;
static void gp_trigger(int which, int down) { /* which: 0=L2 1=R2 */
  int kc = which ? 105 : 104;
  if (down) { if (g_gp_btndown) g_gp_btndown(g_gp_id, kc); }
  else      { if (g_gp_btnup)   g_gp_btnup(g_gp_id, kc); }
  if (g_gp_change) g_gp_change();
}
static int g_hat_u, g_hat_d, g_hat_l, g_hat_r; /* estado do dpad p/ compor o hat (x,y) */
static void gp_send_hat(void) {
  if (!g_gp_hat) return;
  float x = (g_hat_r ? 1.0f : 0.0f) - (g_hat_l ? 1.0f : 0.0f);
  float y = (g_hat_d ? 1.0f : 0.0f) - (g_hat_u ? 1.0f : 0.0f);
  g_gp_hat(g_gp_id, 0, x, y);
}
/* trata um botao SDL (down/up) pelo path nativo de gamepad */
static void gp_button(int sdl_btn, int down) {
  switch (sdl_btn) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:    g_hat_u = down; gp_send_hat(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  g_hat_d = down; gp_send_hat(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  g_hat_l = down; gp_send_hat(); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: g_hat_r = down; gp_send_hat(); break;
    default: {
      int kc = sdl_btn_to_gpkc(sdl_btn);
      if (getenv("KZ_INLOG")) fprintf(stderr, "[gp] btn sdl=%d -> kc=%d %s\n", sdl_btn, kc, down ? "down" : "up");
      if (kc) { if (down) { if (g_gp_btndown) g_gp_btndown(g_gp_id, kc); }
                else      { if (g_gp_btnup)   g_gp_btnup(g_gp_id, kc); } }
    }
  }
  if (g_gp_change) g_gp_change();
}
/* registra o gamepad nativo (id 0, layout Xbox). Mimetiza onGPDeviceAdded:
 * AndroidGamepadConnected(id, name, guid, vendor, product, buttonCount, 32, axisCount, hatCount) */
static void gp_register(void) {
  g_gp_connected = (void (*)(int, const char *, const char *, int, int, int, int, int, int))
    so_find_addr_safe("_Z23AndroidGamepadConnectediPcS_iiiiii");
  g_gp_btndown = (void (*)(int, int))so_find_addr_safe("_Z26AndroidGamepadOnButtonDownii");
  g_gp_btnup   = (void (*)(int, int))so_find_addr_safe("_Z24AndroidGamepadOnButtonUpii");
  g_gp_hat     = (void (*)(int, int, float, float))so_find_addr_safe("_Z19AndroidGamepadOnHatiiff");
  g_gp_axis    = (void (*)(int, int, float))so_find_addr_safe("_Z20AndroidGamepadOnAxisiif");
  g_gp_change  = (void (*)(void))so_find_addr_safe("_Z22AndroidGamepadOnChangev");
  fprintf(stderr, "[gp] conn=%p down=%p up=%p hat=%p axis=%p chg=%p\n",
          (void*)g_gp_connected, (void*)g_gp_btndown, (void*)g_gp_btnup,
          (void*)g_gp_hat, (void*)g_gp_axis, (void*)g_gp_change);
  if (g_gp_connected) {
    static char nm[] = "Microsoft X-Box 360 pad";
    static char guid[] = "030000005e0400008e02000010010000";
    g_gp_connected(g_gp_id, nm, guid, 0x045e, 0x028e, 17, 32, 6, 1);
    if (g_gp_change) g_gp_change();
    g_gp_ready = 1;
    fprintf(stderr, "[gp] device %d registrado (Xbox 360, 17btn/6axis/1hat)\n", g_gp_id);
    /* dump da tabela keycode->gp_index (AndroidGamepadOnButtonDown le table[i]==keycode) */
    { extern void *text_base;
      uintptr_t tp = *(uintptr_t *)((char *)text_base + 0x47d4000 + 2696);
      if (tp) { fprintf(stderr, "[gp] btn-map table @0x%lx:", (unsigned long)tp);
        for (int i = 0; i < 33; i++) fprintf(stderr, " [%d]=%d", i, *(int *)(tp + i * 4));
        fprintf(stderr, "\n"); } }
  } else fprintf(stderr, "[gp] AndroidGamepadConnected NAO achado\n");
}

/* getJNIEnv do libyoyo retorna pthread_getspecific(tls_key) = JNIEnv thread-local. A thread
 * de streaming OGG (COggThread, musica .ogg) e criada internamente e NUNCA tem o TLS setado
 * -> getJNIEnv()==NULL -> "m_pJavaVM was null for OGG thread" -> falha ao abrir o .ogg -> SEM
 * MUSICA (silencio). Hook: retorna fake_env SEMPRE (em qualquer thread) -> OGG abre o song. */
static void *my_getjnienv(void) { return fake_env; }

/* DIAG (KZ_KBHOOK): hook YYGML_keyboard_check(int key) p/ logar quais teclas o jogo polla.
 * Revela a tecla do ataque + se o keyboard e lido no gameplay. Retorna 0 (nao temos call-through). */
static int my_keyboard_check(int key) {
  static char seen[1024]; if (key >= 0 && key < 1024 && !seen[key]) { seen[key] = 1;
    fprintf(stderr, "[kbcheck] poll vk=%d\n", key); }
  return 0;
}
/* DIAG (KZ_GPHOOK): hook F_GamepadButtonCheckPressed p/ LOGAR qual gp_button o jogo polla
 * (revela o botao do ataque) E reimplementar o check real (sem quebrar menu/gameplay). */
extern void *text_base;
static int (*g_yygetint32)(const void *, int);
static int (*g_translate_gp)(int, int);
static int (*g_gmgp_pressed)(void *, int);
static void my_gp_check_pressed(void *result, void *self, void *other, int argc, void *args) {
  (void)self; (void)other; (void)argc;
  int device = g_yygetint32 ? g_yygetint32(args, 0) : 0;
  int button = g_yygetint32 ? g_yygetint32(args, 1) : 0;
  int r = 0; int bi = -1; void *gp = NULL;
  if (g_translate_gp && g_gmgp_pressed) {
    bi = g_translate_gp(device, button);
    uintptr_t p1 = *(uintptr_t *)((char *)text_base + 0x47cf590);
    if (p1) { uintptr_t p2 = *(uintptr_t *)p1;
      if (p2) { gp = *(void **)(p2 + (uintptr_t)device * 8); if (gp) r = g_gmgp_pressed(gp, bi); } }
  }
  static char seen[131072]; unsigned k = (unsigned)button & 0x1FFFF;
  if (!seen[k]) { seen[k] = 1; fprintf(stderr, "[gpcheck] dev=%d btn=0x%x bi=%d gp=%p r=%d\n", device, button, bi, gp, r); }
  /* KZ_GPFORCE: forca TRUE p/ o botao indicado (testar se o prompt e gamepad-pressed) */
  { const char *fb = getenv("KZ_GPFORCE"); if (fb && button == atoi(fb)) r = 1; }
  if (result) { /* RValue real: value@0, kind@8, type@12 = 0 (como o original inicializa) */
    *(double *)result = r ? 1.0 : 0.0;
    *(int *)((char *)result + 8) = 0;
    *(int *)((char *)result + 12) = 0; }
}

/* ---- YYError NAO-FATAL ----
 * YYError -> Error_Show_Action -> __cxa_throw (YYGMLException). No runner Android real
 * ha um try/catch no topo (Java) que mostra o erro e segue/sai limpo. Nos chamamos Process()
 * direto em C SEM handler -> a excecao propaga -> std::terminate -> SIGSEGV (fwrite no
 * terminate). Isso matava o jogo no AUTOSAVE (cloud write timeout -> status -9902 -> YYError).
 * Hook: loga a msg e RETORNA (nao lanca) -> o caller (cbCloudWrite etc.) segue; o save LOCAL
 * ja persiste (o jogo mantem KatanaSave.zero), so o sync de cloud falha em silencio. */
static void my_yyerror(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "[YYError-naofatal] ");
  if (fmt && (uintptr_t)fmt > 0x10000) vfprintf(stderr, fmt, ap); else fprintf(stderr, "(fmt?)");
  fprintf(stderr, "\n");
  va_end(ap);
  /* NAO lanca excecao -> sem terminate/crash */
}

/* ---- CLOUD SAVE/LOAD: completar operacoes pendentes p/ NAO dar timeout (10s) ----
 * O jogo (edicao Netflix) salva/carrega via NfxaCloudRead/Write -> cria uma "operation"
 * e ESPERA o resultado async. Nos (lado-Java) entregamos via JNI CloudResultString.
 * Sem isso a op da TIMEOUT (10s) -> status -9902 -> no WRITE chama YYError -> __cxa_throw
 * sem handler no nosso loop C -> terminate -> SIGSEGV (autosave no gameplay matava o jogo).
 * Layout da operation (deduzido do disasm de CloudResultString):
 *   lista: head em *(base+0x47cf1c0); node->next em +8; node->reqId em +72; node->state em +68.
 * CloudResultString(env,clazz, jstring data, int status, int reqId): casa por reqId, seta
 * state=7 (completo), status->+76, copia data->buffer+16. status=200 = sucesso. Entregar
 * "" (vazio) = "sem dado de cloud" no read (jogo usa save local) e "sucesso" no write. */
static int (*g_cloudresult_str)(void *, void *, const char *, int, int) = NULL;
static volatile char *g_oplist_head_addr = NULL; /* endereco que guarda o ponteiro-head */
static void cloud_resolve(void) {
  uintptr_t crs = so_find_addr_safe("Java_com_yoyogames_runner_RunnerJNILib_CloudResultString");
  if (crs) {
    g_cloudresult_str = (int (*)(void *, void *, const char *, int, int))crs;
    uintptr_t base = crs - 0x14fb2ecUL;          /* vaddr de CloudResultString na ELF */
    g_oplist_head_addr = (volatile char *)(base + 0x47cf1c0UL);
    fprintf(stderr, "[cloud] CloudResultString=0x%lx base=0x%lx oplist=%p\n",
            (unsigned long)crs, (unsigned long)base, (void *)g_oplist_head_addr);
  } else fprintf(stderr, "[cloud] CloudResultString NAO achado\n");
}
/* drena operacoes de cloud pendentes (state!=7) completando-as com sucesso. Chamado/frame. */
static void cloud_drain(void) {
  if (!g_cloudresult_str || !g_oplist_head_addr) return;
  /* DUPLA indirecao (do disasm): container=*(0x47cf1c0); head=*container; next=node+8 */
  char *container = *(char **)g_oplist_head_addr;
  if (!container) return;
  char *node = *(char **)container;
  static int dbg = 0;
  if (node && dbg++ < 12) fprintf(stderr, "[cloud] drain: container=%p head=%p state=%d reqId=%d\n",
                                  (void *)container, (void *)node, *(int *)(node + 68), *(int *)(node + 72));
  int guard = 0;
  while (node && guard++ < 64) {
    int state = *(int *)(node + 68);
    int reqId = *(int *)(node + 72);
    if (state != 7) { /* pendente -> completa com sucesso (data vazia) */
      static int lc = 0; if (lc++ < 40) fprintf(stderr, "[cloud] completando op reqId=%d (state=%d)\n", reqId, state);
      g_cloudresult_str(fake_env, fake_thiz, "", 200, reqId);
    }
    node = *(char **)(node + 8);
  }
}

/* teardown limpo do EGL/SDL antes de sair -> senao o Mali fbdev TRAVA entre runs
 * (tela preta, fb0 read=0). Chamado no max-tempo e em SIGTERM. */
static volatile sig_atomic_t g_quit = 0;
static void kz_teardown(void) {
  if (g_ctx) { glFinish(); SDL_GL_MakeCurrent(g_win, NULL); SDL_GL_DeleteContext(g_ctx); g_ctx = NULL; }
  if (g_win) { SDL_DestroyWindow(g_win); g_win = NULL; }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  SDL_Quit();
}
static void kz_term(int s) { (void)s; g_quit = 1; }

static int my_fileexists(const char *p) {
  if (!p) return 0;
  /* opcoes (.ini): forca existir p/ o IniFile SER construido (vazio = seguro).
   * resto (game.droid externo, saves, etc): check real -> nao inventa arquivos. */
  int forced = (strstr(p, ".ini") != NULL);
  int r = forced ? 1 : (access(p, F_OK) == 0);
  static int c = 0; if (c++ < 80) fprintf(stderr, "[FileExists] '%s' -> %d%s\n", p, r, forced ? " (forcado .ini)" : "");
  return r;
}

/* HOOK LoadSave::BundleFileExists(filepath): o original checa APK assets (via JNI
 * DynamicAssetExists) ou game.droid (zip) -> NAO acha os song_*.ogg streamed que
 * estao soltos no port dir. No room_title Audio_SoundPlay chama isso p/ a musica;
 * se 0 -> YYError fatal -> crash (fwrite). Checamos o FILESYSTEM real (cwd=port dir):
 * strip "assets/" + basename. Existe no disco -> 1 (depois AAssetManager_open abre). */
static int my_bundlefileexists(const char *p) {
  if (!p) return 0;
  int ex = (access(p, F_OK) == 0);
  if (!ex && strncmp(p, "assets/", 7) == 0) ex = (access(p + 7, F_OK) == 0);
  if (!ex) { const char *b = strrchr(p, '/'); if (b) ex = (access(b + 1, F_OK) == 0); }
  static int c = 0; if (c++ < 80) fprintf(stderr, "[BundleExists] '%s' -> %d\n", p, ex);
  return ex;
}

/* postar um async SOCIAL event (subtype 70) com ds_map -> destrava o gate Netflix.
 * Somos o lado-Java: chamamos os JNI exportados dsMapCreate/AddString/AddInt +
 * CreateAsynEventWithDSMap. eventType="Nfx_onPlayerAccessChanged" + access concedido. */
static void post_social_event(const char *eventType) {
  static uintptr_t f_create = 0, f_addstr = 0, f_addint = 0, f_async = 0; static int init = 0;
  if (!init) { init = 1;
    f_create = so_find_addr_safe(JNI_PKG "dsMapCreate");
    f_addstr = so_find_addr_safe(JNI_PKG "dsMapAddString");
    f_addint = so_find_addr_safe(JNI_PKG "dsMapAddInt");
    f_async  = so_find_addr_safe(JNI_PKG "CreateAsynEventWithDSMap");
    fprintf(stderr, "[nfx] dsMapCreate=%p AddString=%p AddInt=%p Async=%p\n",
            (void*)f_create, (void*)f_addstr, (void*)f_addint, (void*)f_async); }
  if (!f_create || !f_addstr || !f_async) return;
  int map = ((int (*)(void *, void *))f_create)(fake_env, fake_thiz);
  void (*addstr)(void *, void *, int, const char *, const char *) = (void *)f_addstr;
  void (*addint)(void *, void *, int, const char *, int) = (void *)f_addint;
  addstr(fake_env, fake_thiz, map, "eventType", eventType);
  addstr(fake_env, fake_thiz, map, "event_type", eventType);
  addstr(fake_env, fake_thiz, map, "type", eventType);
  addstr(fake_env, fake_thiz, map, "Current_Event_Type", eventType);
  addstr(fake_env, fake_thiz, map, "playerID", "katanaplayer1");
  addstr(fake_env, fake_thiz, map, "player_id", "katanaplayer1");
  addstr(fake_env, fake_thiz, map, "playerId", "katanaplayer1");
  addstr(fake_env, fake_thiz, map, "current", "katanaplayer1");   /* player que GANHOU acesso */
  addstr(fake_env, fake_thiz, map, "previous", "katanaplayer1");
  addstr(fake_env, fake_thiz, map, "name", "katanaplayer1");
  if (addint) {
    addint(fake_env, fake_thiz, map, "hasAccess", 1);
    addint(fake_env, fake_thiz, map, "access", 1);
    addint(fake_env, fake_thiz, map, "granted", 1);
    addint(fake_env, fake_thiz, map, "success", 1);
    addint(fake_env, fake_thiz, map, "status", 0);
    addint(fake_env, fake_thiz, map, "id", 1);          /* casa com handle do request (ShowUI=1.0) */
    addint(fake_env, fake_thiz, map, "handle", 1);
    addint(fake_env, fake_thiz, map, "request_id", 1);
    addint(fake_env, fake_thiz, map, "eventType_id", 1);
  }
  int evidx = getenv("KZ_NFXEVIDX") ? atoi(getenv("KZ_NFXEVIDX")) : 70;
  ((void (*)(void *, void *, int, int))f_async)(fake_env, fake_thiz, map, evidx);
  fprintf(stderr, "[nfx] posted social '%s' map=%d evidx=%d\n", eventType, map, evidx);
}

void jni_run(void) {
  int W = 1280, H = 720;
  const char *ws = getenv("KZ_W"), *hs = getenv("KZ_H");
  if (ws) W = atoi(ws); if (hs) H = atoi(hs);
  if (getenv("KZ_NFXINIT")) g_ext_double = atof(getenv("KZ_NFXINIT"));

  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  g_win = SDL_CreateWindow("Katana ZERO", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           W, H, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!g_win) fprintf(stderr, "[gl] CreateWindow FALHOU: %s\n", SDL_GetError());
  g_ctx = SDL_GL_CreateContext(g_win);
  if (!g_ctx) fprintf(stderr, "[gl] CreateContext FALHOU: %s\n", SDL_GetError());
  SDL_GL_MakeCurrent(g_win, g_ctx);
  SDL_GL_SetSwapInterval(getenv("KZ_VSYNC") ? 1 : 0);
  int dw = W, dh = H; SDL_GL_GetDrawableSize(g_win, &dw, &dh);
  fprintf(stderr, "[gl] contexto GLES2 win=%p ctx=%p drawable=%dx%d\n", (void *)g_win, (void *)g_ctx, dw, dh);

  build_env();
  for (unsigned i = 0; i < sizeof(fake_vm) / sizeof(uintptr_t); i++)
    ((uintptr_t *)fake_vm)[i] = (uintptr_t)ret0;
  *(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm;
  *(uintptr_t *)(fake_vm + 0x20) = (uintptr_t)j_AttachCurrentThread; /* idx 4 */
  *(uintptr_t *)(fake_vm + 0x30) = (uintptr_t)j_GetEnv;              /* idx 6 */
  *(uintptr_t *)(fake_vm + 0x38) = (uintptr_t)j_AttachCurrentThread; /* idx 7 */

  /* SL_IID p/ o opensles_shim (audio yyal -> OpenSLES -> SDL) */
  { extern void *SL_IID_ENGINE_shim, *SL_IID_PLAY_shim, *SL_IID_BUFFERQUEUE_shim, *SL_IID_ANDROIDSIMPLEBUFFERQUEUE_shim,
        *SL_IID_VOLUME_shim, *SL_IID_RECORD_shim, *SL_IID_ANDROIDCONFIGURATION_shim;
    SL_IID_ENGINE_shim = (void *)sl_IID_ENGINE; SL_IID_PLAY_shim = (void *)sl_IID_PLAY;
    SL_IID_BUFFERQUEUE_shim = (void *)sl_IID_BUFFERQUEUE; SL_IID_ANDROIDSIMPLEBUFFERQUEUE_shim = (void *)sl_IID_BUFFERQUEUE;
    SL_IID_VOLUME_shim = (void *)sl_IID_VOLUME; SL_IID_RECORD_shim = (void *)0x5151; SL_IID_ANDROIDCONFIGURATION_shim = (void *)0x5152;
    fprintf(stderr, "[sl] SL_IID wired\n"); }

  /* HOOK FileExists: no 1o boot o arquivo de opcoes nao existe -> RunnerLoadGame
   * pula a construcao do IniFile -> IO_Setup(NULL) -> crash em IniFile::find.
   * Forcamos =1 p/ o IniFile SER construido (vazio e seguro; find em map vazio ok).
   * Loga o path p/ sabermos onde criar o arquivo real depois. */
  { extern void hook_arm64(uintptr_t, uintptr_t);
    uintptr_t fe = so_find_addr_safe("_Z10FileExistsPKc");
    if (fe) { so_make_text_writable(); hook_arm64(fe, (uintptr_t)my_fileexists);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[hook] FileExists @0x%lx -> forca 1\n", (unsigned long)fe); }
    else fprintf(stderr, "[hook] FileExists NAO achado\n");
    /* BundleFileExists: acha song_*.ogg no disco -> Audio_SoundPlay nao crasha no titulo */
    uintptr_t bfe = so_find_addr_safe("_ZN8LoadSave16BundleFileExistsEPKc");
    if (bfe) { so_make_text_writable(); hook_arm64(bfe, (uintptr_t)my_bundlefileexists);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[hook] BundleFileExists @0x%lx -> check disco\n", (unsigned long)bfe); }
    else fprintf(stderr, "[hook] BundleFileExists NAO achado\n");
    /* YYError nao-fatal: autosave (cloud write timeout) nao mata o jogo */
    uintptr_t yye = so_find_addr_safe("_Z7YYErrorPKcz");
    if (yye) { so_make_text_writable(); hook_arm64(yye, (uintptr_t)my_yyerror);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[hook] YYError @0x%lx -> nao-fatal\n", (unsigned long)yye); }
    else fprintf(stderr, "[hook] YYError NAO achado\n");
    /* getJNIEnv -> fake_env sempre: thread OGG abre a musica .ogg (sem isso = silencio) */
    uintptr_t gje = so_find_addr_safe("getJNIEnv");
    if (gje) { so_make_text_writable(); hook_arm64(gje, (uintptr_t)my_getjnienv);
      so_make_text_executable(); so_flush_caches();
      fprintf(stderr, "[hook] getJNIEnv @0x%lx -> fake_env\n", (unsigned long)gje); }
    else fprintf(stderr, "[hook] getJNIEnv NAO achado\n");
    /* DIAG keyboard: hook keyboard_check p/ logar teclas pollada (so com KZ_KBHOOK) */
    if (getenv("KZ_KBHOOK")) { uintptr_t kc = so_find_addr_safe("_Z20YYGML_keyboard_checki");
      if (kc) { so_make_text_writable(); hook_arm64(kc, (uintptr_t)my_keyboard_check);
        so_make_text_executable(); so_flush_caches();
        fprintf(stderr, "[hook] keyboard_check @0x%lx -> DIAG\n", (unsigned long)kc); } }
    /* DIAG gamepad: hook F_GamepadButtonCheckPressed p/ logar gp_button do prompt (KZ_GPHOOK) */
    if (getenv("KZ_GPHOOK")) {
      g_yygetint32 = (int (*)(const void *, int))so_find_addr_safe("_Z10YYGetInt32PK6RValuei");
      g_translate_gp = (int (*)(int, int))so_find_addr_safe("_Z23TranslateGamepadButtonMii");
      g_gmgp_pressed = (int (*)(void *, int))so_find_addr_safe("_ZNK9GMGamePad13ButtonPressedEi");
      uintptr_t gpc = so_find_addr_safe("_Z27F_GamepadButtonCheckPressedR6RValueP9CInstanceS2_iPS_");
      if (gpc && g_yygetint32 && g_translate_gp && g_gmgp_pressed) {
        so_make_text_writable(); hook_arm64(gpc, (uintptr_t)my_gp_check_pressed);
        so_make_text_executable(); so_flush_caches();
        fprintf(stderr, "[hook] GamepadButtonCheckPressed @0x%lx -> DIAG\n", (unsigned long)gpc); }
    } }

  uintptr_t onload = so_find_addr_safe("JNI_OnLoad");
  if (onload) {
    int v = ((int (*)(void *, void *))onload)(fake_vm, NULL);
    fprintf(stderr, "[drv] JNI_OnLoad => 0x%x\n", v);
  } else fprintf(stderr, "[drv] JNI_OnLoad nao achado\n");

  uintptr_t startup = so_find_addr_safe(JNI_PKG "Startup");
  uintptr_t process = so_find_addr_safe(JNI_PKG "Process");
  uintptr_t resume  = so_find_addr_safe(JNI_PKG "Resume");
  uintptr_t initgl  = so_find_addr_safe(JNI_PKG "initGLFuncs");
  g_keyevent = (void (*)(void *, void *, int, int, int, int, int))so_find_addr_safe(JNI_PKG "KeyEvent");
  fprintf(stderr, "[drv] Startup=0x%lx Process=0x%lx Resume=0x%lx initGL=0x%lx KeyEvent=%p\n",
          (unsigned long)startup, (unsigned long)process, (unsigned long)resume, (unsigned long)initgl, (void *)g_keyevent);
  if (!startup || !process) { fprintf(stderr, "[drv] Startup/Process ausente — abortando\n"); return; }

  /* Startup(env, clazz, jstring apkPath, jstring saveDir, jstring ?, int density, bool).
   * arg2 (x25) = caminho do APK -> GetStringUTFChars -> zip_open (le game.droid de
   * dentro do APK). args de path = cstr reais (>0x100000) passam por GetStringUTFChars. */
  (void)fake_assetmgr;
  static char apk_path[512];
  const char *apk_env = getenv("KZ_APK");
  snprintf(apk_path, sizeof(apk_path), "%s", apk_env ? apk_env : "/storage/roms/ports/katanazero/katanazero.apk");
  fprintf(stderr, "[drv] chamando Startup(apk='%s', save='%s', density, consent)...\n", apk_path, g_datapath);
  ((void (*)(void *, void *, const char *, const char *, const char *, int, int))startup)(
      fake_env, fake_thiz, apk_path, g_datapath, g_datapath, 320, 1);
  fprintf(stderr, "[drv] Startup retornou\n");

  if (initgl) { fprintf(stderr, "[drv] initGLFuncs()\n"); ((void (*)(void *, void *))initgl)(fake_env, fake_thiz); }
  if (resume) { fprintf(stderr, "[drv] Resume()\n"); ((void (*)(void *, void *))resume)(fake_env, fake_thiz); }

  { int nj = SDL_NumJoysticks(); fprintf(stderr, "[input] %d joysticks\n", nj);
    for (int i = 0; i < nj; i++) { if (SDL_IsGameController(i)) { SDL_GameControllerOpen(i); fprintf(stderr, "[input] gamecontroller %d aberto\n", i); }
      else { SDL_JoystickOpen(i); fprintf(stderr, "[input] joystick RAW %d (%s)\n", i, SDL_JoystickNameForIndex(i)); } } }
  gp_register(); /* registra gamepad nativo (id 0) -> menu do titulo responde */
  cloud_resolve(); /* completa cloud save/load -> sem timeout/crash no autosave */

  /* IO_Update(): computa os estados "pressed"/"released" do input (keyboard/mouse/gamepad)
   * a cada frame a partir do estado raw. NAO tem caller no engine (o runner Android chamava
   * pelo loop Java que nao usamos) -> gamepad_button_check_PRESSED sempre 0 -> prompts/ataque
   * que usam _pressed nao disparam (menu usa _check/held, por isso funcionava). Chamar por frame. */
  void (*io_update)(void) = (void (*)(void))so_find_addr_safe("_Z9IO_Updatev");
  fprintf(stderr, "[drv] IO_Update=%p (pressed/released por frame)\n", (void *)io_update);

  long maxf = getenv("KZ_MAXFRAMES") ? atol(getenv("KZ_MAXFRAMES")) : 0;
  long maxs = getenv("KZ_MAXSECONDS") ? atol(getenv("KZ_MAXSECONDS")) : 0;
  int inlog = getenv("KZ_INLOG") != NULL;
  Uint32 t0 = SDL_GetTicks();
  signal(SIGTERM, kz_term); signal(SIGINT, kz_term);
  fprintf(stderr, "[drv] entrando no loop Process\n");
  for (long f = 0;; f++) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (inlog && ev.type != SDL_MOUSEMOTION) fprintf(stderr, "[ev] type=0x%x\n", ev.type);
      if (ev.type == SDL_QUIT) return;
      else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
        int kc = sdl_key_to_android(ev.key.keysym.scancode);
        if (inlog) fprintf(stderr, "[ev] KEY sc=%d -> kc=%d %s\n", ev.key.keysym.scancode, kc, ev.type == SDL_KEYDOWN ? "down" : "up");
        if (kc) key(kc, ev.type == SDL_KEYDOWN);
      } else if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP) {
        int down = (ev.type == SDL_CONTROLLERBUTTONDOWN);
        if (inlog) fprintf(stderr, "[ev] CBUTTON %d %s\n", ev.cbutton.button, down ? "down" : "up");
        gp_button(ev.cbutton.button, down);            /* path NATIVO de gamepad (UNICO por padrao) */
        /* KeyEvent (teclado) so com KZ_KBDFEED -> por padrao input LIMPO so do gamepad nativo
         * (evita conflito: feed duplo confundia o input do gameplay). */
        if (getenv("KZ_KBDFEED")) { int kc = sdl_btn_to_android(ev.cbutton.button); if (kc) key(kc, down); }
      } else if (ev.type == SDL_CONTROLLERAXISMOTION) {
        /* eixos analogicos -> path nativo (axis 0=LX 1=LY 2=RX 3=RY 4=LT 5=RT) */
        if (g_gp_axis && g_gp_ready) g_gp_axis(g_gp_id, ev.caxis.axis, ev.caxis.value / 32767.0f);
        /* L2/R2 (triggers) tambem como BOTAO: histerese 50%/30% p/ o jogo (gp_shoulderlb/rb) */
        if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
          if (!g_lt_down && ev.caxis.value > 16000) { g_lt_down = 1; gp_trigger(0, 1); }
          else if (g_lt_down && ev.caxis.value < 10000) { g_lt_down = 0; gp_trigger(0, 0); }
        } else if (ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
          if (!g_rt_down && ev.caxis.value > 16000) { g_rt_down = 1; gp_trigger(1, 1); }
          else if (g_rt_down && ev.caxis.value < 10000) { g_rt_down = 0; gp_trigger(1, 0); }
        }
      } else if ((ev.type == SDL_JOYBUTTONDOWN || ev.type == SDL_JOYBUTTONUP) && getenv("KZ_JOYFEED")) {
        /* SD_JOYBUTTON DESABILITADO por padrao: o USB Gamepad gera CONTROLLERBUTTON E JOYBUTTON
         * juntos -> feed duplo/conflitante. So o CONTROLLERBUTTON (gamepad nativo) por padrao. */
        if (inlog) fprintf(stderr, "[ev] JOYBUTTON %d %s\n", ev.jbutton.button, ev.type == SDL_JOYBUTTONDOWN ? "down" : "up");
        int kc = 0; switch (ev.jbutton.button) { case 0: kc = 96; break; case 1: kc = 97; break; case 2: kc = 99; break; case 3: kc = 100; break; case 9: case 6: kc = 108; break; case 8: case 4: kc = 109; break; }
        if (kc) key(kc, ev.type == SDL_JOYBUTTONDOWN);
      } else if (ev.type == SDL_CONTROLLERDEVICEADDED) SDL_GameControllerOpen(ev.cdevice.which);
    }
    cloud_drain(); /* completa cloud ops pendentes ANTES do timeout (10s) -> sem crash */
    { extern void opensles_shim_pump_callbacks(void); opensles_shim_pump_callbacks(); }
    /* ^ pump dos callbacks de audio: a thread OGG (musica) espera o callback de buffer
     *   consumido p/ enfileirar mais. Sem ALooper_pollAll (nao usamos), ela so enfileira 1x
     *   e a musica fica muda. Pump por frame mantem a musica fluindo. */
    /* DIAG: dump g_AndroidKeyCode (traducao android keycode -> vk GameMaker) p/ confirmar X=52->vk88 */
    if (f == 300 && getenv("KZ_KCDUMP")) { extern void *text_base;
      int *tbl = (int *)((char *)text_base + 0x48c9eb4);
      fprintf(stderr, "[kcdump] g_AndroidKeyCode:");
      for (int k = 19; k <= 56; k++) fprintf(stderr, " [%d]=%d", k, tbl[k]);
      fprintf(stderr, "\n"); }
    /* Process(env, clazz, w, h, p2, p3, fa, fb, fc, fd) */
    ((void (*)(void *, void *, int, int, int, int, float, float, float, float))process)(
        fake_env, fake_thiz, W, H, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);

    if (io_update) io_update(); /* avanca pressed/released DEPOIS do Process ler (prev=current p/ proximo frame) */

    /* destrava o gate Netflix: alguns frames apos ShowUI, posta access-gained */
    if (g_showui_called && !getenv("KZ_NONFX")) {
      static long ui_frame = -1; if (ui_frame < 0) ui_frame = f;
      long d = f - ui_frame;
      static const char *evts[] = {
        "Nfx_onPlayerAccessRequest", "Nfx_onPlayerAccessChanged",
        "Nfx_onNetflixUIHidden", "Nfx_onNetflixUiHidden",
        "Nfx_onNetflixUIVisible", "Nfx_onNetflixUiVisible" };
      (void)evts;
      /* concede acesso: request+changed+UIhidden, algumas vezes, depois PARA (SDK fica READY) */
      if (d == 10 || d == 40 || d == 80) {
        post_social_event("Nfx_onPlayerAccessRequest");
        post_social_event("Nfx_onPlayerAccessChanged");
        post_social_event("Nfx_onNetflixUiHidden");
      }
    }
    if (getenv("KZ_TESTCLEAR")) { glClearColor(1.0f, 0.0f, 0.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT); }
    /* screenshot confiavel via glReadPixels (fb0 ssh degrada). KZ_SHOTEVERY=N frames. */
    { const char *se = getenv("KZ_SHOTEVERY");
      if (se && f > 0 && f % atol(se) == 0) {
        int dw = W, dh = H; SDL_GL_GetDrawableSize(g_win, &dw, &dh);
        unsigned char *px = malloc((size_t)dw * dh * 4);
        if (px) { glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px);
          char path[64]; snprintf(path, sizeof(path), "/tmp/kz_shot.raw");
          FILE *fp = fopen(path, "wb"); if (fp) { fwrite(&dw, 4, 1, fp); fwrite(&dh, 4, 1, fp); fwrite(px, (size_t)dw * dh * 4, 1, fp); fclose(fp); }
          fprintf(stderr, "[shot] frame %ld %dx%d -> %s\n", f, dw, dh, path); free(px); } } }
    if (f % 30 == 0) fprintf(stderr, "[loop] frame %ld glErr=0x%x\n", f, glGetError());
    SDL_GL_SwapWindow(g_win);
    if (maxf && f >= maxf) { fprintf(stderr, "[drv] KZ_MAXFRAMES atingido\n"); break; }
    if (maxs && (SDL_GetTicks() - t0) / 1000 >= (Uint32)maxs) { fprintf(stderr, "[drv] KZ_MAXSECONDS atingido\n"); break; }
    if (g_quit) { fprintf(stderr, "[drv] SIGTERM -> teardown\n"); break; }
  }
  kz_teardown();
}
