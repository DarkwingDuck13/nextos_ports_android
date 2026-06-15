# Bully: Anniversary Edition — Análise Completa (2026-06-15)

Consolidação do histórico (v1→v9) + relatos dos testadores (Discord) + estudo de código/git/binários.
Objetivo: pôr em ordem o que se sabe, separar **fato** de **hipótese**, e listar o que falta.

---

## 1. Linha do tempo das versões

| Versão | Data | Esquema glibc-velha | Compilador compat | Clarity | Marco |
|---|---|---|---|---|---|
| v1/v2 | 08/jun | (Mali-450 fbdev, swap) | — | Low (engine) | 1º render no Mali-450 |
| v3 | 11/jun | runtime bundlado | — | Low | backend-agnostic (KMSDRM/wayland), fix tela-preta |
| v4 | 11/jun | runtime bundlado | — | Low | highp no kmsdrm, mipmaps, controles |
| v5 | 11/jun | runtime bundlado (glibc 2.43 + patchelf) | — | Low | SDL auto-backend |
| **v6** | 12/jun | **native + runtime glibc 2.43 bundlado** | — | **HIGH (seed settings.ini)** | **texto OK no R36S** ✅ |
| v7 final | 12/jun | **DUAL: native (≥2.38) OU compat** | **zig (clang 21)** | High via settings | abandona runtime bundlado |
| v8 / v8.1 | 13/jun | compat | zig | High | fix OOM (despejo onLowMemory) + áudio alsoft.conf; **texto some** ❌ |
| v8.3 | 14/jun | compat | zig | High | áudio/escola Mali-450 (despejo OFF + glFlush no Utgard) |
| **v9.0** | 14/jun | compat | **gcc Debian 10** | **HIGH forçado (hook GetResolutionDefault)** | extrator progressor; **texto ainda some no R36S** ❌ |
| v9.1 (WIP) | 15/jun | compat | gcc | High forçado | fix stat/lstat/fstat + VAO (limpa UNRESOLVED) — NÃO resolve o texto |

---

## 2. PROBLEMA #1 — Texto invisível  ✅ causa isolada

**Sintoma:** mundo 3D renderiza, mas todo o texto/UI (HUD, menus, legendas) some. Só em alguns devices.

**PROVA (controle RG35XXH):** o Anbernic **RG35XXH** (640×480, Mali-G31) **mostra o texto**, e é **idêntico ao R36S** em resolução e GPU. A única diferença: RG35XXH roda muOS/Knulli (glibc nova → binário **native**); R36S roda ArkOS (glibc 2.30 → binário **compat**).

**Conclusões (fato):**
- ❌ NÃO é resolução (RG35XXH é 640×480 e tem texto)
- ❌ NÃO é o Mali-G31 (RG35XXH tem o mesmo chip)
- ❌ NÃO é a Clarity RS_High (ambos High)
- ❌ NÃO é o `stat` (o fix limpou os UNRESOLVED e o texto continuou sumido)
- ✅ **É o binário `compat` ligado à glibc VELHA do device**

**Timeline bate:** v6 (native + runtime glibc 2.43 bundlado) = texto OK no R36S. v7+ (compat em glibc velha) = texto some. A v6 mascarava o problema porque o `libGame` rodava sob **glibc 2.43** mesmo no R36S.

**Causa-raiz (hipótese restante, estreita):** alguma(s) função(ões) da **glibc < ~2.38** que o `libGame` usa no caminho de render de fonte se comporta diferente da glibc nova. O `stat` era um sintoma desse grupo; há mais.

**Correções candidatas (não testadas):**
1. **Voltar ao esquema v6** p/ glibc velha: rodar o **native** com **runtime glibc 2.43 bundlado COMPLETO** (glibc + libgcc_s + libstdc++ + ld correto + multiarch paths antes de /usr/lib). Era o que funcionava. ⚠️ a v7 abandonou por erros de bundle incompleto (libgcc_s.so.1 / libmali kernel-mismatch / GLIBC_2.35 not found) — refazer completo.
2. **Caçar a(s) função(ões)** glibc-version-dependent no render de fonte e shimá-las no loader (como o `stat`). Mais cirúrgico, mas precisa achar quais.

---

## 3. PROBLEMA #2 — OOM / freeze (~30 min, ou ao entrar na escola)

**Sintoma:** em devices de 1GB, a RAM enche e o jogo congela/morre — tipicamente ao entrar na escola / 1ª missão.

**Causa-raiz:** leak de render targets / texturas DENTRO do `libGame.so` (binário fechado). O engine só libera textura de streaming quando recebe `onLowMemory` (sinal Android que o port não enviava → criava 2300+ texturas, deletava ~7).

**Mitigação (v8+):** o binário monitora MemAvailable e dispara `implOnLowMemory` ao passar de um piso (del 7→748). Anti-churn (cooldown 2s→30s se não reduz). Sem swap/zram nos devices do dev.
- Mali-450/833MB: estável.
- R36S/1GB: ainda relatam freeze na escola (copatti). O leak não é eliminável (closed binary); a mitigação só "esvazia o balde mais rápido".

**Status:** parcialmente mitigado; 1GB ainda é o limite. Pendente confirmar se o `BULLY_LOWMEM`/despejo atual segura a escola no R36S (precisa do TAIL do log com `[lowmem] disparado`).

