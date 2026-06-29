# DuckTales: Remastered → Mali-450 — STATUS / HANDOFF

# ═══════════════════════════════════════════════════════════════════
# 2026-06-29 — PAUSA / HANDOFF — foco atual: flicker preto entre logos e gameplay
# ═══════════════════════════════════════════════════════════════════
## Estado no device
- Device ativo: `root@192.168.31.154` (EMUELEC/NextOS aarch64, Mali-450).
- Frontend foi confirmado ativo apos os testes.
- Launcher no device usa o bootstrap que chega no Amazon: `DUCK_FORCE_GAMESTATE=Amazon`,
  `DUCK_FORCE_GAMESTATE_FRAME=330`, `DUCK_TEXMIRROR_ANY=1`, `DUCK_ETC1RGBA=1`,
  `DUCK_MVPASS=1`, `DUCK_MVPASS_ALL=1`, `DUCK_MVPASS_AFTER=850`,
  `DUCK_MVPASS_FORCESTATE=1`, `DUCK_MVPASS_DROPWHITE=1`,
  `DUCK_MVPASS_REPLACE=1`, `DUCK_MVPASS_KEEPBLEND=1`.

## Resultado visual/diagnostico confirmado hoje
- O problema principal relatado pelo NextOS e real: a tela pisca preto desde as
  logos ate gameplay. As capturas internas via `glReadPixels` confirmam que alguns
  frames chegam ao `eglSwapBuffers` com framebuffer 0% nao-preto, entao nao e
  apenas captura externa/TV.
- Run sem `hold-black`: alterna frames bons e pretos. Exemplo do log de 75s:
  frames 900/1020/1080/1320/1380/1440 com `non-black=86%`, mas frames
  360/420/480/540/600/660/720/960/1140/1200/1260/1500 com `non-black=0%`.
- O Disney splash ainda renderiza certo em frames bons (`/tmp/duck_hold_cpu_best.png`
  no host mostrou logo Disney em fundo branco).
- O ultimo frame do teste com `hold-black` ainda saiu preto (`/tmp/duck_hold_cpu_last.png`),
  e o log manteve `frame 360 non-black=0%`. Portanto o remendo de hold texture
  NAO resolveu e nao deve ser usado como default.

## Codigo deixado salvo
- `src/egl_shim.c` contem instrumentacao de screenshot robusta (`/tmp/duck_shot.ppm`,
  `/tmp/duck_best.ppm`, `/tmp/duck_menubest.ppm`) e a tentativa de hold de frame,
  mas agora o hold e opt-in: ligar apenas com `DUCK_HOLD_BLACKFRAMES=1`.
- O build local compila antes da pausa. O ultimo build validado antes da inversao
  do default rodou no device; apos esta nota, rebuildar e redeployar antes de retomar
  para garantir que o device tambem fique sem hold por default.

## Proximo caminho recomendado
- Investigar como bug nativo de apresentacao/estado GL, nao como asset faltando:
  `eglSwapBuffers` pode estar apresentando frames onde FBO0 foi limpo/descartado,
  ou o swap ocorre em ponto intermediario do frame.
- Proximos testes leves:
  1. Logar estado no momento do swap: FBO atual, viewport, scissor, color mask,
     clear color, current program, blend/depth/stencil e erro GL antes/depois.
  2. Interceptar `glClear`, `glBindFramebuffer`, `glViewport`, `glScissor`,
     `glColorMask` sempre, nao so com `DUCK_GLFBLOG`, para achar quem zera FBO0.
  3. Comparar runs com `DUCK_NO_HOLD_BLACKFRAMES=1` antigo/sem hold, `RE4_NO_PRESERVE=1`,
     `DUCK_ALLWIN=1` e log de surface atual. O `eglSurfaceAttrib(PRESERVED)` retorna OK,
     mas o padrao ainda parece descarte/alternancia de buffer.
  4. Se o preto vier de `glClear` legitimo durante transicao, filtrar apresentacao
     de frames vazios precisa ser feito sem travar progresso: melhor detectar draw-count
     de frame completo ou apresentar no fim real do frame, nao apenas desenhar hold.

# ═══════════════════════════════════════════════════════════════════
# ✅ 2026-06-24 (sessão 5) — UAF CARACTERIZADO + VIRADA: O FUNDO É DESENHADO
# ═══════════════════════════════════════════════════════════════════
## 🎯 ESTADO VISUAL CONFIRMADO (screenshot /tmp/duck_shot.png via glReadPixels):
## MENU PRINCIPAL RENDERIZA LINDO — logo DuckTales:Remastered colorido (gradiente
## laranja→amarelo + sombra), NEW GAME/OPTIONS/EXTRAS amarelos, © 1989 2018 Disney,
## botão "?" redondo. FALTA SÓ o fundo animado de Duckburg (preto). Disney splash
## (pré-corrupção) renderiza 100% (logo cinza em branco).
##
## 🟢🟢 VIRADA (DUCK_GLTEXLOG+DUCK_DRAWLOG, instrumentação LEVE preserva o bug):
## O FUNDO É DESENHADO! As texturas ETC1 do fundo (ifmt=0x8d64: 1024x512, 1024x256,
## 512x512...) SOBEM OK (err=0x0) E SÃO DESENHADAS (milhares de draws grandes/frame:
## big>=512 chega a 7478, classe 1024 domina). ⟹ o preto NÃO é dado de textura
## faltando/corrompido — é COMPOSITING/STATE/OVERLAY. Algo desenha preto POR CIMA do
## fundo já renderizado (bate com bisseção do handoff "draws 45-50 escurecem"). 
## PISTA: existe 1 textura 1024x1024 GL_ALPHA (0x1906) com nonblack=0% = ALPHA TODA
## ZERO. Se o fill do fundo amostra alpha dela → alpha=0 → fundo invisível. MAS
## DUCK_SHADERFIX (força gl_FragData=vec4(cor.xyz,1.0), alpha=1) NÃO trouxe o fundo →
## não é puramente alpha=0 do fill; investigar FBO-composite OU quad preto full-screen.
## ➡️ PRÓXIMO MAIS PROMISSOR (GL-level, pode NÃO precisar do fix do UAF): achar o draw
## que pinta preto sobre o fundo. DUCK_DRAWDBG/DRAWSTOP=N bissecta o frame (já existem).
## Checar: (a) fundo vai pra um FBO mal-compositado? (b) quad preto full-screen depois
## do fundo? (c) blend/alpha do composite. O fundo renderiza UMA vez no offscreen e some.
##
## 🔬 UAF CARACTERIZADO COM PRECISÃO (DUCK_HIST, ASLR off):
## EXATAMENTE 13 nós de free-list corrompidos em UM ÚNICO BURST no menu-load (entre
## frame 751 e 800), por 2 threads worker. DEPOIS do burst: ZERO UAF. Vítimas
## DETERMINÍSTICAS sob instrumentação leve: 1ª SEMPRE 0x3186fe40, todas na low-arena
## (0x3186/0x43e8/0x44055/0x442b6/0x4517f). Assinatura: next=nil, prev=self (bins de 1
## elemento self-circular com next ZERADO); 1 caso prev=outro (multi-elem).
## ✅ Disasm CONFIRMOU lista CIRCULAR (insert 0x939d20: bin vazio→self-circular
## streq r1,[r1]+streq r1,[r1,#4]). Logo next=nil É corrupção real; my_unlink correto.
## Fault do crash original = head-unlink 0x939dd8 (str ip,[next+4], next=NULL).
## 🔑 O WRITER escreve 0 LITERAL no offset 0 (node[0]/next). NÃO é o destrutor 0x8d3164
## (esse escreve VTABLE em [r4], não 0). HIPÓTESE FORTE: UAF-READ de um ponteiro stale
## de memória liberada + escreve 0 através dele → explica por que a vítima MUDA sob
## perturbação (conteúdo do bloco liberado é layout-dependente) E por que bionic-OK
## (glibc deixa lixo diferente no bloco liberado). Vítima 0x3186fe40 "no alloc record"
## = é nó INTERNO do arena TLSF (carved de 1 malloc grande), não malloc individual.
##
## ❌ DESCARTADO HOJE (não repetir):
## - DOUBLE-FREE no TLSF (DUCK_DFDETECT): só falso-positivo (mesmo chunk 0x8b0b8e4
##   reciclado 12×, NÃO casa com vítimas). NÃO é double-free → é UAF-write genuíno.
## - LOW ARENA como causa (DUCK_NO_LOWHEAP=1): 11 UAFs persistem + crash. Não é a causa.
## - BQ = quarentena do free TLSF (DUCK_BQ=1, novo enable): CRASH cedo (quebra alocador).
##   Confirma handoff. Dead end.
## - HW WATCHPOINT via ptrace: EIO (kernel/CPU sem debug-regs). CONFIRMADO indisponível.
##   probe: tools/hwwp_probe.c. gdb também não conseguia — é o device, não o gdb.
## - mprotect WATCHPOINT in-process (DUCK_WP e novo DUCK_WP_ONFREE=arm-no-free): HEISENBUG.
##   Proteger a página HOT muda o timing das 2 threads → a vítima 0x3186fe40 é RE-LINKADA
##   com ponteiros válidos (0x3186febc/fe6e), não zerada → o write-0 vai pra outro lugar.
##   Pegou writes de ptr válido pc=duck+0x80e120 (não-zero). Página-protect é incompatível
##   com esse bug multi-thread. DEAD END (4ª confirmação).
## - SHADERFIX (alpha=1): fundo segue preto.
## - ID ESTÁTICA do writer: inviável. Loop de destrutores está em gap de 2.8MB SEM
##   símbolo (entre __cxa_current_exception_type 0x6100b9 e png_set_sig_bytes 0x93c2e8).
##
## 🧰 REGRA NOVA (importante p/ caçar): instrumentação LEVE (hash table: DUCK_HIST/
## ATRACK/DFDETECT) PRESERVA o bug (vítima estável 0x3186fe40). Instrumentação PESADA
## (page-protect, breakpoint, single-step) MUDA o bug (vítima migra). ⟹ qualquer caça
## ao writer que pare/proteja threads tem risco de heisenbug. Métodos que SÓ leem/contam
## são seguros.
##
## 🆕 MUDANÇAS DE CÓDIGO HOJE (todas opt-in, defaults INALTERADOS — manter, são diag):
## - DUCK_BQ: liga a quarentena de free TLSF (g_bq antes era inalcançável).
## - DUCK_WP_ONFREE: arma o mprotect-WP lazy quando a vítima é liberada self-circular
##   (wp_check_onfree() chamado no pool_free_wrap). [resultado: heisenbug]
## - hist_put adicionado ao at_alloc_wrap (HeapAlloc 0x82bd78) p/ registrar allocs de
##   alto nível também.
## - ducktales escreve /tmp/duck_base.txt ("<base> <size> <pid>") ao subir → p/ tracer
##   ptrace externo resolver offsets (load base era 0xeacf8000 com ASLR off).
## - HOST: duckdis.py (disasm capstone ARM/Thumb por file-offset; VMA==file off, .text
##   em 0x2232e0), tools/hwwp_probe.c.
##
## ➡️ PRÓXIMA SESSÃO — 2 caminhos, ORDEM RECOMENDADA:
## (A) ⭐ COMPOSITING (mais promissor, GL-level, LEVE, pode dispensar o fix do UAF):
##     o fundo é desenhado e some. Bissectar o frame do menu (DUCK_DRAWDBG/DRAWSTOP)
##     p/ achar o draw/estado que apaga o fundo. Olhar FBO-composite e quad preto.
## (B) WRITER do UAF via TRACER PTRACE SEPARADO (base em /tmp/duck_base.txt pronta;
##     HW-WP fora). Mas single-step/breakpoint = heisenbug provável. Caminho (A) primeiro.

