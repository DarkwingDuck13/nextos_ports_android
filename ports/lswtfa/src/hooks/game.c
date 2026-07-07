/* game.c -- binary patches on libProject_Douglas_HH.so (symbol-based)
 *
 * Everything is resolved by exported symbol (the .so keeps its full C++ dynsym),
 * so these patches are version-independent. Two patches for bring-up:
 *   - fnaFMV_Open/Finished/Close redirected to our skip-cutscene stubs,
 *   - the 0.75 scene render-scale constant bumped to 1.0 (full-res scene),
 *     found by scanning fnaRender_Init's body for the fmov immediate.
 *
 * The UILoading::PreLoadShadersDone level-load busy-wait handover is handled
 * separately once we reach in-level loading (its 12-byte getter is too small
 * to trampoline over).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "../util.h"
#include "../so_util.h"
#include "../hooks.h"
#include "../fmv.h"

extern so_module game_mod; // defined in main.c

// scan a symbol's body for a 32-bit instruction word and patch it
static int patch_insn_in_symbol(const char *sym, uint32_t find, uint32_t repl,
                                size_t max_words) {
  uintptr_t base = so_try_find_addr(&game_mod, sym); // writable load_base mirror
  if (!base) {
    debugPrintf("patch_game: symbol %s not found\n", sym);
    return 0;
  }
  uint32_t *w = (uint32_t *)base;
  for (size_t i = 0; i < max_words; i++) {
    if (w[i] == find) {
      w[i] = repl;
      debugPrintf("patch_game: %s+0x%zx  0x%08x -> 0x%08x\n", sym, i * 4, find, repl);
      return 1;
    }
  }
  debugPrintf("patch_game: pattern 0x%08x not found in %s\n", find, sym);
  return 0;
}

// ---------------------------------------------------------------------------
// interposição por GOT: símbolos DEFINIDOS no .so mas chamados via PLT
// (preemptíveis) podem ser desviados escrevendo o wrapper no slot do GOT.
// O wrapper chama o endereço real (dynsym) -- sem trampolim.
// ---------------------------------------------------------------------------
static uintptr_t got_interpose(const char *symname, uintptr_t wrapper) {
  so_module *mod = &game_mod;
  uintptr_t real = so_try_find_addr_rx(mod, symname);
  if (!real) { debugPrintf("interpose: %s nao encontrado\n", symname); return 0; }
  int patched = 0;
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.plt") != 0 && strcmp(sh_name, ".rela.dyn") != 0)
      continue;
    Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    for (size_t j = 0; j < mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela); j++) {
      int type = ELF64_R_TYPE(rels[j].r_info);
      if (type != R_AARCH64_JUMP_SLOT) continue;
      Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
      const char *name = mod->dynstrtab + sym->st_name;
      if (strcmp(name, symname) != 0) continue;
      uintptr_t *slot = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
      *slot = wrapper;
      patched++;
    }
  }
  debugPrintf("interpose: %s slots=%d real=%p\n", symname, patched, (void *)real);
  return patched ? real : 0;
}

/* GameLoopModule::LoadPostWorldLoad faz BUSY-WAIT apertado em
 * UILoading::PreLoadShadersDone() -- gira `while(!PreLoadShadersDone());` na
 * thread de LOAD do engine. Essa thread segura o contexto GL de contexto-único
 * e nunca o solta enquanto gira, então a thread que compila os shaders de
 * preload nunca pega o contexto -> a flag nunca vira 1 -> DEADLOCK (loading
 * trava pra sempre). Interpondo o getter (chamado via PLT), cedemos o contexto
 * GL a cada giro: park solta o contexto se esta thread o tem, o compilador
 * pega, compila, e a flag finalmente vira 1. usleep evita fritar a CPU. */
static unsigned char (*real_PreLoadShadersDone)(void);
static unsigned char wrap_PreLoadShadersDone(void) {
  unsigned char r = real_PreLoadShadersDone();
  if (!r) {
    egl_gl_ownership_park();
    usleep(2000);
  }
  return r;
}

void patch_game(void) {
  // cutscene stubs: redirect the engine's fnaFMV_* to our skip player (fmv.c)
  hook_arm64(so_try_find_addr(&game_mod, "_Z11fnaFMV_OpenPKcbPK18fnaFMVTRACKCHANNELjS0_"),
             (uintptr_t)&fmv_hook_open);
  hook_arm64(so_try_find_addr(&game_mod, "_Z15fnaFMV_FinishedP9FMVHANDLE"),
             (uintptr_t)&fmv_hook_finished);
  hook_arm64(so_try_find_addr(&game_mod, "_Z12fnaFMV_CloseP9FMVHANDLE"),
             (uintptr_t)&fmv_hook_close);
  debugPrintf("patch_game: hooked fnaFMV_Open/Finished/Close (skip cutscenes)\n");

  // full-resolution 3D scene: fnaRender_Init sets the scene colour buffer to
  // screen * 0.75 (fmov v0.2s,#0.75 = 0x0f03f500). Bump to 1.0 (0x0f03f600).
  patch_insn_in_symbol("_Z14fnaRender_InitP12fnFUSIONINIT",
                       0x0f03f500u, 0x0f03f600u, 0x32c / 4);

  // quebra o deadlock do busy-wait de compilação de shaders no load da fase
  real_PreLoadShadersDone = (void *)got_interpose(
      "_ZN9UILoading18PreLoadShadersDoneEv", (uintptr_t)&wrap_PreLoadShadersDone);
}