---

## 4. PROBLEMA #3 — Áudio broken-pipe  ✅ resolvido

**Sintoma:** em devices **ALSA-only** (R36S/ArkOS, sem PulseAudio), o OpenAL cai no caminho ALSA **mmap** e dá `Broken pipe`, travando o frame loop.

**Fix (v8.1):** `alsoft.conf` com caminho **non-mmap** (device plughw, buffers maiores), só nesses devices (X5M intacto). Bundle de libopenal/libmpg123 removido (quebrava launch em glibc velha).

---

## 5. PROBLEMA #4 — Resolução baixa/pixelada em 640×480  ⚠️ separado, em aberto

**Sintoma:** devices de **640×480** (R36S, Miyoo Flip) ficam pixelados/borrados mesmo com Clarity High; 720p e 1080p ficam ótimos.

**Status:** problema **distinto** do texto. O próprio dev notou ("640x480 lose much more quality, 720p/1080p look great"). MSAA 0-4 ajuda pouco. Em aberto — é como o engine escala a UI/3D em baixa resolução. (NÃO confundir com o texto invisível.)

---

## 6. PROBLEMA #5 — Launch failures em glibc velha  ✅ evoluído

Histórico de erros e como foi resolvido:
- `GLIBC_2.34/2.35/2.38 not found` → DUAL mode (v7): native p/ glibc≥2.38, compat p/ <2.38.
- `stack smashing detected` na GameMain (Mali-G31/glibc velha) → TLS pad 256B no main.c + `__stack_chk_fail` neutralizado.
- `libgcc_s.so.1 / libstdc++ not found` (RG34XX-SP muOS) → motivo de abandonar o runtime bundlado forçado (v7).
- `libmali` kernel-mismatch (r10p6 vs r11p7, R36S) → multiarch paths antes de /usr/lib.
- `__libc_pthread_init GLIBC_PRIVATE` → idem bundle incompleto.

---

## 7. Matriz de devices (relatos)

| Device | GPU | Backend | Res | glibc | Binário | Texto | Joga? |
|---|---|---|---|---|---|---|---|
| S905X5M | Mali-G310 | kmsdrm | 1080p | nova | native | ✅ | sim (30/60fps) |
| Mali-450 (Amlogic-old) | Mali-450 | fbdev | 720p | nova | native | ✅ | sim |
| RG35XXH (Anbernic) | Mali-G31 | — | 640 | nova | **native** | ✅ | sim |
| RG34XX-SP | — | — | — | nova (2GB) | native | ✅ | sim (v8 "great") |
| RG40XX-H | — | — | — | nova | native | ✅ | sim (res baixa) |
| RGCubeXX (Knulli) | Mali-G31 | kmsdrm | 720x720 | nova | native | — | ❌ áudio sim, **vídeo preto** |
| Trimui Smart Pro | PowerVR GE8300 | — | — | nova | native | ✅ | sim (após tirar SDL_VIDEODRIVER) |
| **R36S** (ArkOS) | Mali-G31 | kmsdrm | 640 | **2.30** | **compat** | ❌ | trava na escola |
| Miyoo Flip | — | — | 640 | velha | compat | ? | pixelado |

**Padrão claro:** todo **native** (glibc nova) = texto OK. Todo **compat** (glibc velha) = texto some. RGCubeXX é um caso à parte (vídeo preto em KMSDRM, problema de present/backend, não de texto).

---

## 8. O que está descartado (não repetir)

- Texto NÃO é: resolução, Mali-G31, Clarity, MSAA, locale do sistema, `stat` sozinho, zig-vs-gcc.
- Freeze NÃO se conserta no Bully.sh (leak é interno ao libGame).
- Clarity High é MANTIDA (decisão: é proposital, bonita, funciona no native).

---

## 9. Próximos passos (ordem sugerida)

1. **Texto:** decidir entre (a) reviver o runtime glibc 2.43 bundlado completo p/ glibc velha (esquema v6, comprovado) ou (b) caçar as funções glibc-dependent e shimá-las. (a) é mais garantido; (b) mais limpo.
2. **Freeze R36S:** obter o TAIL do log com `[lowmem] disparado` p/ confirmar se o despejo segura a escola; ajustar piso se preciso.
3. **RGCubeXX vídeo preto:** caso KMSDRM separado (present/page-flip) — investigar com log `[gl]`/`ls -la /dev/dri`.
4. **Resolução 640:** estudo separado de como o engine escala UI/3D em baixa res.

---

## 10. Fatos de build (referência)

- `bully` (native): toolchain NextOS, GCC 16, GLIBC_2.38, `bash build.sh`.
- `bully.compat`: `docker run --rm -v "$PWD":/repo -v "$SYSROOT":/sysroot:ro debian:bullseye bash /repo/build_compat_gcc.sh` → GCC Debian 10, GLIBC_2.17 → copiar p/ `bully.compat`.
- Launcher escolhe por glibc (≥2.38 → native; senão → compat).
- APK: v1.4.311 (BuildID 6139a628). Dados data_0-4.zip extraídos do APK.
- Screenshot: Mali-450 fbdev = `dd /dev/fb0` (BGRA 1280x720); X5M kmsdrm = `touch /dev/shm/bully_shot` (glReadPixels).
