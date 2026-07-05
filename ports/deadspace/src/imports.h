#ifndef DEADSPACE_IMPORTS_H
#define DEADSPACE_IMPORTS_H

#include "so_util.h"
#include <stddef.h>

extern DynLibFunction deadspace_overrides[];
extern const int deadspace_overrides_count;

void *deadspace_raise_stub(void);
void *deadspace_abort_stub(void);
int deadspace_resolve_read_path(const char *path, char *out, size_t outsz);
int deadspace_resolve_write_path(const char *path, char *out, size_t outsz);

#endif