# ═══════════════════════════════════════════════════════════════════
# ✅ 2026-06-24 (sessão 4) — RENDER DIAGNOSTICADO + FIX DE SAMPLER + TOOLING
# ═══════════════════════════════════════════════════════════════════
## 🎯 BREAKTHROUGH: a tela da DISNEY (splash texturizada) que era PRETA agora
## RENDERIZA (logo cinza). Achei e corrigi um bug REAL de sampler GFx.
##
## 🔬 SCREENSHOT CONFIÁVEL (essencial p/ tudo): /dev/fb0 retorna 0 bytes enquanto
## o Mali está com o cliente GL -> capturar via glReadPixels DENTRO do binário.
## DUCK_SHOT=1 (DUCK_SHOTEVERY=N): a cada N frames dumpa /tmp/duck_shot.ppm +
## /tmp/duck_best.ppm (maior %não-preto) + /tmp/duck_menubest.ppm (melhor frame
## VARIADO com frame>600). Loga non-black% e flat% (dominância de 1 cor = intro).
## egl_shim.c egl_shim_maybe_shot(). PIL no host pra PNG. ⚠️ /tmp é tmpfs 416M:
## limpar (rm /tmp/*.raw /tmp/*.ppm /tmp/cores/*) senão escreve 0 bytes.
##
## 🟢 FIX GENUÍNO — DUCK_FIXSAMPLERS (default ON em main_ducktales.c):
## RAIZ: o engine NUNCA chama glUniform1i (confirmado, 0 chamadas) -> os samplers
## dos shaders GFx ficam no DEFAULT unit 0. O shader de fill texturizado lê COR de
## g_textureSampler e ALPHA de g_textureSamplerA; o engine liga a textura de cor e a
## textura GL_ALPHA às units 0/1 em QUALQUER ordem. Quando a textura GL_ALPHA (0x1906)
## cai na unit 0, g_textureSampler (default 0) amostra ALPHA -> RGB=(0,0,0)=PRETO.
## FIX: antes de cada draw, aponto g_textureSampler p/ a unit que tem a textura
## NÃO-alpha (cor) e g_textureSamplerA p/ a unit GL_ALPHA; g_lightSampler->2.
## Hooks: glUseProgram(track prog), glActiveTexture/glBindTexture (track unit->tex),
## glTexImage2D/glCompressedTexImage2D (track fmt por tex). fix_samplers() seta via
## glUniform1i com locs cacheados por programa. ✅ Disney splash passou de PRETO->cinza.
##
## 🔴 MAS menu Duckburg bg + GAMEPLAY SEGUEM PRETOS. NÃO é sampler (a maioria dos
## draws do menu já tem cor em ambas units, colorUnit=0) nem shader (compila OK) nem
## textura (sobem com dados válidos, nonblack alto) nem mipmap (tudo LINEAR) nem
## stencil (8 bits presentes; forçar always-pass não mudou) nem tint (passthrough do
## shader não revelou). O bg do menu/gameplay simplesmente NÃO É DESENHADO direito —
## gated pelo MESMO UAF de heap do menu-load/level-load (corrompe o display-list do
## clip Scaleform). Disney splash renderiza pq é PRÉ-corrupção. Bisseção com
## DUCK_DRAWSTOP=N mostrou: backdrop AZUL (0,0,84) + logo + menu desenham (draws ~40),
## depois draws ~45-50 escurecem (no estado warm). Determinístico PRETO agora (16+ runs;
## o "50% renderiza" do status antigo NÃO reproduz mais).
##
## 🧰 UAF — agora DETERMINÍSTICO p/ caçar: com ASLR off (echo 0 >
## /proc/sys/kernel/randomize_va_space) o 1º nó-vítima é SEMPRE 0x3186fe40 (next
## zerado, na LOW ARENA minha 0x30000000). HW watchpoint do gdb NÃO existe no kernel
## 3.14 (ptrace sem debug-regs); SW watchpoint single-steptudo (lento demais p/
## chegar ao menu-load). mprotect-watchpoint (DUCK_WP=0x3186fe40 DUCK_WPFRAME=N) bate
## mas a PÁGINA é HOT (STM/STREX/VSTR de objetos vivos) -> emular todos os stores é
## inviável e o writer-alvo nunca isolado. ➡️ PRÓX p/ GAMEPLAY: o muro é o UAF (mesmo
## das sessões 1-3). Caminhos: (a) emular TODOS os tipos de store no mprotect-WP p/
## pegar o writer 1×; (b) achar o write estático desassemblando o loop de destrutores
## GFx (capstone ARM no host: ~/...; .text VMA==file off, base 0x2232e0); (c) RE do
## caminho que deixa o clip do bg fora do display-list.
##
## 🧱 s4 cont. — POR QUE o catch dinâmico do UAF travou (3 vias fechadas) + a saída:
## (1) gdb HW watchpoint: kernel 3.14 sem debug-regs no ptrace ("Unable to determine
## number of hw watchpoints"). (2) perf_event_open HW breakpoint: ENOSYS (kernel SEM
## CONFIG_PERF_EVENTS — probe /tmp/perfbp_probe confirma). (3) /proc/self/mem write numa
## página PROT_READ: REJEITADO nesse kernel (pwrite ret=-1, probe /tmp/memwr_probe) → o
## truque FOLL_FORCE não funciona aqui. (4) mprotect-watchpoint COMPLETO implementado
## (emula STR/STRB/STRH/STRD/STM/STREX*/VSTR/VSTM via unprotect→write→reprotect+spinlock,
## DUCK_WP=0xADDR DUCK_WPFRAME=N) MAS: a instrumentação (thread de arm + proteção) MUDA o
## layout do heap → o endereço determinístico do build HIST (0x3186fe40) NÃO é mais a
## vítima no build WP → HEISENBUG. Armou e nada bateu no alvo; a corrupção rolou em outro
## endereço. ⚠️ A corrupção é detectada em 2 threads (workers) — multi-thread.
##
## ✅ A SAÍDA CERTA = TRACER PTRACE SEPARADO (próxima sessão): um processo tracer NÃO
## toca a memória do alvo (não adiciona thread/alloc) → layout determinístico preservado →
## vítima continua 0x3186fe40. Plano: fork+exec ducktales sob PTRACE_TRACEME (+ASLR off +
## PTRACE_O_TRACECLONE p/ seguir as 10 threads); breakpoint (troca instr por trap) na ENTRADA
## da função do loop de destrutores (text_base+0x8d30xx, a que contém 0x8d3164); quando bate,
## PTRACE_SINGLESTEP NAQUELA thread lendo 0x3186fe40 (PTRACE_PEEKDATA) após cada passo; quando
## vira 0 → PTRACE_GETREGS dá o PC do writer. ptrace single-step É suportado (gdb usa). Achar
## a base do .so em runtime: /proc/pid/maps (carregado pelo so_loader via mmap, não dlopen;
## com ASLR off é fixo, foi 0xeafce000). Alternativa estática: desassemblar as chamadas
## virtuais do destrutor 0x8d3164 (já desassemblado: seta vtable [r4], free membro [this+0x14c]
## via 0x92a538, blx [vtbl+0x34]) com capstone ARM no host até achar o str que zera o nó liberado.
##
## 🎮 navegação CONFIRMADA na TV: NEW GAME -> tela de dificuldade EASY/MEDIUM/HARD
## aparece (vpad). Após dificuldade -> preto (cutscene/level black, frames avançam =
## não trava, renderiza preto). vpad.py agora PERSISTENTE em /roms/ports/ducktales/
## (tmpfs apaga /tmp/vpad.py no reboot). Harnesses no device: run_test.sh, run_play2.sh
## (entra no jogo via vpad), run_goldhunt.sh, run_play.sh. Flags diag novas:
## DUCK_SHOT/SHOTEVERY, DUCK_GLSHLOG (shader src/compile), DUCK_GLTEXLOG (+nonblack do
## upload), DUCK_DRAWLOG/DRAWSTOP/DRAWDBG (tracer/bisseção de draws c/ unit+fmt+prog),
## DUCK_NOMIP, DUCK_NOSTENCIL, DUCK_SHADERFIX, DUCK_U1ILOG, DUCK_FIXDBG, DUCK_WP/WPFRAME.
## Device .165 (IP muda no DHCP). Binário tem fix ON, logging tudo OFF por default.


# ═══════════════════════════════════════════════════════════════════
# ✅ 2026-06-24 (sessão 3 cont.) — GAMEPLAY ENTRA + FUNDOS (root achado)
# ═══════════════════════════════════════════════════════════════════
## 🎮 CONTROLE FUNCIONA / ENTRA NO GAME (confirmado na TV: "o A do
## controle entrou no game"). FIX (default ON no binário, sem env):
## 1. `hasJoystick()` JNI agora retorna 1 (era 0 → o jogo tratava como teclado e
##    IGNORAVA todo key de gamepad: handled=0, e nunca engatava o path Nv).
##    jni_shim.c CallIntMethodV. DUCK_NO_HASJOY=1 reverte.
## 2. `NvGetGamepadButtons` (0x4c0034) REIMPLEMENTADO: o stub antigo retornava
##    count=0 (nenhum botão registrado → pad não registra → keys dropadas). Agora
##    retorna a lista de CAPABILITIES = keycodes Android de gamepad {19-23 dpad,
##    96,97,99,100 ABXY, 102-105 LR, 106-110 thumb/start/select/mode}. Contrato
##    (RE): chama Java gamepadButtonIndices()[I via JNI (fake falha) → retorna int*
##    + *count; buffer freed pelo caller via operator delete[] (=glibc free → malloc
##    OK). DUCK_NV0BASED=1 = índices 0-based (fallback). main_ducktales.c nv_get_buttons.
## 3. android_shim abre TODOS os controllers (não só o 1º) → pad físico + virtual.
## ⌨️ TESTE REMOTO: /tmp/vpad.py (uinput Xbox360 ctypes puro, cria js1). run_gp.sh
##    no device dá A/Start automático. Confirmado: keycode 96=A chega handled=1.

