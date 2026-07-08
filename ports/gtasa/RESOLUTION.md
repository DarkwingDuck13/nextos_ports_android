# GTA SA aarch64 — Resolução de ponteiros / plano de reconstrução

Trabalho 100% local (PC), feito com `~/gta-sa-deploy/gtasa/{libGTASA.so,gtasa}` (ambos
aarch64, não-stripados) + o fork Vita `TheOfficialFloW/gtasa_vita`.

## 1. Fork Vita estudado (fonte-mãe da lógica SA)
- Repo: **TheOfficialFloW/gtasa_vita** (canônico), commit "Use vitaGL's shader cache".
- Alvo: **ARMv7 softfp** (Vita, Thumb). ⇒ offsets crus dele **não** servem no aarch64.
- Resolve hooks de 2 formas:
  - ✅ **por nome** (`so_symbol(mod,"_ZN4CPad6GetPadEi")`) → **portável** (nome mangled
    igual em armv7/aarch64). **67 hooks por-nome; 64 já existem na nossa libGTASA aarch64.**
  - ❌ **por offset cru** (`text_base + 0x003A152A + 0x1`) → ARMv7-Thumb-only; re-derivar
    no aarch64 achando a MESMA função por símbolo (libGTASA não-stripada → dá).
- Módulos Vita (linhas): main 1358, gfx_patch 1109, opengl_patch 602, jni_patch 404,
  so_util 388, config 269. → base pra portar pro Linux aarch64.

## 2. Variantes da linhagem (NÃO confundir)
| Base | Alvo | Header-tell | Serve pro device? |
|---|---|---|---|
| `gtasa_vita` (TheFloW) | Vita ARMv7 | vitasdk | fonte da LÓGICA (portar) |
| `experiments/bully/ref-bully-NX` | **Nintendo Switch** | `<switch.h>`,`sys/reent.h` | ❌ (é Switch) |
| `ports/bully/src` | **Linux aarch64 (nosso device)** | SDL2/EGL, dlopen | ✅ padrão de porte Linux |
| Yavuz `gtasa` (binário) | **Linux aarch64 R36S** | — | ✅ é o que roda liso |

⚠️ O scaffold atual em `src/` foi copiado do `ref-bully-NX` (**Switch**) e tem
`#include <switch.h>` — precisa ser re-baseado no jeito Linux (ver §4).

## 3. Superfície de ponteiros a resolver (imports da libGTASA.so) — `re/libGTASA_imports_274.txt`
**274 símbolos indefinidos** que o loader deve fornecer:
- **88 libc/pthread/m** → passthrough trivial pras libs do sistema.
- **75 GLES/EGL** → passthroughp/ `libGLESv2/libEGL` do device (+ shims Mali só-ETC1).
- **12 OpenSLES/AL** → mapear p/ OpenAL do device.
- **7 AAsset + 1 android_log** → asset_archive.c (já temos no framework).
- **~91 stubs Android/Rockstar** (o miolo): `cloud*` (saves na nuvem R*), `GetRockstarID`,
  `EnterGameFromSCFunc`, `IsProfileStatsBusy`, `hasTouchScreen`, `RTPrioLevel`,
  telemetria, Social Club. **O Vita já implementa todos** (jni_patch.c/main.c) → portar.

> 💡 Pro ONLINE (meta futura): os `cloud*`/`GetRockstarID`/`EnterGameFromSCFunc`/Social-Club
> são exatamente os pontos que o Vita **stuba** (offline). Reativá-los "de verdade" é o
> caminho de multiplayer — mapear aqui desde já.

## 4. Plano de build (Linux aarch64, glibc ≤2.30)
1. Re-basear `src/` do jeito **Linux** (partir de `ports/bully/src` p/ so_util/asset/zip/
   egl_shim/jni_shim + portar a lógica SA do `gtasa_vita`). Remover `switch.h`/vitasdk.
2. Fornecer os 274 imports (libc/GL/AL = passthrough; ~91 stubs = do Vita).
3. Hooks: 64 por-nome direto; offsets crus do Vita → re-derivar por símbolo no aarch64.
4. Config: portar parser do `config_gtasa.txt` (opções já conhecidas do estudo).
5. Compilar no **Docker buster** (glibc 2.27/2.30) — não no host (GCC16/glibc nova).
6. `objdump -T` antes de empacotar (regra glibc do repo).

## 5. Artefatos salvos (`re/`)
- `libGTASA_imports_274.txt` — a tabela de ponteiros a resolver.
- `vita_by_name_hooks.txt` — 67 hooks por-nome (64 resolvem no aarch64).
- `loader_symbol_refs_1753.txt` — todos os `_Z` referenciados pelo binário Yavuz.

## Estado
Imagem imediata = binário Yavuz (transferindo p/ device). Loader próprio = reconstruir
com §4 usando estes artefatos. **Motor RenderWare = mesmo do Bully (já fomos 1º nele).**
