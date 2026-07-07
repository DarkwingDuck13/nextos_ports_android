/* so_util.c -- load and hook .so modules (Linux/mmap port)
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * Adapted from the libnx/Switch version to plain Linux: the loaded image is
 * placed directly into an RWX mmap region supplied by the caller, so
 * load_base == load_virtbase and there is no separate code-memory mapping.
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

// not always in the host elf.h
#ifndef DT_RELR
#define DT_RELR    36
#define DT_RELRSZ  35
#define DT_RELRENT 37
#endif
#define DT_ANDROID_RELR    0x6fffe000
#define DT_ANDROID_RELRSZ  0x6fffe001
#define DT_ANDROID_RELRENT 0x6fffe003

static so_module *so_list = NULL;

void hook_arm64(uintptr_t addr, uintptr_t dst) {
  if (addr == 0)
    return;
  uint32_t *hook = (uint32_t *)addr;
  hook[0] = 0x58000051u; // LDR X17, #0x8
  hook[1] = 0xd61f0220u; // BR X17
  *(uint64_t *)(hook + 2) = dst;
}

#include <sys/mman.h>
// Detour that preserves a call-through path: overwrite `addr`'s first 4 insns
// with a jump to `dst`, and return a pointer to a trampoline that runs those
// 4 original insns then jumps back to addr+16. NOTE: only safe when none of the
// first 4 instructions are PC-relative (adr/adrp/b/bl/ldr-literal). The OBB
// package functions we instrument start with plain stp prologues, so this holds.
uintptr_t hook_arm64_tramp(uintptr_t addr) {
  if (!addr) return 0;
  uint32_t *tramp = mmap(NULL, 64, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tramp == MAP_FAILED) return 0;
  uint32_t *src = (uint32_t *)addr;
  for (int i = 0; i < 4; i++) tramp[i] = src[i];
  tramp[4] = 0x58000051u; // LDR X17, #8
  tramp[5] = 0xd61f0220u; // BR X17
  *(uint64_t *)(tramp + 6) = addr + 16;
  __builtin___clear_cache((char *)tramp, (char *)(tramp + 8));
  return (uintptr_t)tramp;
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
  // the image already lives in an RWX region; nothing to remap on Linux.
  // (kept as a hook point; patches are applied before this in patch_game.)
  (void)mod;
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
  if (!mod->so_base) {
    fclose(fd);
    return -2;
  }

  if (fread(mod->so_base, mod->so_size, 1, fd) != 1) {
    fclose(fd);
    free(mod->so_base);
    mod->so_base = NULL;
    return -2;
  }
  fclose(fd);

  if (memcmp(mod->so_base, ELFMAG, SELFMAG) != 0) {
    res = -1;
    goto err_free_so;
  }

  mod->elf_hdr = (Elf64_Ehdr *)mod->so_base;
  mod->prog_hdr = (Elf64_Phdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_phoff);
  mod->sec_hdr = (Elf64_Shdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_shoff);
  mod->shstrtab = (char *)((uintptr_t)mod->so_base + mod->sec_hdr[mod->elf_hdr->e_shstrndx].sh_offset);

  if (mod->elf_hdr->e_phnum > SO_MAX_SEGMENTS * 2) {
    debugPrintf("so_load: %s has too many program headers (%d)\n", filename, mod->elf_hdr->e_phnum);
    res = -4;
    goto err_free_so;
  }

  // save a pristine copy of the phdrs for dl_iterate_phdr
  mod->phnum = mod->elf_hdr->e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf64_Phdr));

  // total size of the LOAD zone = highest vaddr+memsz of all LOAD segments
  mod->load_size = 0;
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    if (mod->prog_hdr[i].p_type == PT_LOAD) {
      const size_t seg_end = mod->prog_hdr[i].p_vaddr + mod->prog_hdr[i].p_memsz;
      if (seg_end > mod->load_size)
        mod->load_size = seg_end;
    }
  }

  mod->load_size = ALIGN_MEM(mod->load_size, 0x1000);
  if (mod->load_size > max_size) {
    res = -3;
    goto err_free_so;
  }

  // Linux: load straight into the caller's RWX region; virtbase == base.
  mod->load_base = base;
  mod->load_virtbase = base;
  if (!mod->load_base) {
    res = -2;
    goto err_free_so;
  }
  memset(mod->load_base, 0, mod->load_size);

  debugPrintf("%s: load base = %p, size = %u KB\n", filename, mod->load_virtbase,
              (unsigned)(mod->load_size / 1024));

  // copy segments into place, then fix up the runtime phdrs
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    Elf64_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type == PT_LOAD) {
      memcpy((void *)((uintptr_t)mod->load_base + p->p_vaddr),
             (void *)((uintptr_t)mod->so_base + p->p_offset),
             p->p_filesz);
    }
    p->p_vaddr += (Elf64_Addr)mod->load_virtbase;
  }

  mod->syms = NULL;
  mod->dynstrtab = NULL;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".dynsym") == 0) {
      mod->syms = (Elf64_Sym *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      mod->num_syms = mod->sec_hdr[i].sh_size / sizeof(Elf64_Sym);
    } else if (strcmp(sh_name, ".dynstr") == 0) {
      mod->dynstrtab = (char *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    }
  }

  if (mod->syms == NULL || mod->dynstrtab == NULL) {
    res = -2;
    goto err_free_so;
  }

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

static Elf64_Xword so_dynamic_tag(so_module *mod, Elf64_Sxword tag) {
  // mod->phdr is the saved (un-rebased) copy; read PT_DYNAMIC from the file image
  for (int i = 0; i < mod->phnum; i++) {
    if (mod->phdr[i].p_type == PT_DYNAMIC) {
      const Elf64_Dyn *dyn = (const Elf64_Dyn *)((uintptr_t)mod->so_base + mod->phdr[i].p_offset);
      for (; dyn->d_tag != DT_NULL; dyn++)
        if (dyn->d_tag == tag)
          return dyn->d_un.d_val;
    }
  }
  return 0;
}

static void so_process_relr(so_module *mod, const Elf64_Xword *relr, size_t relrsz) {
  uintptr_t where = 0;
  const size_t count = relrsz / sizeof(Elf64_Xword);
  for (size_t i = 0; i < count; i++) {
    const Elf64_Xword entry = relr[i];
    if ((entry & 1) == 0) {
      where = (uintptr_t)entry;
      *(uint64_t *)((uintptr_t)mod->load_base + where) += (uint64_t)mod->load_virtbase;
      where += 8;
    } else {
      for (int bit = 1; bit < 64; bit++) {
        if (entry & (1ull << bit))
          *(uint64_t *)((uintptr_t)mod->load_base + where + (bit - 1) * 8) += (uint64_t)mod->load_virtbase;
      }
      where += 63 * 8;
    }
  }
}

int so_relocate(so_module *mod) {
  int deferred_abs64 = 0;

  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (size_t j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
            if (sym->st_shndx == SHN_UNDEF) {
              *ptr = rels[j].r_addend;
              deferred_abs64++;
            } else {
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            }
            break;
          case R_AARCH64_RELATIVE:
            *ptr = (uintptr_t)mod->load_virtbase + rels[j].r_addend;
            break;
          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
            if (sym->st_shndx != SHN_UNDEF)
              *ptr = (uintptr_t)mod->load_virtbase + sym->st_value + rels[j].r_addend;
            break;
          default:
            fatal_error("Error: unknown relocation type:\n%x\n", type);
            break;
        }
      }
    }
  }

  Elf64_Xword relr_off = so_dynamic_tag(mod, DT_RELR);
  Elf64_Xword relr_size = so_dynamic_tag(mod, DT_RELRSZ);
  if (!relr_off) {
    relr_off = so_dynamic_tag(mod, DT_ANDROID_RELR);
    relr_size = so_dynamic_tag(mod, DT_ANDROID_RELRSZ);
  }
  if (relr_off && relr_size) {
    debugPrintf("%s: processing %u bytes of RELR relocations\n", mod->name, (unsigned)relr_size);
    // relr_off is a link-time vaddr; rebase onto the loaded image
    so_process_relr(mod, (const Elf64_Xword *)((uintptr_t)mod->load_base + relr_off), relr_size);
  }

  if (deferred_abs64)
    debugPrintf("%s: deferred %d ABS64 imports\n", mod->name, deferred_abs64);

  return 0;
}

static uintptr_t so_lookup_export(so_module *mod, const char *name) {
  for (int i = 0; i < mod->num_syms; i++) {
    if (mod->syms[i].st_shndx == SHN_UNDEF)
      continue;
    if (ELF64_ST_BIND(mod->syms[i].st_info) == STB_LOCAL)
      continue;
    const char *sname = mod->dynstrtab + mod->syms[i].st_name;
    if (sname[0] == name[0] && strcmp(sname, name) == 0)
      return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
  }
  return 0;
}

static uintptr_t so_resolve_symbol(so_module *mod, DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int k = 0; k < num_funcs; k++) {
    if (strcmp(name, funcs[k].symbol) == 0)
      return funcs[k].func;
  }
  for (so_module *m = so_list; m; m = m->next) {
    if (m == mod)
      continue;
    const uintptr_t addr = so_lookup_export(m, name);
    if (addr)
      return addr;
  }
  return 0;
}

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint_missing_imports) {
  int missing = 0;
  int resolved_abs64 = 0;
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.dyn") == 0 || strcmp(sh_name, ".rela.plt") == 0) {
      Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      for (size_t j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
        uintptr_t *ptr = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
        Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];

        int type = ELF64_R_TYPE(rels[j].r_info);
        switch (type) {
          case R_AARCH64_ABS64:
          case R_AARCH64_GLOB_DAT:
          case R_AARCH64_JUMP_SLOT:
            if (sym->st_shndx == SHN_UNDEF) {
              char *name = mod->dynstrtab + sym->st_name;
              uintptr_t addr = so_resolve_symbol(mod, funcs, num_funcs, name);
              if (addr) {
                *ptr = addr + rels[j].r_addend;
                if (type == R_AARCH64_ABS64)
                  resolved_abs64++;
              } else {
                missing++;
                debugPrintf("%s: unresolved import: %s\n", mod->name, name);
                if (taint_missing_imports)
                  *ptr = rels[j].r_offset;
              }
            }
            break;
          default:
            break;
        }
      }
    }
  }

  if (missing)
    debugPrintf("%s: %d unresolved imports\n", mod->name, missing);
  if (resolved_abs64)
    debugPrintf("%s: resolved %d ABS64 imports\n", mod->name, resolved_abs64);

  return 0;
}

void so_execute_init_array(so_module *mod) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".init_array") == 0) {
      int (**init_array)() = (void *)((uintptr_t)mod->load_virtbase + mod->sec_hdr[i].sh_addr);
      for (size_t j = 0; j < mod->sec_hdr[i].sh_size / 8; j++) {
        if (init_array[j] != 0)
          init_array[j]();
      }
    }
  }
}

uintptr_t so_find_addr(so_module *mod, const char *symbol) {
  const uintptr_t addr = so_try_find_addr(mod, symbol);
  if (!addr)
    fatal_error("Error: could not find symbol:\n%s\n", symbol);
  return addr;
}

uintptr_t so_try_find_addr(so_module *mod, const char *symbol) {
  for (int i = 0; i < mod->num_syms; i++) {
    char *name = mod->dynstrtab + mod->syms[i].st_name;
    if (strcmp(name, symbol) == 0)
      return (uintptr_t)mod->load_base + mod->syms[i].st_value;
  }
  return 0;
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
  const uintptr_t addr = so_try_find_addr_rx(mod, symbol);
  if (!addr)
    fatal_error("Error: could not find symbol:\n%s\n", symbol);
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

DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name) {
  for (int i = 0; i < num_funcs; ++i)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

int so_unload(so_module *mod) {
  if (mod->load_base == NULL)
    return -1;
  if (mod->so_base)
    so_free_temp(mod);
  if (so_list == mod) {
    so_list = mod->next;
  } else {
    for (so_module *m = so_list; m; m = m->next) {
      if (m->next == mod) {
        m->next = mod->next;
        break;
      }
    }
  }
  return 0;
}

struct so_dl_phdr_info {
  Elf64_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf64_Phdr *dlpi_phdr;
  Elf64_Half dlpi_phnum;
};

int so_dl_iterate_phdr(int (*callback)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  for (so_module *mod = so_list; mod; mod = mod->next) {
    struct so_dl_phdr_info info;
    info.dlpi_addr = (Elf64_Addr)mod->load_virtbase;
    info.dlpi_name = mod->name;
    info.dlpi_phdr = mod->phdr;
    info.dlpi_phnum = mod->phnum;
    ret = callback(&info, sizeof(info), data);
    if (ret)
      break;
  }
  return ret;
}