## 🖼️ FUNDOS PRETOS — ROOT ACHADO = GFxShaders.cache CORROMPIDO.
## O menu/título/fundo Duckburg é um Scaleform GFx MOVIE (TitleScrn, wfScaleformManager
## ::LoadMovie). UI (texto/logo) renderiza; o FUNDO (bitmap fills) ficava preto.
## Investigação (DUCK_GLTEXLOG/DUCK_GLFBLOG): texturas SOBEM OK (22 RGBA ≤1024² +
## 400 ETC1 0x8D64 que o Mali-450 SUPORTA, err=0x0 em todas; SEM FBO; ≤2048²). NÃO é
## tamanho/formato/FBO. 🔑 **Deletar GFxShaders.cache → o fundo RENDERIZA (azul
## Duckburg, 89.5% non-black, /tmp/nocache.png).** O cache de shaders do Scaleform
## é ESCRITO CORROMPIDO quando o UAF de heap do menu-load bate na compilação do
## shader → todo launch seguinte carrega o shader ruim → fundo preto. ⚠️ É ~50%
## (timing/layout): o cache fresco renderiza o fundo SÓ quando a corrupção não bate
## na compilação naquele run. FIX no launcher: `rm -f GFxShaders.cache` a cada
## launch (DuckTales.sh; DUCK_KEEP_SHADERCACHE=1 mantém) → fundo aparece quando a
## compilação sai limpa. CONFIÁVEL = precisa do fix do root UAF (writer).
## 📌 logos da intro (green/white) agora renderizam FULLSCREEN com cache fresco.

## ➡️ PRÓXIMO p/ FUNDO 100% CONFIÁVEL + GAMEPLAY SEM CRASH: achar o WRITER do UAF.
## A vítima = bloco LIVRE self-circular do TLSF GFx (next zerado off0), no LOOP de
## destrutores GFx 0x8d30f0/0x8d3164/0x8d32b0 (libera N objs; um write stale zera
## offset0 de um já-liberado). 0x92a538 = free de membro (this+332), NÃO é o writer.
## Precisa HW watchpoint (perf_event_open system-wide root, OU gdb watch cond ==0 no
## nó self-circular). Jogo agora ESTÁVEL (containment) → dá pra caçar sem morrer.
## ⚠️ captura fb0 via ssh DEGRADA após muitos launches (cat→0 bytes); validar na TV
## ou rebootar (EmuELEC: `reboot` às vezes não pega). Device .165.

# ═══════════════════════════════════════════════════════════════════
# ✅ 2026-06-24 (sessão 3) — CRASH DO MENU RESOLVIDO (menu confiável)
# ═══════════════════════════════════════════════════════════════════
## RESULTADO: o crash de menu-load (era 100% fresh / ~50% warm) NÃO acontece mais.
3/3 runs limpos, INCLUSIVE 1 run em device RECÉM-REBOOTADO (a condição que antes
dava 12/12 crash determinístico). Menu renderiza estável (Disney DuckTales
Remastered + NEW GAME/OPTIONS/EXTRAS), frames passam de 1255+ sem travar.

## CAUSA-RAIZ FINAL (corrigida) — a CASCATA, não só o write:
1. O "crash" NÃO era deref selvagem: é `raise(SIGSEGV)` DELIBERADO do engine
   (sequência gettid svc 0xe0 + getpid + tgkill svc 0x10c, sinal 11, pc dentro
   da libc no retorno do tgkill). Os registradores/fault-addr do dump eram LIXO
   residual (red herring — não confundir com memcpy(NULL,..,11), aquilo é benigno
   n=0). O engine tem um integrity-check da free-list que dispara o raise.
2. A corrupção REAL: um UAF zera o ponteiro `next` (offset 0) de um bloco LIVRE
   da free-list circular do TLSF do GFx (vítima = nó self-circular `next=node,
   prev=node`, vira `next=0,prev=node`). Confirmado com DUCK_HIST (alloc-history).
   A vítima fica na low-arena (0x31xx/0x4528xxxx). Acontece no LOOP de destrutores
   do GFx em `duck+0x8d3164/0x8d32b0/0x8d3300/0x92a564` (libera N objetos no
   menu-load). NÃO é race (persiste 100% serializado) — é platform-specific
   (bionic OK com o MESMO .so) = bug latente do engine exposto pelo layout glibc.
3. 🔑 O FIX que destravou: o NOSSO `my_unlink` (hook 0x939d84) TOLERAVA o nó
   corrompido mas PROPAGAVA o lixo — fazia `prev->next = next(=0)`, zerando o
   `next` de um vizinho VÁLIDO → cria a próxima vítima → cascata até o engine
   detectar e dar raise (o crash via 0x86ecd8/0x835654). NOVA política em
   my_unlink/my_insert: quando QUALQUER link é inválido, NUNCA escrever lixo num
   vizinho — COLAPSA o bin para um único sobrevivente válido (self-circular) ou
   esvazia, e põe o nó-corpse num self-loop benigno. Cada bin fica internamente
   consistente → o walk do integrity-check passa → SEM raise. (Blocos extras de
   um bin >2 vazam, limitado, só no menu-load.) Também alinhei o clamp de
   size-class com o real (sc==0 → bin 31). DEFAULT ON (sem env). Verificação:
   `grep -c "contained"` ~15/run, `grep -c "CRASH sig"` = 0.

## ⚠️ AINDA FALTA (próximos): 
- **FUNDO do menu PRETO** (real = Duckburg animado). É a MESMA corrupção: os ~15
  blocos contidos/vazados provavelmente incluem dados da imagem de fundo. A CURA
  REAL (achar o WRITER do UAF e impedir o write) restauraria o fundo. Agora o
  jogo fica ESTÁVEL (sem crash) → dá pra caçar o writer com gdb/HW-watchpoint sem
  o jogo morrer. O subsistema é o loop de destrutores 0x8d3xxx do GFx.
- gameplay (NEW GAME via input), áudio, frame-pacing das logos.

## 🎮 INPUT/GAMEPLAY (investigado s3 — falta plumbing):
- ✅ INFRA DE INPUT FUNCIONA: criei /tmp/vpad.py (uinput Xbox360 ctypes puro, sem
  evdev — VID 045e:028e p/ SDL mapear; cria js1 "Microsoft X-Box 360 pad"). MUDEI
  android_shim init_gamecontroller p/ abrir TODOS os controllers (não só o 1º) —
  default ON, melhor comportamento. Confirmado no log: abre o pad físico (id=0
  "Twin USB PS2 Adapter") E o vpad (id=1 "Xbox 360 Controller"); minhas batidas
  CHEGAM no android_shim ("button DOWN keycode=96"=BUTTON_A, 108=START).
- ❌ MAS O MENU NÃO RESPONDE ao gamepad. CAUSA: DuckTales lê gamepad pelo HELPER
  NVIDIA `NvGetGamepadButtons/Axes` (JNI), que a gente STUBOU p/ retornar 0 botões
  (main_ducktales.c:98 nv_gamepad_enum_stub) — o caminho AInputQueue (key events)
  que o android_shim alimenta NÃO é o que o menu usa. O jogo poll'a hasJoystick
  direto. ➡️ PRÓX p/ gameplay: RE do contrato de NvGetGamepadButtons (retorno =
  ponteiro p/ array de estados de botão? + count via int&; achar o MAPEAMENTO de
  índice→botão no CALLER) e religar com estado real do SDL (SDL_GameControllerGetButton).
  Símbolos: _Z19NvGetGamepadButtonsP7_JNIEnvP8_jobjectRi / _Z16NvGetGamepadAxes...
- ✅ DUCK_NORAISE=1 (opt-in, NOVO enable — antes era código morto): suprime o
  raise(SIGSEGV) deliberado do engine (via import raise/abort + my_syscall tgkill +
  crash_handler si_code<=0). Com a CONTENÇÃO nova (lista consistente) NÃO trava mais
  (rodou a 2230 frames sem crash/freeze, ao contrário do freeze-1339 de antes SEM
  contenção). Bom como rede de segurança p/ o level-load (que ainda dispara o raise
  ~50% via 0x939ad0/0x938908). Mantido OPT-IN (band-aid; cura real = fix do UAF).
- run_gp.sh no device = harness (mata jogo+vpad, vpad em bg, screenshots, jogo fg).

## Flags diag desta sessão: DUCK_HIST=1 (loga [UAF] nó+backtrace de alloc da
## vítima), DUCK_HEAPSLACK=N (pad TLSF chunk — NÃO ajudou, é UAF não overflow),
## DUCK_MEMGUARD=1 (memcpy guard — só pegou n=0 benignos).

# ═══════════════════════════════════════════════════════════════════
# 🟢 HANDOFF ANTERIOR (sessão 2) — contexto histórico
# ═══════════════════════════════════════════════════════════════════

## OBJETIVO
Port DuckTales: Remastered (WayForward + **Scaleform GFx** + FMOD, NDK r17c armv7,
so-loader) rodar JOGÁVEL no Mali-450. Falta: **menu confiável (sem crash) → gameplay
→ áudio → imagens (fundo animado de Duckburg)**.

## ONDE ESTAMOS (já funciona)
WayForward logo → título → **MENU PRINCIPAL renderiza** (NEW GAME/OPTIONS/EXTRAS),
~55fps. Boot inteiro renderiza. **Config vencedora = DEFAULT (sem env especial).**

## ⛔ ÚNICO BLOQUEIO: crash no MENU-LOAD (~frame 750), ~50% das vezes
Device FRESCO (pós-reboot, rápido) = 12/12 crash determinístico; device WARM = ~50%.

## 🎯 CAUSA-RAIZ EXATA (achada no gdb, 1 run — NÃO precisa mil runs):
É um **USE-AFTER-FREE no Scaleform GFx menu-load**. O engine escreve **0 nos primeiros
4 bytes (o ponteiro `next`, offset 0) de um bloco JÁ LIBERADO** que está na free-list
circular do alocador GFx. gdb comprovou: no nó corrompido só `[node+0]` (next) = 0; o
`prev` (`[node+4]`) e `[node+8]`/`[node+0xc]` INTACTOS → escrita pontual de 4 bytes, NÃO
memset. Depois o unlink (`0x939d84`/fault em `0x939dd8`: `str rX,[next+4]` com next=NULL)
crasha. Path do overflower (ATRACK, determinístico): funções LOCAIS Scaleform
`0x769e50`/`0x7718b0`/`0x76da50`/`0x771ed8` (loader do .gfx do menu = o fundo animado
de Duckburg). Os nós corrompidos ficam na low-arena (0x45xxxxxx).

## ➡️ PRÓXIMO PASSO (o jeito de RESOLVER, via gdb 1-run — PROMETIDO):
Achar QUEM escreve o 0 em `[freed_block+0]` (o writer do UAF). Os endereços dos blocos
VARIAM por run (timing das 10 threads → ordem de alloc varia; ASLR já está off=0 mas não
basta). Estratégias p/ pegar o writer em 1 run:
  (a) gdb HW watchpoint: rodar com DUCK_NO_SAFEUNLINK=1, no crash pegar o nó (r1); OU
  (b) MELHOR: um detector UAF em software — no `pool_free_wrap` (hook do bucket free
      0x938bec) gravar cada bloco liberado + checksum dos 8 bytes dos links; antes de
      cada free/unlink, varrer os blocos liberados recentes e, se os links mudaram sem
      re-alloc, LOGAR o bloco + backtrace (df_backtrace já existe) = pega o contexto do
      writer. (c) Ou hookar `memset`/o ponteiro: o writer escreve 4 bytes=0 em offset 0.
Depois de achar o writer (uma função do Scaleform que usa um ponteiro stale), o fix real
= corrigir esse caminho (provável race de 2 jobs do menu-load compartilhando um recurso
Scaleform; OU serializar ESSE recurso específico).

