#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

#define LOG_NAME "debug.log"

int debugPrintf(const char *text, ...) {
  va_list list;

  FILE *f = fopen(LOG_NAME, "a");
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);
  }

  va_start(list, text);
  vprintf(text, list);
  va_end(list);

  return 0;
}

uintptr_t read_tls_stack_guard(void) {
#if defined(__aarch64__)
  uintptr_t tls = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
  if (tls)
    return *(uintptr_t *)(tls + 0x28);
#endif
  return 0;
}

const char *resolve_android_path(const char *path) {
  static _Thread_local char alt_path[2048];
  static _Thread_local char asset_path[2048];

  if (!path || path[0] == '\0')
    return path;

  /* Redireciona escritas Android para o GAMEDIR (cwd). O plandroid grava em
   * "/data/data/<pkg>/userdata/..." (savedata, fontcache). Tiramos o prefixo
   * "/data/data/" ou "/data/user/0/" + o componente de pacote -> "./<resto>". */
  const char *dd = NULL;
  if (strncmp(path, "/data/data/", 11) == 0) dd = path + 11;
  else if (strncmp(path, "/data/user/0/", 13) == 0) dd = path + 13;
  if (dd) {
    const char *slash = strchr(dd, '/');       /* pula o nome do pacote */
    const char *rest = slash ? slash + 1 : dd;
    snprintf(alt_path, sizeof(alt_path), "./%s", rest);
    return alt_path;
  }
  static const char *sdcard_prefix =
      "/storage/emulated/0/Android/data/com.snkplaymore.kof2012a/";
  if (strncmp(path, sdcard_prefix, strlen(sdcard_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s", path + strlen(sdcard_prefix));
    return alt_path;
  }

  if (access(path, F_OK) == 0)
    return path;

  if (path[0] == '/') {
    if (snprintf(alt_path, sizeof(alt_path), ".%s", path) <
            (int)sizeof(alt_path) &&
        access(alt_path, F_OK) == 0) {
      return alt_path;
    }
  }

  const char *basename = strrchr(path, '/');
  basename = basename ? basename + 1 : path;
  if (basename[0] != '\0') {
    if (snprintf(asset_path, sizeof(asset_path), "./assets/%s", basename) <
            (int)sizeof(asset_path) &&
        access(asset_path, F_OK) == 0) {
      return asset_path;
    }
  }

  return path;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
