#ifndef IMPORTS_H
#define IMPORTS_H

#include <stdint.h>

#include "so_util.h"

extern DynLibFunction dynlib_functions[];
extern const int dynlib_functions_count;
extern uintptr_t g_limbo_audio_obj;
extern uintptr_t g_limbo_audio_sem;

#endif // IMPORTS_H