## ⚠️ REGRA DO PORTER: usar **gdb (1 run captura o erro)**, NÃO rodar o jogo mil vezes
pra "medir confiabilidade". Achar a causa → corrigir → verificar 1x. (Loops de teste
repetidos são indesejados — cada launch mostra as logos na TV.)

## 📱 ORÁCULO: o Moto G100 (adb 0074124494) roda o jogo REAL (mesmo .so v7a, bionic):
menu = **fundo ANIMADO de Duckburg** (cidade, montanhas, ponte; ver /tmp/ph_25.png). O
nosso = fundo PRETO. Confirma: o decode/render da imagem de fundo está quebrado pela
MESMA corrupção. Nota do porter: "resolver as imagens destrava tudo" (provavelmente certo — é o
mesmo UAF no menu-load). Logos da intro passam RÁPIDO DEMAIS (~0.5s vs ~2.5s no real) =
possível problema de frame-pacing/delta-time A INVESTIGAR depois.

## FIXES GENUÍNOS JÁ FEITOS (default ON, mantêm o alocador robusto, ~0%→50%):
- `my_unlink`/`my_insert` (hooks 0x939d84/0x939d20): free-list circular NULL-safe.
- `ALLOCREC` (crash handler): fault no código do alocador (0x938800..0x93a000) → pula
  a instrução in-place (load→zera dest, store→dropa).
- single-arena (mallopt M_ARENA_MAX=1+M_MMAP_MAX=0) + low-arena bump + shims malloc/
  calloc/realloc/free/memalign (tudo addr baixo, paridade bionic) + heaplock (6 pontos
  do alocador sob 1 mutex recursivo g_heap_mtx) + egl creator-window (render na thread
  criadora) + inflate-lock + texlock (serializa wfPNGTexture::Init/DecompressUpdate).

## ❌ BAND-AIDS QUE FALHARAM (NÃO repetir):
NORAISE default (FREEZE ~frame 1339), free-guard (freeze, vaza lock interno GFx),
quarantine/dedup/bucket-quarantine (quebram coalescing), thread-reduction (deadlock
coordenador), job-serial (deadlock), glibc-only (crash mais cedo), bucket-size-slack
(quebra o alocador), image-isolation (sem efeito no crash).

## COMO RODAR / GDB (device .165 agora — IP MUDA no reboot via DHCP; ping-sweep
<device-ip> + checar `ls /roms/ports/ducktales`; senha <senha>; ASLR off:
`echo 0 >/proc/sys/kernel/randomize_va_space`):
```
cd /roms/ports/ducktales; systemctl stop emustation; killall -9 ducktales
DUCK_LIBDIR=lib DUCK_ASSETS=assets DUCK_DATADIR=userdata RE4_GAMEDIR=$PWD \
  RE4_USERDATA=$PWD/userdata RE4_NO_SEMBREAK=1 DUCK_MAXSECONDS=40 ./ducktales
# gdb (device TEM gdb 17.2, SEM python): /tmp/w.gdb no host é o template. Base via
# `x/1xw &g_main_text` (símbolo do NOSSO binário não-stripado). Resolver offsets vs
# base com objdump -T + c++filt (script python no host, ver histórico).
```
Build: `./build.sh` (toolchain Amlogic-old armhf, igual RE4). Binário NÃO versionar.
Screenshot: `cat /dev/fb0 > x.raw` + PIL RGBA→RGB (BGRA swap), 1280x720.
Input p/ testar gameplay: uinput Xbox360 (/tmp/vpad.py, VID 045e:028e = SDL mapeia);
NÃO escrever em /dev/input/eventN (evdev ignora). Pad físico USB já em js0/event2.

# ═══════════════════════════════════════════════════════════════════

**Data:** 2026-06-23  •  **Device:** Mali-450 EmuELEC (IP muda; senha <senha>), fbdev 1280x720
**Pasta device:** `/roms/ports/ducktales` (dados) + `/roms/ports_scripts/DuckTales.sh` (launcher) — partição EEROMS (mmcblk1p3, 111G).
**⚠️ NUNCA gravar em `/storage` (mmcblk1p2, partição de sistema, 2.3G) — só em `/roms` (=`/storage/roms`, partição grande).**

## ⏩ ATUALIZAÇÃO 2026-06-23 (sessão 2) — AVANÇO: EGL + MAIN LOOP ALCANÇADOS
**Marco:** com `DUCK_DFSKIP=1` o jogo agora passa do load, **cria contexto EGL
(eglCreateContext/MakeCurrent OK), entra no android_main e no MAIN LOOP** (polling
hasJoystick, controller detectado). Crash residual move para uma init de singleton
(`0x4fc108`/`0x4f9cb0`, registra chave ofuscada) — não mais no asset-load.

**Causa-raiz do DFSKIP ter destravado:** a entrada REAL do `free` do alocador TLSF
é **`0x938bec`** (faz as ops de bitmap das free-lists e só no fim chama o coalesce
`0x939e04`). A sessão 1 hookava `0x939e04` (tarde demais — bitmap já corrompido).
Agora `DUCK_DFSKIP`/`DUCK_POOLLOCK` hookam `0x938bec` (chunk=r1). Call-sites únicos:
free←0x915d88, malloc(0x9392a4)←0x9166f8, coalesce←0x938d54 → **alocador 100%
serializável; POOLLOCK serializa tudo e MESMO ASSIM corrompe → NÃO É RACE.**

**Diagnóstico definitivo (gdb no device, pega o fault REAL sem o disfarce do re-raise):**
- É **BUFFER OVERFLOW DETERMINÍSTICO** corrompendo ponteiros da free-list (next/prev)
  → unlink escreve em ponteiro-lixo (ex. r0=0x18e90, ou `str r12,[r3+4]` com r3 lixo).
  Os "double-frees" da sessão 1 eram **SINTOMA** disso, não a causa.
- O engine **detecta a corrupção e dá `raise(SIGSEGV)` deliberado** (gettid svc 0xe0 +
  tgkill svc 0x10c, sinal r2=11) = assert fatal. **Por isso band-aids (DFSKIP/RECOVER)
  NÃO chegam à imagem**: mesmo evitando o fault do alocador, o engine se auto-aborta.
  **Só corrigir o overflow resolve.**
- Assinatura na memória corrompida: runs de `0x3d` ("===="). NÃO bate com conteúdo de
  asset (nenhum .pak/.fsb tem "======") → provável fill/stale, não overflow de texto.

**ORÁCULO CONFIRMADO:** o APK é **só armeabi-v7a**; roda no Moto G100 (Android 12, arm64
em modo 32-bit) com **IMAGEM + SOM** (mesmo `.so`). Prova que o bug é 100% do nosso
ambiente glibc (bionic OK). ⚠️ frida-server arm64 **NÃO injeta no processo 32-bit**
("agent connection closed") → frida-no-celular indisponível p/ este port.

**JÁ DESCARTADO como causa do overflow:** sync structs (pthread_shim usa mapa
bionic→glibc, sem overflow), `struct stat` (stat_shim traduz TODOS os campos),
setjmp→_setjmp (já estava em imports.gen.c L109-110, sem efeito), strlcpy (glibc
2.43 TEM, correto), alinhamento malloc 16B (testado; arena do engine é mmap
page-aligned, não vem do malloc import), race no alocador (100% serializado, ainda
corrompe), `__android_log_print` (resolve p/ stub_generic no-op, sem buffer).
**`__sF` (stdio): BUG REAL CORRIGIDO mas NÃO era a causa do crash.** O resolver antigo
ligava `__sF`→`dlsym("stdout")` (ERRADO: __sF é array de 3 FILE bionic; &__sF[1/2]
viravam FILE* lixo). Novo: src/stdio_shim.c dá região própria + redireciona
fprintf/fwrite/fputc/etc dentro dela p/ stderr glibc. **Mas instrumentei e o engine
faz 0 chamadas de stdio em std streams** (só em arquivos fopen reais) → não era a
fonte das escritas selvagens. Fix mantido (era latente/incorreto).
Assinatura "====" (0x3d) na arena: origem ainda não identificada (não é asset, não é
__sF via funções; pode ser fill de freed-mem do próprio TLSF = red herring).

**PRÓXIMO PASSO (campanha gdb watchpoint):** desligar ASLR
(`echo 0 >/proc/sys/kernel/randomize_va_space`; device SEM `setarch`), achar a arena
TLSF (região 0xe0xxx mmap), e watchpoint de hardware no ponteiro de free-list que
vira lixo p/ pegar o WRITER do overflow → subir até a função do engine (provável
parser de 1 asset específico OU uma função libc com ABI diferente ainda não achada).
Flags novas em jni/main: DUCK_DFSKIP, DUCK_POOLLOCK(real entries), DUCK_DFDETECT
(+backtrace do engine), DUCK_MEMGUARD, DUCK_RECOVER, DUCK_NO_ALIGN16, DUCK_FREE_OFF.

## 🎉 BREAKTHROUGH 2026-06-23 (sessão 2 cont.) — INTRO + TÍTULO RENDERIZANDO!
**WayForward logo → Disney DuckTales: Remastered title renderizam LINDOS na tela**
(~50fps, 749+ frames). A corrupção de heap de MÚLTIPLAS SESSÕES está RESOLVIDA.

**CAUSA-RAIZ (dupla) + FIX (ambos default-ON agora):**
1. **glibc per-thread malloc arenas** (mmap em 0x80000000+) — os workers alocavam o
   heap do GFx em endereços ALTOS espalhados; o GMemoryHeap do Scaleform não lida.
   FIX: `mallopt(M_ARENA_MAX,1)` + `M_MMAP_MAX,0` no início do main → arena ÚNICA
   no brk baixo. + arena própria sub-0x80000000 p/ allocs grandes (low_alloc) +
   shims malloc/calloc/realloc/free/**memalign**/posix_memalign/aligned_alloc.
   (DUCK_NO_LOWHEAP desliga.) Bionic já fazia tudo baixo → por isso funcionava lá.
2. **GFx GMemoryHeap NÃO é thread-safe** — os 10 workers corrompem a free-list.
   FIX: `DUCK_HEAPLOCK` (default ON) serializa HeapAlloc(0x82bd78)/HeapFree(0x82bb84)
   com 1 mutex recursivo global. (DUCK_NO_HEAPLOCK desliga.)
3. **Render**: o jogo renderiza NA thread CRIADORA (não há render-thread separada como
   no RE4/Unity); o egl_shim dava PBUFFER à criadora → swap pulado → frames=0.
   FIX: egl_shim g_creator_win default ON → criadora usa a WINDOW. (DUCK_NO_EGL_CREATOR_WIN desliga.)

Config vencedora = **DEFAULT** (sem env): single-arena + HEAPLOCK + creator-win.

🎉 **MENU PRINCIPAL RENDERIZANDO** (NEW GAME/OPTIONS/EXTRAS, logo, © Disney) — boot
COMPLETO: WayForward → título → menu, ~55fps. A race final era os **boundary-tags do
FreeWrap(0x915d64)/AllocWrap(0x9166bc)** rodando FORA do malloc/free travado. FIX:
travar os CORPOS de FreeWrap+AllocWrap também (fw_wrap/aw_wrap). Agora TODOS os 6
pontos do alocador (bucket malloc 0x9392a4/free 0x938bec, HeapAlloc 0x82bd78/HeapFree
0x82bb84, FreeWrap 0x915d64, AllocWrap 0x9166bc) sob 1 mutex recursivo (g_heap_mtx;
pool_lock usa ele qdo heaplock). Tudo DEFAULT-ON.

