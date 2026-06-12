/*
 * main_re4.c -- RECON do loader ARM32 (RE4 Unity 2018 v7a).
 *
 * FASE 0: carrega libunity.so (ELF32 ARM), relocate (39k R_ARM_RELATIVE), e
 * resolve com tabela VAZIA + taint -> imprime os imports UNRESOLVED (o contrato
 * que precisamos fornecer). NAO roda init_array nem JNI -> seguro (zero execucao
 * de codigo do jogo). So valida o loader + lista o que falta.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/mman.h>

#include "so_util.h"

FILE *stderr_fake; /* caso util/error referenciem */

#define SO_NAME "libunity.so"
#define MAP_SZ (32 * 1024 * 1024) /* libunity ~18MB + folga */

int main(void) {
  fprintf(stderr, "=== RE4 loader ARM32 -- FASE 0 (load+reloc+recon imports) ===\n");

  void *base = mmap(NULL, MAP_SZ, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  fprintf(stderr, "[re4] mmap base = %p (%d MB)\n", base, MAP_SZ / (1024 * 1024));

  int r = so_load(SO_NAME, base, MAP_SZ);
  if (r < 0) {
    fprintf(stderr, "[re4] so_load FALHOU (%d)\n", r);
    return 1;
  }
  fprintf(stderr, "[re4] so_load OK\n");

  if (so_relocate() < 0) {
    fprintf(stderr, "[re4] so_relocate FALHOU\n");
    return 1;
  }
  fprintf(stderr, "[re4] so_relocate OK (R_ARM_RELATIVE/GLOB_DAT/ABS32 internos)\n");

  fprintf(stderr, "[re4] --- imports UNRESOLVED (o contrato a fornecer) ---\n");
  so_resolve(NULL, 0, 1); /* tabela vazia -> imprime todos os imports */
  fprintf(stderr, "[re4] === FASE 0 OK: loader ARM32 carregou+relocou a engine ===\n");
  return 0;
}
