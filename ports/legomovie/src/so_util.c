/* so_util.c -- load and hook .so modules (ELF32/ARM, Linux/mmap)
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * 32-bit ARM variant: the image is placed into an RWX mmap region supplied by
 * the caller (load_base == load_virtbase). Relocations are REL (Elf32_Rel, no
 * r_addend -- the addend lives in-place at the target word). R_ARM_* types,
 * ARM/Thumb trampolines.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

#include "so_util.h"
#include "util.h"
#include "error.h"

#ifndef R_ARM_ABS32
#define R_ARM_ABS32     2
#endif
#ifndef R_ARM_GLOB_DAT
#define R_ARM_GLOB_DAT  21
#endif
#ifndef R_ARM_JUMP_SLOT
#define R_ARM_JUMP_SLOT 22
#endif
#ifndef R_ARM_RELATIVE
#define R_ARM_RELATIVE  23
#endif

static so_module *so_list = NULL;

void hook_arm(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;
  if (addr & 1) {
    // Thumb-2: LDR.W PC, [PC, #0] ; .word dst
    uint16_t *hook = (uint16_t *)(addr & ~1u);
    hook[0] = 0xf8df;
    hook[1] = 0xf000;
    *(uint32_t *)(hook + 2) = (uint32_t)dst;
  } else {
    // ARM: LDR PC, [PC, #-4] ; .word dst
    uint32_t *hook = (uint32_t *)addr;
    hook[0] = 0xe51ff004u;
    hook[1] = (uint32_t)dst;
  }
}

void so_flush_caches(so_module *mod) {
  __builtin___clear_cache((char *)mod->load_virtbase,
                          (char *)mod->load_virtbase + mod->load_size);
}

void so_free_temp(so_module *mod) {
  free(mod->so_base);
  mod->so_base = NULL;
}

void so_finalize(so_module *mod) {
  (void)mod; // image already lives in an RWX region
}

int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
  int res = 0;

  memset(mod, 0, sizeof(*mod));
  strncpy(mod->name, filename, sizeof(mod->name) - 1);

  FILE *fd = fopen(filename, "rb");
  if (fd == NULL)
    return -1;

  fseek(fd, 0, SEEK_END);
  mod->so_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);

  mod->so_base = malloc(mod->so_size);
  if (!mod->so_base) { fclose(fd); return -2; }

  if (fread(mod->so_base, mod->so_size, 1, fd) != 1) {
    fclose(fd); free(mod->so_base); mod->so_base = NULL; return -2;
  }
  fclose(fd);

  if (memcmp(mod->so_base, ELFMAG, SELFMAG) != 0) { res = -1; goto err_free_so; }

  mod->elf_hdr = (Elf32_Ehdr *)mod->so_base;
  mod->prog_hdr = (Elf32_Phdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_phoff);
  mod->sec_hdr = (Elf32_Shdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_shoff);
  mod->shstrtab = (char *)((uintptr_t)mod->so_base + mod->sec_hdr[mod->elf_hdr->e_shstrndx].sh_offset);

  if (mod->elf_hdr->e_phnum > SO_MAX_SEGMENTS * 2) {
    debugPrintf("so_load: %s has too many program headers (%d)\n", filename, mod->elf_hdr->e_phnum);
    res = -4; goto err_free_so;
  }

  mod->phnum = mod->elf_hdr->e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf32_Phdr));

  mod->load_size = 0;
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    if (mod->prog_hdr[i].p_type == PT_LOAD) {
      const size_t seg_end = mod->prog_hdr[i].p_vaddr + mod->prog_hdr[i].p_memsz;
      if (seg_end > mod->load_size)
        mod->load_size = seg_end;
    }
  }

  mod->load_size = ALIGN_MEM(mod->load_size, 0x1000);
  if (mod->load_size > max_size) { res = -3; goto err_free_so; }

  mod->load_base = base;
  mod->load_virtbase = base;
  if (!mod->load_base) { res = -2; goto err_free_so; }
  memset(mod->load_base, 0, mod->load_size);

  debugPrintf("%s: load base = %p, size = %u KB\n", filename, mod->load_virtbase,
              (unsigned)(mod->load_size / 1024));

  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    Elf32_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type == PT_LOAD) {
      memcpy((void *)((uintptr_t)mod->load_base + p->p_vaddr),
             (void *)((uintptr_t)mod->so_base + p->p_offset),
             p->p_filesz);
    }
    p->p_vaddr += (Elf32_Addr)(uintptr_t)mod->load_virtbase;
  }

  mod->syms = NULL;
  mod->dynstrtab = NULL;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      mod->syms = (Elf32_Sym *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      mod->num_syms = mod->sec_hdr[i].sh_size / sizeof(Elf32_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      mod->dynstrtab = (char *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    }
  }

  if (mod->syms == NULL || mod->dynstrtab == NULL) { res = -2; goto err_free_so; }

  mod->next = NULL;
  if (!so_list) {
    so_list = mod;
  } else {
    so_module *m = so_list;
    while (m->next) m = m->next;
    m->next = mod;
  }
  return 0;

err_free_so:
  free(mod->so_base);
  mod->so_base = NULL;
  return res;
}

int so_relocate(so_module *mod) {
  int deferred = 0;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rel.dyn") == 0 || strcmp(sh_name, ".rel.plt") == 0) {
      Elf32_Rel *rels = (Elf32_Rel *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (size_t j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf32_Rel); j++) {
        uint32_t *ptr = (uint32_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf32_Sym *sym = &mod->syms[ELF32_R_SYM(rels[j].r_info)];
        int type = ELF32_R_TYPE(rels[j].r_info);

        switch (type) {
          case R_ARM_RELATIVE:
            // in-place addend + load bias (link base is 0)
            *ptr += (uint32_t)(uintptr_t)mod->load_virtbase;
            break;
          case R_ARM_ABS32:
            if (sym->st_shndx == SHN_UNDEF) {
              deferred++; // leave in-place addend for so_resolve
            } else {
              *ptr += (uint32_t)((uintptr_t)mod->load_virtbase + sym->st_value);
            }
            break;
          case R_ARM_GLOB_DAT:
          case R_ARM_JUMP_SLOT:
            if (sym->st_shndx != SHN_UNDEF)
              *ptr = (uint32_t)((uintptr_t)mod->load_virtbase + sym->st_value);
            else
              deferred++;
            break;
          default:
            debugPrintf("%s: unknown reloc type %d\n", mod->name, type);
            break;
        }
      }
    }
  }
  if (deferred)
    debugPrintf("%s: deferred %d imports to resolve\n", mod->name, deferred);
  return 0;
}

static uintptr_t so_lookup_export(so_module *mod, const char *name) {
  for (int i = 0; i < mod->num_syms; i++) {
    if (mod->syms[i].st_shndx == SHN_UNDEF) continue;
    if (ELF32_ST_BIND(mod->syms[i].st_info) == STB_LOCAL) continue;
    const char *sname = mod->dynstrtab + mod->syms[i].st_name;
    if (sname[0] == name[0] && strcmp(sname, name) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

static uintptr_t so_resolve_symbol(so_module *mod, DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int k = 0; k < num_funcs; k++)
    if (strcmp(name, funcs[k].symbol) == 0)
      return funcs[k].func;
  for (so_module *m = so_list; m; m = m->next) {
    if (m == mod) continue;
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr) return addr;
  }
  return 0;
}

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports) {
  int missing = 0;
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rel.dyn") == 0 || strcmp(sh_name, ".rel.plt") == 0) {
      Elf32_Rel *rels = (Elf32_Rel *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (size_t j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf32_Rel); j++) {
        uint32_t *ptr = (uint32_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf32_Sym *sym = &mod->syms[ELF32_R_SYM(rels[j].r_info)];
        int type = ELF32_R_TYPE(rels[j].r_info);

        if (sym->st_shndx != SHN_UNDEF) continue;
        if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT) continue;

        char *name = mod->dynstrtab + sym->st_name;
        uintptr_t addr = so_resolve_symbol(mod, funcs, num_funcs, name);
        if (addr) {
          if (type == R_ARM_ABS32)
            *ptr += (uint32_t)addr;            // + in-place addend
          else
            *ptr = (uint32_t)addr;             // GLOB_DAT / JUMP_SLOT
        } else {
          missing++;
          debugPrintf("%s: unresolved import: %s\n", mod->name, name);
          if (taint_missing_imports)
            *ptr = rels[j].r_offset;
        }
      }
    }
  }
  if (missing)
    debugPrintf("%s: %d unresolved imports\n", mod->name, missing);
  return 0;
}

void so_execute_init_array(so_module *mod) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") == 0) {
      void (**init_array)(void) = (void *)((uintptr_t)mod->load_virtbase + mod->sec_hdr[i].sh_addr);
      for (size_t j = 0; j < mod->sec_hdr[i].sh_size / 4; j++) {
        uintptr_t f = (uintptr_t)init_array[j];
        if (f != 0 && f != (uintptr_t)-1)
          init_array[j]();
      }
    }
  }
}

uintptr_t so_try_find_addr(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_base + mod->syms[i].st_value;
  }
  return 0;
}

uintptr_t so_find_addr(so_module *mod, const char *symbol) {
  const uintptr_t addr = so_try_find_addr(mod, symbol);
  if (!addr) fatal_error("Error: could not find symbol:\n%s\n", symbol);
  return addr;
}

uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
  const uintptr_t addr = so_try_find_addr_rx(mod, symbol);
  if (!addr) fatal_error("Error: could not find symbol:\n%s\n", symbol);
  return addr;
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(so_module *mod) {
  if (mod->load_base == NULL) return -1;
  if (mod->so_base) so_free_temp(mod);
  if (so_list == mod) {
    so_list = mod->next;
  } else {
    for (so_module *m = so_list; m; m = m->next)
      if (m->next == mod) { m->next = mod->next; break; }
  }
  return 0;
}

struct so_dl_phdr_info {
  Elf32_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf32_Phdr *dlpi_phdr;
  Elf32_Half dlpi_phnum;
};

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  for (so_module *mod = so_list; mod; mod = mod->next) {
    struct so_dl_phdr_info info;
    info.dlpi_addr = (Elf32_Addr)(uintptr_t)mod->load_virtbase;
    info.dlpi_name = mod->name;
    info.dlpi_phdr = mod->phdr;
    info.dlpi_phnum = mod->phnum;
    ret = callback(&info, sizeof(info), data);
    if (ret) break;
  }
  return ret;
}