🟡 **RESIDUAL: crash ~50% no burst do menu-load (~frame 1000).** Quando sobrevive
essa janela, fica ESTÁVEL (2489+ frames). Com os 6 pontos travados NÃO há race de
alocador possível → é **overflow determinístico cujo CRASH depende do layout do heap**
(timing das threads varia o layout → 50%) OU race de dados do engine (não-alocador).
⚠️ Tentativas que FALHARAM: DUCK_DFSKIP (quebra render, frames=0), slack nas allocs
diretas (256B, sem efeito), inflar size do bucket no AllocWrap (PIOROU — quebra o
alocador, crash mais cedo), NORAISE+RECOVER (chega a 752 mas cascateia stack-fault).
PRÓX p/ confiabilidade: achar o overflow (ATRACK c/ addrs baixos achou cand de 3988B
em 0x7833020 via Scaleform 0x769e50/0x7718fc — não nomeável, é função local GFx); OU
reduzir concorrência só na fase de menu; OU red-zone nos buckets do GFx (sem quebrar).

## 🔬 CAUSA-RAIZ DO CRASH DO MENU (achada no gdb 2026-06-24) + fixes genuínos
**O crash do menu-load é um USE-AFTER-FREE/overflow CONCORRENTE do engine** que
corrompe a free-list circular do alocador GFx. gdb (device, ASLR off, símbolo
g_main_text p/ o base) pegou o fault REAL (não o re-raise): **unlink em 0x939d84
faz `next->prev = prev` com next==NULL** — numa lista circular next NUNCA é NULL,
logo um link foi ZERADO/virou lixo por um write em memória já liberada. Pontos
exatos mapeados: insert `0x939d20`, unlink `0x939d84`, free/coalesce `0x9388ec`.

**FIXES GENUÍNOS (default ON) — alocador robusto a links corrompidos:**
- `my_unlink`/`my_insert` (hooks 0x939d84/0x939d20): reimplementação fiel da
  lista circular com GUARDA de NULL/out-of-arena nos stores (su_ok). DUCK_NO_SAFEUNLINK.
- `ALLOCREC` (crash handler): qualquer fault no código do alocador (0x938800..0x93a000)
  → pula a instrução in-place (load→zera dest, store→dropa). DUCK_NO_ALLOCREC.
- Resultado: **device FRESCO 0%→~50% live** (era 12/12 crash determinístico pós-reboot).

⚠️ **DESCOBERTA sobre confiabilidade:** device FRESCO (pós-reboot, rápido) = race
SEMPRE bate (determinístico ~frame 750); device WARM (degradado por horas) = ~50%.
Confirma RACE timing-sensível. ASLR resetou p/ 2 no reboot (desligar: echo 0 >
/proc/sys/kernel/randomize_va_space p/ gdb base estável).

🔴 **O QUE FALTA:** a corrupção é severa — depois de blindar o alocador, ela vaza e
corrompe até endereços de retorno (pc=0x1650c, lr=lixo). Tolerar sintomas = whack-a-mole
(cada fix revela outro ponto). Band-aids que FALHARAM: NORAISE default (freeze ~frame
1339), free-guard (freeze, vaza lock interno GFx), quarantine/dedup/bucket-quarantine
(quebram coalescing), thread-reduction (deadlock coordenador), job-serial (deadlock).
**CURA REAL = achar QUEM faz o write-after-free** (2 jobs do menu-load — JPEG decode +
Scaleform — compartilham um ponteiro; um libera, outro escreve). Precisa watchpoint de
HW no gdb na free-list, MAS endereços variam (threads não-determinísticas, lock só
serializa, não determiniza a ordem). Próx: serializar 1 recurso compartilhado do
Scaleform, OU achar o ponteiro compartilhado por RE do caminho 0x8d3164→HeapFree.

## RESUMO EXECUTIVO
so-loader armv7 (NDK r17c, NativeActivity) **funciona e inicializa o engine WayForward por completo**:
carrega `libducktales.so` + `libfmodex.so` + `libfmodevent.so`, resolve 401 imports, roda 1013
construtores, `JNI_OnLoad`, cria contexto **GL Mali 1280x720**, sobe o job-system (10 threads) e
**carrega TODO o conteúdo do jogo** (todos os `.pak` + sound banks `.fsb`, incl. `music.fsb` 75MB).

🟡 **MARCO:** com `DUCK_SKIPFILE=music.fsb` (pula o 37º/último asset) **NÃO crasha e CHEGA AO
MAIN LOOP** (polling `hasJoystick` por frame). Mas não renderiza (frames=0): fica em retry-loop
porque o music bank é requisito antes do 1º frame.

🔴 **BLOQUEIO ATUAL:** crash por **corrupção de heap (unlink de free-list de alocador interno
custom, `node->prev=NULL`)** disparado ao carregar **com sucesso** o **37º (último) asset**
(`music.fsb`), **antes do 1º frame**. CHAVE: redirecionar music.fsb p/ um `.fsb` pequeno VÁLIDO
TAMBÉM crasha → **não é conteúdo/tamanho do music.fsb**; é o ato de completar o último load que
toca uma região de heap **já corrompida por um load anterior** (provável **buffer overflow
determinístico** em 1 dos 36 loads — crasha igual com 1, 2 e 10 threads efetivas, então não é
corrida pura). Não é bloqueio externo.

## O QUE FOI RESOLVIDO (cada um destravou um estágio)
1. **memset NULL** — tabela de imports gerada tinha todos os UND com func=0; `so_resolve` ligava 0.
   Fix: `so_util.c` trata func==0 como "não resolvido" → cai no resolvedor (dlsym/shims). 
2. **Helper de gamepad NVIDIA** (`NvGetGamepadAxes/Buttons`) faz reflexão JNI sobre JNI fake → lixo.
   Fix: hook → stub "0 eixos/0 botões" (input real vem por AInputQueue).
3. **Corrupção de heap por pthread bionic↔glibc** (mutex bionic=4B vs glibc=24B, sem bionic=4B).
   Fix: `recon_wire_pthread(dt_set_import)` wirado ANTES dos resolves (faltava ser chamado).
4. **`getDataDirectory()=""`** → `CopyFromAssetManager` copia asset→datadir, `fopen` falhava → `fwrite(NULL)`.
   Fix: jni_shim retorna userdata real p/ getDataDirectory + getDeviceLanguage="en".
5. **`fstat` layout bionic vs glibc** (st_size em offset diferente) → tamanho de arquivo lixo →
   buffer pequeno → parser FILELINK estoura. Fix: `stat_shim.c` traduz glibc→bionic (st_size@48).
6. **Deadlock do job-system** com SEMBREAK off + <10 threads. Causa: o coordenador (`wfMCP::Exec`)
   espera os ready-posts das 10 workers; reduzir threads quebra. **Precisa exatamente 10 threads.**
   Com 10 threads + `RE4_NO_SEMBREAK=1` (espera real, sem force-wake espúrio) carrega tudo.

## O BLOQUEIO (detalhe técnico)
- Backtrace converge em **`wfManagedPackage::RegisterFiles` / `wfFileSystemJob::Run` / um alocador
  em ~`0x939d84`** (file offset). Decodificado (ARM forçado): é um **unlink de free-list duplamente
  ligada** com bins por classe de tamanho — `prev->next=next; next->prev=prev` onde prev/next estão
  corrompidos (NULL ou valor pequeno ~0x3000). Clássico de heap/free-list corrompido.
- Esse alocador (malloc=`0x9392a4`, free=`0x939e04`, ambos chamam o unlink) é **bundle estático
  NÃO thread-safe** (≠ malloc glibc, que o `wfHeap::Alloc` usa). Sob 10 workers, corrompe.
- Crash é **antes de qualquer `eglSwapBuffers`** (frames=0 sempre) → tela preta, sem imagem.

### Tentativas que NÃO resolveram (registradas p/ não repetir)
- **Serializar `wfJob::Exec`** (lock global via trampolim): **deadlocka** — jobs têm dependência
  cross-thread (job A segura o lock e espera job B). Recursive lock idem. (env `DUCK_SERIAL_JOBS`)
- **Lockar o alocador interno** malloc/free (`0x9392a4`/`0x939e04`) com trampolim de 8 args
  (preserva args de pilha — as funções leem `[sp,#76]`): **não eliminou o crash** → há outro
  caminho que mexe na free-list (provável `realloc`/`malloc_consolidate`), OU a corrupção é um
  **buffer overflow** (escrita além de um buffer na metadata do chunk vizinho), não alloc concorrente.
  (env `DUCK_POOLLOCK` — opt-in, default off; usa spinlock recursivo async-signal-safe.)
- **Stack 8MB** nas threads (eram 128KB): não mudou. (env `DUCK_STACK_MB`, default 8)
- **Pular `.fsb`** (DUCK_NOFSB): ainda crasha → não é exclusivo do fmod.
- **Reduzir threads** (`DUCK_JOBTHREADS`): <10 deadlocka (coordenador espera 10).

### Dados que estreitam a causa (medidos)
- **NÃO é OOM:** peak VmRSS = 136MB, device com 558MB livres no crash. O jogo libera os buffers de
  asset após parsear (RSS baixo mesmo com music 75MB).
- **`node->prev == NULL`** no unlink é assinatura clássica de **double-free / use-after-free** no
  alocador custom (≠ glibc; glibc abortaria com "double free"). Provável: algum buffer/recurso é
  liberado 2× nesse pool durante o load (talvez no caminho wfFileReader/CopyFromAssetManager ou no
  parser de um asset). 🔎 PRÓXIMO: gdb watchpoint no campo prev do nó que corrompe, OU hookar o
  `free` do pool (`0x939e04`) p/ logar double-free (mesmo ptr liberado 2×).
- Crasha igual com 1, 2 e 10 threads efetivas (GetCpuCount forçado) → **não é corrida pura**.

