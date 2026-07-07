/* so_util.h -- utils to load and hook .so modules (ELF32/ARM port)
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * 32-bit ARM variant of the arm64 loader: Elf32_*, REL relocations (in-place
 * addend), R_ARM_* types, hook_arm (ARM+Thumb). Same so_module API as the arm64
 * version so the Fusion port code compiles unchanged.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include <stdint.h>
#include <stddef.h>
#include <elf.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

#define SO_MAX_SEGMENTS 8

typedef struct {
  char *symbol;
  uintptr_t func;
} DynLibFunction;

typedef struct so_module {
  struct so_module *next;
  char name[64];

  // entire LOAD zone
  void *load_base, *load_virtbase;
  size_t load_size;
  void *load_memrv;

  // copy of the unmodified program headers (link-time vaddrs)
  Elf32_Phdr phdr[SO_MAX_SEGMENTS * 2];
  int phnum;

  // temporary file image
  void *so_base;
  size_t so_size;

  Elf32_Ehdr *elf_hdr;
  Elf32_Phdr *prog_hdr;
  Elf32_Shdr *sec_hdr;
  Elf32_Sym *syms;
  int num_syms;
  char *shstrtab;
  char *dynstrtab;
} so_module;

extern void *text_base;
extern size_t text_size;

// ARM/Thumb trampoline (bit0 of addr selects Thumb)
void hook_arm(uintptr_t addr, uintptr_t dst);

void so_flush_caches(so_module *mod);
void so_free_temp(so_module *mod);
void so_finalize(so_module *mod);
int so_load(so_module *mod, const char *filename, void *base, size_t max_size);
int so_relocate(so_module *mod);
int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports);
void so_execute_init_array(so_module *mod);
uintptr_t so_find_addr(so_module *mod, const char *symbol);
uintptr_t so_find_addr_rx(so_module *mod, const char *symbol);
uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol);
uintptr_t so_try_find_addr(so_module *mod, const char *symbol);
DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name);
int so_unload(so_module *mod);

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data);

#endif