## PRÓXIMOS PASSOS sugeridos (em ordem de promessa)
1. **Determinar race vs overflow determinístico:** rodar single-thread REAL até o fim do load.
   O obstáculo é o deadlock do coordenador (espera 10). → achar a contagem esperada em `wfMCP::Exec`
   (espera `sem[0]` em loop) e **patchar a contagem + o spawn juntos** p/ rodar com 1-2 threads.
   Se 1 thread carrega tudo SEM crashar → é race (seguir p/ #2). Se crasha igual → é overflow
   determinístico (achar o buffer; provável no parser FILELINK ou no manejo de evento fmod).
2. **Se race:** lockar TAMBÉM o `realloc`/`consolidate` do alocador interno (achar offsets),
   OU tornar o alocador thread-safe redirecionando suas chamadas internas. Alternativa: liberar o
   lock global de jobs durante os waits (sem_wait/wfEvent::Wait soltam o lock) → serializa o
   trabalho de CPU mas resolve dependências (padrão condvar).
3. **Se overflow:** gdb watchpoint no chunk que corrompe (achar quem escreve o ~0x3000 no campo
   prev/next), subir até a função do engine que estoura.
4. Depois da imagem: áudio (fmod já cai em `org.fmod.FMODAudioDevice`→pulse via jni_shim se
   `dlopen(libOpenSLES)` falhar), input (AInputQueue já mapeado p/ Xbox), empacotar.

## COMO RODAR (device)
```sh
ssh root@<device-ip>   # senha <senha>
cd /roms/ports/ducktales
systemctl stop emustation
DUCK_LIBDIR=lib DUCK_ASSETS=assets DUCK_DATADIR=userdata \
RE4_GAMEDIR=$PWD RE4_USERDATA=$PWD/userdata RE4_NO_SEMBREAK=1 DUCK_MAXSECONDS=30 \
./ducktales > logs/run.log 2>&1
```
Ou pelo launcher: `bash /roms/ports_scripts/DuckTales.sh` (mata-antes, watchdog, logs persistentes).

## INFRA DE SEGURANÇA (entregue, conforme pedido)
- **Watchdog anti-travamento** no binário: `DUCK_MAXSECONDS=N` força saída; detector de "render
  travado" (force-exit se frames>0 param 15s); heartbeat a cada 5s no log.
- **`ulimit -c 0`** + `setrlimit(RLIMIT_CORE,0)`: SEM core dump (já travou o device 1× enchendo
  partição). core_pattern do device = `|/bin/true` (descarta), mas garantido em 2 camadas.
- **Mata+confirma** instâncias por `/proc/*/exe` antes de cada run (launcher e runner).
- **Logs persistentes com timestamp** em `logs/`: `game_test.log` + split `crash.log`/`audio.log`/`input.log`.
- `run_dev.sh` = runner de DEV (timeout externo + watchdog + screenshot fb0).

## ARQUIVOS (host: ~/nextos_ports_android/ports/ducktales)
- `build.sh` — build armhf (toolchain RE4). `src/` — loader + shims.
- `src/main_ducktales.c` — harness (load fmod+ducktales, JNI, EGL, watchdog, hooks).
- `src/asset_shim.c` (AAssetManager→disco), `src/stat_shim.c` (bionic stat), reusados do RE4:
  `so_util.c egl_shim.c android_shim.c jni_shim.c pthread_shim.c softfp_shim.c opensles_shim.c imports.gen.c`.
- `DuckTales.sh` — launcher PortMaster. `payload/` — APK extraído (libs+assets).
- Binário `ducktales` NÃO versionado (regra). Device IP pode mudar (DHCP).

# ═══════════════════════════════════════════════════════════════════
# 🆕 2026-06-24 (sessão 6) — PORTADO PRO DEVICE .88 (NextOS pediu)
# ═══════════════════════════════════════════════════════════════════
## Deploy completo no .88: binario (md5 98a4e590, + diag stencil) + lib/armeabi-v7a/
## (libducktales+fmodex+fmodevent) + assets 561M (rsync). DUCK_LIBDIR=lib/armeabi-v7a.
## ✅ BOOTA: frames avançam (1200+, ~30fps), 32 shaders compilam (SHOK), SwapBuffers,
## audio path. stencil REAL concedido = 8 (NÃO é stencil faltando). SEM FBO (GLFBLOG=0
## binds -> jogo renderiza DIRETO no fb0, sem render-target).
## 🔴 MAS no .88 glReadPixels(back-buffer, ANTES do swap) = COR CHAPADA, menu NUNCA
## aparece (flat=100% varied=0 menubest=-1 em todos os frames). Cor muda com o swap:
## PRESERVE=verde(0,255,0), NO_PRESERVE=ciano(0,255,255) -> só o CLEAR chega ao
## back-buffer, os draws do menu não. Diferente do .165 (lá o menu renderizava).
## 🎯 HIPÓTESE FORTE: thread de render do .88 fica current no PBUFFER 16x16 (setup)
## em vez da WINDOW surface -> draws vão pro pbuffer, janela só mostra clear. (egl_shim
## linha 56: "criadora precisa usar a WINDOW senao renderiza num pbuffer"). Investigar
## o binding de surface por thread no .88 (multi-context). glReadPixels pode TAMBÉM
## enganar (memória: validar OLHANDO A TV nesses Amlogic-old) -> PENDENTE NextOS ver a TV.

## 🔑 ACHADO-CHAVE s6 (.88): ZERO DRAW CALLS
## DUCK_DRAWLOG no .88 = NENHUM [DRAW] (imprime a cada 2000 draws; no .165 eram
## milhares/frame). Logo o engine NÃO emite glDrawElements/Arrays no .88 -> só o
## glClear roda -> back-buffer flat (verde/ciano = clear). Descartados: stencil(=8 ok),
## FBO(0 binds, sem render-target), pbuffer(DUCK_ALLWIN força WIN, segue flat),
## present/PRESERVE. ⟹ NÃO é GL/present — é o PATH DE RENDER DO ENGINE que não roda
## no .88 (o worker que emite os draws não executa). Mesmo binário RENDERIZA no .165.
## 🎯 PRÓXIMO: por que o render-worker não roda no .88? Investigar wfMCP::Exec /
## wfSystem::GetCpuCount (conta de cores/barreira dos workers) — talvez .88 reporte
## cores diferente -> workers não spawnam/barram. Logar spawn de thread + GetCpuCount
## no .88 vs .165. DUCK_ALLWIN=1 (egl_shim) disponível mas NÃO foi a causa.

## s6 cont: DUCK_CPUCOUNT=1 (serial) -> ainda ZERO draws/flat. NÃO é contagem de
## worker. Independente de cpucount/pbuffer/stencil/FBO/preserve. O engine SWAPA todo
## frame (frames avançam) mas NUNCA emite geometria no .88. Hipóteses abertas p/ próx:
## (1) engine TRAVADO antes do menu (loading/splash flat; menu nunca alcançado) — vs
##     .165 onde o menu (frame ~1150) renderizava. Ver thread states no .88 (gdb/proc
##     stack) — algum worker/join bloqueado?
## (2) license gate "Error 101" disparando diferente no .88 e travando o load.
## (3) engine cacheou ponteiro de glDrawElements via dlsym ANTES do nosso hook (então
##     draws reais não passam pelo my_glDrawElements) — checar se há draws REAIS via
##     glReadPixels variado OU instrumentar eglGetProcAddress/dlsym de gl*.
## Próximo passo concreto: capturar /proc/<tid>/stack de todas as threads no .88 num
## frame "estável" p/ ver onde o engine está parado (load vs render-wait).

## 🩻 CAUSA-RAIZ s6 (.88) = DEADLOCK DO JOB-SYSTEM (classe RE4)
## /proc/<tid> no .88 num frame estável: workers em `futex_wait_queue_me` (presos em
## semáforo), GL thread em osd_wait_vsync, main R. Log spamma `[SEMBREAK] forçando wake
## b=0x... após 150ms` — nossa rede de segurança força acordar mas os JOBS DE RENDER
## NÃO COMPLETAM -> nenhum draw -> back-buffer só com clear (flat). 14 threads.
## ⟹ é o MESMO deadlock de job-system que fechou o RE4 (workers esperam post que ninguém
## faz; SEMBREAK insuficiente). DEVICE-ESPECÍFICO: no .165 o job-system NÃO trava (menu
## renderizava com milhares de draws). Timing/scheduling do .88 dispara o deadlock.
## 🎯 DECISÃO/RUMO p/ NextOS: (a) DuckTales JÁ funciona até o menu no .165 -> manter lá;
## (b) atacar o deadlock do job-system do .88 = RE profundo do wfMCP/wfJob (RE4 provou
## ser trade-off fatal/incerto). NÃO é GL — é o motor de jobs travando neste device.

## ✅✅ s6 RESOLVIDO (NextOS acertou): NÃO era deadlock de hardware — era ARQUIVO FALTANDO
## A conclusão "deadlock job-system .88" acima estava ERRADA. O .88 e o .165 são o MESMO
## device/sistema. O que faltava no .88:
##  (1) userdata/ VAZIO — faltavam os 112MB de game-data (autoload.pak, ai_beagle_boys.pak,
##      *.fsb, _wfshaderslow.pak, etc.). Sem eles o engine não tinha conteúdo p/ desenhar
##      -> 0 draws -> flat. (Eu só tinha deployado assets/, não userdata/.)
##  (2) GFxShaders.cache (golden) não deployado.
##  (3) Rodava COM SEMBREAK; o run que funciona usa RE4_NO_SEMBREAK=1 (o spam de SEMBREAK
##      era sintoma de rodar sem os dados, não a causa).
## FIX: copiei do .165 (NextOS autorizou) binário proven b7ed78d8 + userdata/ 112M +
## GFxShaders.cache + rodei com env do .165 (RE4_NO_SEMBREAK=1, DUCK_DATADIR=userdata).
## ⟹ MENU RENDERIZA NO .88 (screenshot duck_stable.png: logo Remastered + NEW GAME/
## OPTIONS/EXTRAS + © Disney + "?"). non-black 0%->14%. PARIDADE COM .165.
## FALTA (igual .165): fundo animado de Duckburg (preto). Mesma investigação da sessão 5
## (o bg É desenhado mas algo o apaga; ver bisseção DUCK_DRAWSTOP/DRAWDBG, hipótese alpha/composite).

## 🎯 s6 CARACTERIZAÇÃO PRECISA (.88, env bom + vpad New Game):
## Progressão: intro/logo Disney @frame360 = 97% NÃO-PRETO (full-screen RENDERIZA!) ->
## menu @840+ = 14% (logo+NEW GAME/OPTIONS/EXTRAS, bg Duckburg PRETO) -> New Game ->
## CUTSCENE de abertura (screenshot duck_gameplay.png: botões ⏩skip + ⏸pause renderizam,
## conteúdo do movie PRETO).
## ⟹ ISSUE UNIFICADO: SPRITES/UI estáticos RENDERIZAM (intro logo, menu logo/texto,
## botões cutscene), mas TODO conteúdo Scaleform-MOVIE sai PRETO (Duckburg do menu +
## cutscene + provavelmente cenário do gameplay). NÃO é "all rendering" (intro prova que
## full-screen funciona) — é específico do render de MOVIE/animação Scaleform.
## Sprites e movies usam paths/shaders diferentes. Sem FBO (GLFBLOG=0). bg movie É
## desenhado (tex 1024² milhares de draws) mas preto.
## 🎯 PRÓXIMO: instrumentar SÓ os draws de movie (textura 1024² tex4/tex236): logar
## program+texturas-por-unit+blend+sampler-uniforms e comparar com os draws de SPRITE
## (que funcionam). A diferença de estado revela por que o movie sai preto (suspeita s5:
## shader de fill amostra a textura GL_ALPHA all-zero como COR -> preto; fix_samplers
## pode estar errando p/ o shader específico do movie). Cutscene: vpad precisa apertar
## o ⏩ skip p/ chegar no gameplay real.

## 🔬 s6 ACHADO IMPORTANTE: shaders GFx vêm do CACHE (glProgramBinary), não glShaderSource
## DUCK_SHADERRED (força frag vermelho) NÃO pintou NADA -> os shaders do menu/movie NÃO
## passam pelo my_glShaderSource. Eles vêm PRÉ-COMPILADOS do GFxShaders.cache via
## glProgramBinary. ⟹ SHADERFIX/SHADERRED da s5 NUNCA afetaram o movie (não-conclusivos!).
## rm GFxShaders.cache no .88 -> bg AINDA preto (14%) — confirma o nondeterminismo da s5
## (rm cache renderizava só ~50%; e o cache regenera durante o run, podendo voltar ruim).
## NOTA: os shaders de logo/texto (que RENDERIZAM) também vêm do cache -> não é "cache todo
## corrompido"; é específico do shader/estado do MOVIE.
## 🎯 PRÓXIMO REAL p/ testar o shader do movie: interceptar glProgramBinary (não
## glShaderSource) — logar/substituir o programa dos draws de movie; OU forçar o engine a
## compilar do source (achar a flag GFx "no shader cache") p/ então SHADERFIX/RED valerem.
## Sem isso, SHADERFIX/RED são inúteis (não alcançam o movie). Esta é a investigação da s5
## que ficou aberta (raiz = UAF que corrompe o cache na compilação; HW-watchpoint indispon.).

## s6 BALANÇO (fundo Duckburg = muro s5, nondeterminístico):
## - DUCK_NOPROGBIN (no-op glProgramBinaryOES) NÃO quebrou o menu e NÃO trouxe o bg ->
##   inconclusivo (hook pode não ter disparado; "ProgramBinary" aparece 1x no log).
##   Resultados combinados (NOPROGBIN+SHADERRED+nocache) VARIAM entre runs = mesmo
##   nondeterminismo da s5 (UAF que corrompe o GFxShaders.cache na compilação).
## ✅ ENTREGUE no .88: MENU renderiza (logo+NEW GAME/OPTIONS/EXTRAS), gameplay/cutscene
##   ALCANÇÁVEL (botões skip/pause renderizam). Paridade c/ .165. Env bom documentado.
## 🔴 ABERTO (= s5): conteúdo Scaleform-MOVIE (Duckburg/cutscene/cenário) preto.
##   Leads p/ próxima sessão focada: (1) confirmar se glProgramBinaryOES dispara (logar no
##   import resolve, não só no hook) e se há fallback p/ source; (2) interceptar o
##   programa do MOVIE especificamente (correlacionar com textura 1024²) e dumpar/forçar
##   cor; (3) raiz real = UAF na compilação do shader GFx (s5: HW-watchpoint indisp. no
##   device) -> talvez detectar via mprotect só na região do cache durante a compilação.
## ⚙️ Flags novas no binário: DUCK_SHADERRED (frag vermelho), DUCK_NOPROGBIN (no-op
##   glProgramBinaryOES). Binário atual md5 muda; NÃO commitado.

## 🎯🔑 s6 DIAGNÓSTICO NOVO (avança s5): FUNDO = GEOMETRIA, não cor/cache/alpha
## Bug do meu teste: DUCK_SHADERRED sozinho não instalava my_glShaderSource (corrigido).
## Com SHADERRED instalado + cache apagado (source): 9 frags reescritos p/ VERMELHO SÓLIDO.
## RESULTADO (duck_red3.png): só o LOGO DuckTales ficou VERMELHO; a ÁREA DO FUNDO ficou
## PRETA (non-black=13%). ⟹ se o fundo fosse cor/alpha/cache, o shader vermelho o pintaria.
## Como NÃO pinta -> a GEOMETRIA do movie-clip do fundo NÃO RASTERIZA nos pixels do fundo
## (quads colapsados/offscreen/degenerados). É TRANSFORM/PROJEÇÃO do movie-clip Scaleform,
## NÃO o shader/textura/cache (hipóteses da s5 ESTAVAM no caminho errado).
## Nota: os draws 1024² DO acontecem (DRAWLOG 114k) mas não cobrem a área -> geometria
## com matriz errada (g_worldView/g_projection do movie-clip = 0/identidade/escala 0?).
## 🎯 PRÓXIMO: instrumentar os draws de movie (textura 1024²): logar as MATRIZES
## (g_worldView/g_projection via glUniformMatrix4fv) e as COORDS dos vértices
## (glVertexAttribPointer) -> ver se o quad do fundo está em coords válidas de tela.
## Comparar com o draw do LOGO (que rasteriza). A diferença de matriz/coords = a causa.

## 🏁 s6 DIAGNÓSTICO FINAL (super preciso — descarta TUDO da s5):
## Instrumentei o estado GL completo dos draws de MOVIE (DUCK_MVDBG, enums ES2 corretos):
##  - blend=1 RGB(SRC_ALPHA,ONE_MINUS_SRC_ALPHA) A(ONE,ZERO) eq=FUNC_ADD  -> NORMAL
##  - depth=1(LESS) scissor=0 cwm=1111 vp=[0,0,1280,720] glerr=0  -> TUDO OK
##  - g_tint=(1,1,1,1) (DUCK_MVTINT) -> tint OK; DUCK_FORCETINT branco não mudou nada
##  - DUCK_MVNOBLEND (blend off no movie) -> AINDA preto => NÃO é alpha/transparência
##  - ambas as units do movie = ETC1 0x8d64 1024² (u0=color u1=alpha), NÃO GL_ALPHA;
##    fix_samplers roteia certo (color=u0). ETC1 sobe direto pro Mali (sem decode), err=0.
## ⟹ DESCARTADOS: blend, geometria/viewport/depth/scissor/cwm, tint, alpha-routing,
##    GL_ALPHA-misroute, FBO, stencil, pbuffer, shader-cache-as-cause.
## ⟹ RESTA: a TEXTURA ETC1 de COR do movie (tex161 etc., 1024²) AMOSTRA PRETO. Candidatos:
##    (a) o DADO ETC1 dessas texturas é preto (asset/upload do .pak do userdata);
##    (b) o movie animado está parado num frame PRETO (timeline não avança no nosso port);
##    (c) issue de amostragem (texcoords/mip) dessas ETC1 grandes no Mali.
## 🎯 PRÓXIMO: ler de volta o conteúdo de tex161 (render num quad debug fullscreen
##    amostrando tex161 com shader passthrough de SOURCE) -> se aparecer arte=timeline;
##    se preto=dado/decode. Flags novas: DUCK_MVDBG/MVNOBLEND/MVTINT/FORCETINT/SHADERRED/NOPROGBIN.

## 🏁🔚 s6 RAIZ CONCLUSIVA: as TEXTURAS DE COR do movie estão PRETAS (dado, não render)
## DUCK_TEXDUMP=0 (desenha a textura REAL do movie — g_dbg_movie_tex = g_unit_tex[0] do
## último movie draw — num quad fullscreen com shader passthrough PRÓPRIO): resultado =
## 0% non-black = PRETO TOTAL. ⟹ a textura ETC1 1024² de cor do movie NÃO TEM ARTE (preta).
## Confirma que o fundo preto NÃO é GL/render-state (tudo descartado), é o CONTEÚDO da
## textura. A causa está UPSTREAM do GL:
##   (a) a arte ETC1 do .pak não está sendo carregada/decodificada nessas texturas;
##   (b) o movie ANIMADO não decodifica/preenche as texturas (playback parado) — Duckburg
##       é um movie animado; se o decoder/timeline não roda, as texturas ficam pretas;
##   (c) upload da ETC1 1024² falha silencioso no Mali (err=0 mas dado não aplicado).
## 🎯 PRÓXIMO (fora do GL): rastrear o PREENCHIMENTO dessas texturas — glCompressedTexImage2D/
##   glTexSubImage2D de tex de cor do movie tem dado não-preto? (DUCK_GLTEXLOG já loga
##   nonblack% do SubImage). Se o upload é preto -> problema no load/decode do .pak (movie
##   asset). Se nunca há upload de arte -> o movie playback/decode não roda (RE do decoder).
## ⚙️ Flags de debug no binário: DUCK_TEXDUMP(=0 auto), DUCK_MVDBG/MVNOBLEND/MVSWAP/MVTINT/
##   FORCETINT/SHADERRED/NOPROGBIN. (binário NÃO commitado; é de debug.)

## s6 ETCCHECK: decoder SW não suporta ETC1 (0x8d64, faz só ETC2) -> "decode FALHOU".
## MAS: as texturas do movie sobem com TAMANHO ETC1 CORRETO (512²=131072B, 1024²=524288B,
## =0.5B/px), err=0, e o Mali renderiza ETC1 do LOGO perfeitamente (mesma família/path).
## ⟹ EVIDÊNCIA CONCLUSIVA: o DADO ETC1 das texturas de cor do movie É PRETO (não é Mali,
## não é tamanho). A arte não chega nessas texturas. Causa = pipeline de conteúdo do movie:
##   - ou o .pak de Duckburg fornece ETC1 preto (load/decompress do pak),
##   - ou são texturas dinâmicas que o rasterizador do movie Scaleform deveria preencher e
##     não preenche (playback/decode do movie não roda no nosso port).
## 🎯 RE necessário (fora de GL): rastrear a ORIGEM do byte ETC1 dessas texturas — qual
##   função do engine chama glCompressedTexImage2D p/ elas e de onde vem o ponteiro `px`
##   (do pak? de um buffer que o movie-rasterizer deveria preencher?). Breakpoint no
##   glCompressedTexImage2D das 1024² -> backtrace -> achar o produtor do dado.
## == FIM da investigação GL-level (s6). Menu .88 ENTREGUE; fundo isolado em "textura preta
##    do movie = pipeline de conteúdo upstream". ==

## ⚠️ s6 CORREÇÃO IMPORTANTE: as texturas do movie TÊM ARTE (não são pretas!)
## DUCK_ETCCHECK com scan de BYTES CRUS da ETC1 que sobe: as texturas de COR do movie têm
## RAW nonzero=68-88% com base-colors variados (b0=af b1=5e b2=08 etc.) = ARTE REAL ETC1.
## Vêm em PARES: cor (arte) + alpha ("ff ff ff 00" = branco/máscara). ⟹ minha conclusão
## anterior "texturas pretas" estava ERRADA — o DADO tem arte.
## Mas: bg renderiza preto + MVNOBLEND preto + texdump(passthrough+LINEAR) preto.
## DUCK_NOMIP e LINEAR-no-texdump NÃO trouxeram a arte. ⟹ contradição: textura COM arte
## amostra preto. Suspeitas restantes: (a) o texdump é NÃO-CONFIÁVEL (g_dbg_movie_tex =
## textura errada/estado mudou; validar dumpando a textura do LOGO que funciona); (b) Mali
## ETC1 quirk nessas texturas; (c) algum estado de sampler/textura (wrap/base-level/
## swizzle) específico. ⚠️ resultados parcialmente NÃO-DETERMINÍSTICOS (timing).
## 🎯 PRÓXIMO confiável: validar o texdump dumpando a textura do LOGO (deve mostrar o logo);
## se mostrar -> texdump OK e o movie tex amostra preto de verdade (Mali/estado); se o logo
## tb sair preto -> texdump quebrado (refazer a sonda). Status: bg AINDA aberto; menu .88 OK.

## ✅ s6 SONDA VALIDADA + conclusão limpa (resolve a contradição):
## DUCK_TEXDUMP=0 + DUCK_TDGRAD=1 (frag=gradiente UV, sem textura) -> RENDERIZA gradiente
## (non-black=100% varied=1). ⟹ a sonda texdump FUNCIONA (quad/program/draw OK).
## Logo: quando o texdump da textura do movie deu PRETO, a textura AMOSTRA PRETO de verdade.
## MAS o raw scan provou que o DADO ETC1 tem ARTE. Reconciliação:
##   a arte é uploadada (glCompressedTexImage2D com ETC1 real), mas no MOMENTO do draw do
##   movie a textura amostrada está PRETA/VAZIA -> suspeita = TEXTURE-ID errado/reusado/
##   stale (binding ou gerência de textura no nosso loader), NÃO o dado nem render-state.
## 🎯 PRÓXIMO confiável: capturar o ID da textura NO upload da ETC1-com-arte (glCompressed)
##   e o ID amostrado no movie draw (g_unit_tex[0]) -> são o MESMO objeto? Se diferentes,
##   a arte vai pra textura A e o draw amostra textura B (vazia). Logar par (upload-id,
##   draw-id) e cruzar. Provável raiz: glGenTextures/glBindTexture/glDeleteTextures
##   mapeando errado, ou a engine recicla o ID antes do draw.
## == Sonda confiável agora; bg = problema de binding/identidade de textura. Menu .88 OK. ==

## 🎯🏁 s6 RAIZ FINAL (2 hipóteses precisas, ambas plausíveis):
## Rastreio de upload por textura (g_tex_up): o movie cicla por DEZENAS de texturas ETC1/
## frame (animação). Em runs/frames diferentes o movie draw amostra:
##   - texturas UP=1 (ETC1 com arte, raw nonzero 68-91%, base colors orange/brown=Duckburg) -> renderizam PRETO mesmo assim;
##   - às vezes texturas "NUNCA_UPLOADADA" (tex203/229) -> a textura do frame ainda não
##     terminou o upload quando foi amostrada.
## A sonda texdump É confiável (gradiente UV renderiza). Então:
## HIPÓTESE A (sync): o movie DECODIFICA/UPLOADA texturas numa worker thread e o RENDER
##   thread amostra ANTES do upload completar -> textura vazia/preta no frame. (Race
##   upload-vs-draw; bate com o job-system do engine e com o nondeterminismo.)
## HIPÓTESE B (Mali ETC1): o Mali-450 não amostra corretamente essas ETC1 grandes/non-square
##   (1024x512/256/128) apesar de err=0 e de ter arte -> precisa VERIFICAR se o logo/menu
##   (que renderiza) é ETC1 ou RGBA. Se o menu for RGBA e só o movie ETC1 -> Mali ETC1 quebrado.
## 🎯 PRÓXIMO: (1) checar o fmt das texturas do LOGO (RGBA vs ETC1) -> se RGBA, testar FIX =
##   decodificar ETC1->RGBA em software no upload (precisa escrever decoder ETC1, ~100 linhas);
##   (2) se ETC1 do logo funciona -> é race de upload (hip. A) -> serializar/sincronizar o
##   upload de textura do movie com o draw (já há [texlock] serializando wfTexture::DecompressUpdate).
## == s6 FIM: menu .88 entregue; fundo = ETC1-art-amostra-preto, 2 hipóteses (sync vs Mali-ETC1). ==

## s6 TEXFLUSH: glFinish após upload de textura -> NÃO trouxe o fundo. Não é flush
## cross-context simples. ⟹ hipótese restante mais forte = Mali-450 não amostra essas ETC1
## grandes/non-square corretamente (apesar de err=0 + arte no dado + pipeline OK).
## FIX a testar (fresco): software ETC1->RGBA decode no upload (escrever decoder ETC1
## ~100-150 linhas; o etc2_decode existente NÃO faz ETC1). Se ao subir RGBA o fundo
## aparecer = Mali-ETC1 confirmado e RESOLVIDO. Senão = race upload/draw mais profundo.
## == ENCERRAMENTO s6: menu .88 ENTREGUE+confiável. Fundo Duckburg = muro exaustivamente
##    caracterizado (descartados ~15 hipóteses), raiz provável = Mali-ETC1 grande, fix =
##    decoder ETC1 software (próxima sessão focada). ~45 ciclos build/run/fix nesta sessão. ==

## s6 ETC1RGBA: decodificar ETC1 via etc2_decode(0x9274) e subir RGBA -> NÃO funcionou
## (etc2_decode retorna NULL p/ dado ETC1; o decoder ETC2 não cobre o modo individual do
## ETC1). Fix real precisa de um decoder ETC1 DEDICADO (individual+differential mode), ~120
## linhas. ⚠️ MAS hipótese Mali-ETC1 NÃO confirmada: ETC1 PEQUENAS (logo/UI) RENDERIZAM ->
## ETC1 não está 100% quebrado. Pode ser size-específico OU race upload/draw do movie animado.
## == FIM s6 (~47 ciclos). Menu .88 ENTREGUE. Fundo = muro profundo (= s5), exaustivamente
##    caracterizado; precisa investida FRESCA: (1) confirmar logo ETC1-vs-RGBA, (2) se Mali-
##    ETC1, escrever decoder ETC1 dedicado, (3) senão, atacar sync upload/draw do movie. ==

## 🎯🎯 s6 RAIZ DEFINITIVA (teste sintético decisivo): É CONTEXTO/THREAD, NÃO Mali-ETC1
## DUCK_ETCTEST: criei+uploadei na RENDER thread uma textura ETC1 1024² SINTÉTICA branca
## (blocos ff ff ff 00...) e desenhei -> renderizou BRANCO 100%. ⟹ Mali-450 amostra ETC1
## 1024² PERFEITAMENTE. Hipótese Mali-ETC1 DESCARTADA.
## ⟹ RAIZ = COMPARTILHAMENTO DE TEXTURA ENTRE CONTEXTOS/THREADS:
##   - logo/UI: uploadados no LOAD (main/render thread) -> visíveis -> renderizam.
##   - movie (animado): texturas uploadadas em WORKER threads -> a render thread amostra a
##     SUA textura de mesmo ID (vazia) -> PRETO. (Bate com "tex203 NUNCA_UPLOADADA" e com
##     tex163 UP=1 ainda preto: o UP=1 foi setado no contexto do worker, não no da render.)
##   - egl_shim cria 1 contexto POR THREAD (share=g_real_ctx). O share group do Mali NÃO
##     está compartilhando as texturas do worker p/ a render thread (ou há race de sync).
## glFinish após upload (TEXFLUSH) NÃO resolveu -> não é só visibilidade; é o share group.
## 🎯 FIX (próxima sessão, egl_shim): garantir share REAL de texturas entre os contextos
##   das threads (worker upload <-> render sample). Opções: (a) upload de textura SEMPRE no
##   contexto da render thread (interceptar e enfileirar p/ a render thread); (b) 1 contexto
##   ÚNICO compartilhado de verdade (cuidado BAD_ACCESS); (c) re-bind/re-upload no render.
## == RAIZ FINAL ISOLADA. Não é decoder, não é render-state, não é Mali-ETC1: é SHARE de
##    textura cross-context do movie. Menu .88 entregue. ~48 ciclos build/run/fix nesta sessão. ==

## 🔚 s6 FIM DEFINITIVO: namespace de textura SEPARADO entre contextos por-thread
## DUCK_TEXMIRROR (guardar upload + re-upload no render thread por ID) NÃO disparou nenhum
## replay -> o ID que o worker uploada NÃO bate com o ID que a render amostra. ⟹ os
## contextos por-thread do egl_shim têm NAMESPACES de textura SEPARADOS (não compartilham
## nem objetos nem IDs). Por isso o movie (upload no worker) é preto e o logo (upload no
## render/load) renderiza. Mali-ETC1 OK (teste sintético branco), render-state OK.
## 🎯 FIX REAL (egl_shim, investida pesada/delicada): os contextos por-thread (tl_ctx,
##   share=g_real_ctx) NÃO estão num share group efetivo no Mali. Opções:
##   (a) UM contexto GL único usado por TODAS as threads com lock serializando (risco perf,
##       mas garante 1 namespace) — o comentário do egl_shim alerta BAD_ACCESS, validar;
##   (b) garantir que eglCreateContext use o share corretamente (testar share=g_real_ctx vs
##       o ctx da thread; conferir se o Mali honra share de textura compressed);
##   (c) mirror por (w,h,conteudo) em vez de por ID (heurística), já que os IDs não batem.
## == RAIZ CONCLUSIVA: share/namespace de textura cross-context. ~50 ciclos build/run/fix.
##    Menu .88 ENTREGUE. Fundo = fix de contexto GL (próxima sessão focada no egl_shim). ==

## 🔒 s6 CONFIRMAÇÃO IRONCLAD (DUCK_MIRLOG): namespaces desconectados, PROVADO
## MIRSTORE: 40 uploads, TODOS de 1 thread (tid e3859220); curtex()=TEXTURE_BINDING_2D
## nessa thread retorna SEMPRE id=4. Movie draws (render thread) amostram IDs 155-233.
## ⟹ os uploads (worker, namespace onde binding=4) e os samples (render, IDs 155-233)
## estão em NAMESPACES DE TEXTURA TOTALMENTE DESCONECTADOS. Raiz CONFIRMADA além de dúvida:
## os contextos por-thread do egl_shim NÃO compartilham objetos de textura no Mali-450.
## (Mirror por-ID não pode funcionar pq os IDs nem existem no mesmo namespace.)
## 🎯 FIX (egl_shim, sessão focada): garantir 1 namespace de textura entre worker e render.
##   Mais provável: UM contexto GL real único, com lock serializando as chamadas GL das
##   threads (a engine já serializa muita coisa via [texlock]/job-system). Validar BAD_ACCESS.
##   Alternativa: descobrir por que eglCreateContext(share=g_real_ctx) não compartilha no
##   blob Mali-450 (talvez precise MESMO config/precise re-mapear o handle de share).
## == RAIZ DEFINITIVA E PROVADA. Menu .88 entregue. Fundo = fix de share de contexto GL. ==

## s6 DUCK_ONECTX (1 contexto único p/ todas threads) -> CRASH sig=11 (BAD_ACCESS), como
## o egl_shim alertava: uso concorrente de 1 contexto sem serialização total quebra. ⟹ o
## acesso GL das threads do DuckTales NÃO é totalmente serializado -> não dá p/ só compartilhar
## 1 contexto. Fix precisa de UMA das duas (sessão focada, careful):
##   (A) GLOBAL GL LOCK: serializar TODA chamada GL (mutex em torno de cada gl*/egl*) +
##       1 contexto único -> garante 1 namespace SEM corrida. Custo perf, mas correto.
##   (B) SHARE GROUP REAL: descobrir por que r_createContext(share=g_real_ctx) não compartilha
##       texturas no blob Mali-450 (testar criar TODOS os contextos com share encadeado, ou
##       um config/flag específico; conferir eglQueryContext do share).
## == FIM ABSOLUTO s6 (~53 ciclos). Menu .88 ENTREGUE (flags todas gated, default=menu OK).
##    Raiz PROVADA = namespace de textura cross-context. Fix = (A) global GL lock OU (B)
##    share group real. Ambos = investida pesada/careful no egl_shim, próxima sessão. ==
