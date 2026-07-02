# ESTUDO — Sombras e Qualidade Gráfica do Bully (2026-07-02)

## ★ CONCLUSÃO (sessão noturna Fable 5) — a sombra diferida NÃO renderiza fora de Adreno; decisão: ESCONDER a opção do menu

**Cadeia de sombra diferida (screen-space) 100% mapeada e testada ao vivo no X5M:**
1. O passo de **resolve DESENHA** (confirmado: resolve frag shader 317 → prog 395,
   `glDrawElements` GL_TRIANGLES em fbo=2). O pipeline roda.
2. Mas o resolve escreve **zero** → a textura screen-space de sombra fica vazia →
   sombra invisível no mundo inteiro. (Readback via glReadPixels é enganoso no
   Mali tile-based: até forçar a saída p/ constante 1.0 lê 0, porque o FBO ainda
   é o DRAW target; validação real só por screenshot.)
3. **Causa-raiz (disasm):** o shader de resolve amostra `framedepth`
   (=`pp_depthstencil`, o depth-stencil da cena). Esse depth só vira textura
   **amostrável** via `RenderTarget2DES3::BindAdrenoSubBuffer` (@0x95a14c), que
   só é chamado quando a flag @offset **2053** do renderer está ligada. Essa flag
   liga SÓ quando a GPU reporta a extensão `GL_AMD_compressed_ATC_texture`
   (@0x599932) — ou seja, é **detecção de Adreno/Qualcomm**. Em Mali (não tem ATC)
   a flag fica 0 → `framedepth` não amostrável → `textureProj` retorna 0 → sombra 0.
   Isso casa com "funciona no celular Adreno (Moto G100), não no Mali/X5M".

**Experimento de fix (BULLY2_SHADOW_DEPTHFIX, opt-in):** NOP no `cbz w8,skip`
@0x95aa8c força `BindAdrenoSubBuffer` em não-Adreno SEM fingir ATC (texturas
intactas). Efeito confirmado: o resolve passou a desenhar em **fbo=3** (FBO
privado novo com o depth re-anexado). **PORÉM a sombra continua invisível** no
A/B diurno (sol baixo hora 8, Jimmy no pátio: OFF≈ON). Logo, tornar o framedepth
amostrável não basta — há mais camada(s) Mali-específica(s) (sampler2DShadow do
light-depth-map / matriz shadowtrx / sampling do world shader). Sombra diferida
inteira no Mali = **muro profundo**, não resolvido nesta sessão.

**Decisão do usuário ("já que não existe sombra, desativar do menu"):** a opção
**"Shadows" foi ESCONDIDA do menu Settings→Display** (`GetDisplayShadowOption`
@0x1033ccc hookado → 0). Confirmado visualmente no X5M: a linha "Shadows" some
(sobra a lacuna entre Clarity e Textures); restam Brightness/Subtitles/Language/
Clarity/Textures/Light. Escape hatch: `BULLY2_SHADOWS_MENU=show` reexibe.
Nível de sombra fica em **Medium** (default de sempre) de propósito — o nível
controla também os blob/mesh shadows que FUNCIONAM; pôr Off seria regressão
visual. Blob shadow sob personagens continua igual ao validado.

**Ferramentas/infra desta sessão** (todas opt-in por env, cacheadas p/ não pesar
em 1GB): `BULLY2_RESOLVEDRAW` (rastreia se o resolve desenha), `BULLY2_RESOLVEDUMP`
(readback do FBO de resolve), `BULLY2_RESOLVE_FORCE=1..5` (força a saída do resolve
p/ isolar fator), `BULLY2_SHADOW_DEPTHFIX` (força BindAdrenoSubBuffer). Screenshot
sob demanda: `touch /dev/shm/bully_shot`. Toque por coord: `echo "X Y" >
/dev/shm/bully_tap` (abre pause tocando o radar ~1770,110; navega menus touch).

---



Missão: fazer a opção "Shadows" do menu funcionar e melhorar a qualidade
gráfica em devices com RAM/GPU sobrando (X5M 4GB Mali-G310), SEM afetar os
devices de 1GB que já rodam bem. Estudo + disasm + testes ao vivo no X5M.

## Devices do estudo
- **X5M** (192.168.31.103, root/nextos): 3680MB, Mali-G310 (Bifrost/Valhall),
  ES 3.2 real, kmsdrm 1080p — "melhor imagem de todas".
- Mali-450 (.79): 832MB, Utgard, ES2. R36S (.160, ark/ark): 639MB, Mali-G31.

## 1. Por que a opção "Shadows" do menu quase não faz nada (disasm)

Confirmado no assembly do libGame.so:

1. **LUT raw→level `{0,1,2,2}` @ 0x5d05c0** (`BullyGameRenderer::GetShadowLevel`
   @ 0x102b2c8): Off→0, Low→1, Medium→2, **High→2**. Med e High produzem o
   MESMO nível interno. A única diferença de "High" é o SSAO (gated por
   `raw>=3` dentro de SetupPostProcess).
2. **`ApplyDisplay` só grava um escalar** (`SetEffectsLevel` @ 0x949e64 =
   `str w1,[x0,#92]; ret`) — trocar a opção NÃO reconstrói o post-process nem
   redimensiona o shadow map. Só vale após recarregar o nível.
3. **Nível 3 CRASHA**: forçar a LUT p/ `{0,1,2,3}` faz GetShadowLevel devolver
   3, que é fora do range do renderer mobile (só 0/1/2) → leitura OOB (fault
   ~0x11718). Testado ao vivo no X5M: crash no frame 300. O clamp Med=High é
   INTENCIONAL no build mobile.

Ou seja: Off/Low/Med diferem (mesh shadows via `UpdateMeshShadows` no fechar do
menu), mas **Med↔High só difere por SSAO, e SSAO está quebrado**.

## 2. Shadow map (nitidez da sombra)

- `ShadowSceneView::RenderView` @ 0x884d24: shadow map base **1024**
  (`mov w10,#0x400` @ 0x884dd8), modulado por um float (ShadowSceneView+952).
  No runtime o log mostra RT2DES 512x512 (o 1024 já modulado).
- **NÃO depende do setting de sombra** — trocar Off→High não muda a nitidez.
- Patch testado: 1024→2048 (`0x5280800a`→`0x5281000a`). **Estável no X5M**
  (frame 3900, 0 crash). Mas ver seção 4: sombra não é visível, então não há
  ganho visível ainda.

## 3. SSAO — a única diferença real de "High", mas crasha

- Tudo em `BullyGameRenderer::SetupPostProcess` @ 0x1028b6c. Gate do SSAO:
  `cmp w21,#3; b.lt` @ 0x1028d70/0x1028d80 (w21 = raw shadow). Cria pp_ssao,
  pp_blur ×2, pp_ssaoapply.
- **CRASHA no X5M também** (não é específico do Utgard): sig=11 dentro de
  `Material::QueueVectorParameter` (bl @ 0x102a15c em RenderGame), chamada libc
  com ponteiro lixo (~0x11718). O material pp_ssao é criado mas o feed dos
  parâmetros (ssao_vec=0/3) deixa um ponteiro corrompido. `BULLY2_SHADOW_SSAO=0`
  (default) evita criar o pp_ssao e mantém estável.
- Fix do SSAO = path fundo/arriscado (material param interno da engine). Não
  resolvido nesta sessão; fica DESLIGADO por segurança em todos os devices.

## 4. A descoberta central: NENHUMA sombra é projetada na cena

Screenshots ao vivo no X5M (Off, Medium, High-sem-ssao, ped-shadow forçado):
**o Jimmy não tem sombra no chão em NENHUM caso, e a cena inteira não tem
sombras** (nem do prédio, nem das árvores) numa cena externa de dia.

- O shadow map É renderizado (RT2DES/ShadowSceneView ativo, blit=1), mas
  **nunca é projetado/composto na cena**.
- Forçar `g_bPlayerHasShadow=1` (@ 0x13d26d9) NÃO produz blob shadow visível
  (estável, mas o render do blob precisa de mais que o flag — textura/estado).
- Hipótese principal: a projeção/sampling da sombra usa um **shader ES3** que é
  pulado/incompatível no nosso caminho **ES2 forçado** (glGetString spoof
  "OpenGL ES 2.0" + highp→mediump). No X5M (ES3.2 real) o teste
  `BULLY2_REAL_GL_VERSION=1` (sem spoof) é o experimento decisivo — RESULTADO
  ABAIXO.

## 5. Alavancas de qualidade (disasm) — endereços

| Item | VA | Efeito |
|---|---|---|
| GetShadowLevel (LUT) | 0x102b2c8 / LUT 0x5d05c0 | Med=High (clamp intencional) |
| SetupPostProcess | 0x1028b6c | monta post-process 1x |
| Gate SSAO `cmp #3` | 0x1028d70 | SSAO só em High (crasha) |
| Shadow map 1024 | 0x884dd8 | nitidez (fixo) |
| Clamp resolução 800 | 0x1028cb4/cb8 | RS_Low teto 800px; RS_High=nativo |
| Seleção RS | 0x1028cc0 | app+176+32 |
| g_bPlayerHasShadow | .bss 0x13d26d9 | blob ped (flag insuficiente) |
| g_AntiAliasing | .bss 0x136ccf0 | MSAA (sem toggle no menu) |
| perfprofile GetDefaultProfile | 0x974784 | defaults por device |
| RS interna: RS_High=nativo | 0x1028d28 | X5M já em nativo (sem supersampling) |

- **Resolução**: RS_High já dá resolução NATIVA (sem supersampling acima do
  nativo — o path 2x exige EffectsLevel>=4, que nunca ocorre = código morto).
  Por isso o X5M "mesmo em low é a melhor imagem": o nativo 1080p já é cheio.

## 6. Experimentos implementados (todos OPT-IN, default = zero mudança)

`patch_shadow_quality()` / `maybe_force_ped_shadow()` em jni_shim.c:
- `BULLY2_SHADOW_MAP=2048` — shadow map 1024→2048 (auto em RAM≥2600MB; ver
  seção 4, sem ganho visível até a sombra ser projetada). Estável.
- `BULLY2_SHADOW_LUT_HIGH=1` — **NÃO USAR** (crasha, level 3 OOB). Documentado.
- `BULLY2_PED_SHADOW=1` — força g_bPlayerHasShadow (estável, sem efeito visível).

## 7. 🎯 DESCOBERTA PRINCIPAL: RendererES3 = modo de alta qualidade (X5M)

O experimento decisivo (`BULLY2_REAL_GL_VERSION=1`, sem spoof ES2) no X5M:
- **RODA e RENDERIZA no Mali-G310** (a nota antiga de "tela preta com ES3" era
  do Mali-G31/R36S e Utgard, não do G310). Estável **18600 frames, 0 crash** no
  mundo aberto (com shadow map 2048 junto).
- **Imagem visivelmente mais rica**: iluminação/color grading/post-process do
  caminho ES3 (RendererES3) para o qual o jogo mobile foi FEITO. É o maior
  ganho de qualidade em device capaz.
- Ainda sem sombra dura de personagem nesta cena, MAS a fidelidade geral sobe
  bem — que é o cerne do "qualidade em 2GB+ parece ruim".

### Implementação (gateada por RAM — 1GB INTOCADO)
`es3_quality_mode()` em imports.c:
- **AUTO**: usa RendererES3 (não spoofa ES2) só se **RAM≥2600MB** (classe 4GB =
  X5M) **E** o contexto GL real é "OpenGL ES 3.x". Senão → spoof ES2 (default
  de sempre).
- Mali-450 (832MB Utgard) e R36S (639MB Mali-G31) → ES2 spoof, **zero mudança**.
- `BULLY2_RENDERER=es3|es2` força; `BULLY2_REAL_GL_VERSION=1` compat.
- Escape hatch: se alguma cena quebrar no X5M, `BULLY2_RENDERER=es2` volta ao
  seguro.

## 8. Resumo do que ficou

- **Sombras "não funcionam"**: raiz estudada e documentada — Med=High (clamp
  intencional), level 3 crasha, SSAO crasha, e a projeção da sombra no mundo
  não aparece (mesmo em ES3). NÃO é consertável com segurança nesta sessão sem
  arriscar o render que funciona.
- **Qualidade gráfica em device capaz**: RESOLVIDO com RendererES3 auto
  (RAM-gated) — imagem mais rica no X5M, 1GB intocado.
- Experimentos de sombra (map 2048, LUT high, ped shadow) ficam opt-in; só o
  shadow map 2048 auto acompanha o modo ES3 em RAM alta (estável, sem regressão).
- SSAO e a projeção de sombra do personagem ficam como trabalho futuro
  (arriscado; precisa RE do composite de sombra + fix do material param do SSAO).

## 9. Diagnóstico do composite (2026-07-02, hooks SetShadowTexture/Matrix)

Hookei `RendererES3::SetShadowTexture` (0x95bd5c) e `RendererES::SetShadowMatrix`
(0x956b74) — os que entregam o shadow map ao shader do mundo:
- **AMBOS SÃO CHAMADOS** com textura válida (tex=0x55607c55e0) e matriz. Ou
  seja, **a pipeline de sombra está LIGADA**: o shadow map (renderizado,
  512x512) É aplicado ao renderer/shader. Não é o composite que falta.
- Então a sombra ou (a) renderiza SUTIL demais na iluminação da cena, (b) fica
  ATRÁS do personagem (câmera é traseira, sol parece à frente → sombra projetada
  pro lado oposto da câmera, escondida pelo corpo), ou (c) o conteúdo do shadow
  map / matriz de projeção está deslocado.
- Determinar qual exige teste INTERATIVO (rotacionar câmera, mover personagem,
  mudar hora do dia) ou comparar com a referência (celular) — não dá pra
  resolver por screenshot estático de câmera fixa.
- Hooks de diagnóstico ficam opt-in (BULLY2_SHADOWLOG=1); não afetam jogo normal.

## Próximo passo (futuro, arriscado)
- Investigar por que o shadow map não é projetado no mundo (composite/sampling)
  — possível shader específico. Testar em ES3 com log de shaders.
- Fix do SSAO: alimentar os 3 vetores do pp_ssao (Material::QueueVectorParameter
  @ 0x9441fc) sem o ponteiro lixo.
- Validar RendererES3 em cenas variadas (interiores/cutscenes/combate) no X5M.

## 10. Diagnóstico PROFUNDO (2026-07-02, dump de shaders + uniforms)

Referência do usuário: **sombra FUNCIONA no celular** (Moto G100 = Adreno 650),
mesmo libGame. Fork antigo do amigo TAMBÉM não mostra sombra = não é regressão
nossa. Arquitetura de sombra do Bully (via BULLY2_DUMP_SHADERS):
- **Deferred screen-space shadow**. 3 estágios:
  1. Shadow map luz: depth texture 2048x2048 (`highp sampler2DShadow shadowdepth`).
  2. Resolve (sh_0153): `textureProj(shadowdepth, shadowPos)` lê `framedepth`
     (depth-stencil da cena), projeta por `shadowtrx`, escreve shadowtex =
     `shadowAmt * shadowparam.x`.
  3. World (sh_0016): `getshadow()=1.0-texture(shadowtex, gl_FragCoord*rendersize.zw).r`.
- **Tudo verificado CORRETO no nosso port (X5M ES3)**:
  - Shaders compilam: 0 falhas (BULLY2_SHADERCHK).
  - `GL_TEXTURE_COMPARE_MODE=COMPARE_REF_TO_TEXTURE(0x884e)`, func LEQUAL — setado
    certo na textura de sombra (BULLY2_SHADOWLOG glTexParameteri).
  - Depth textures criadas (shadow 2048 depth, gbuffer depth-stencil).
  - Uniforms (shadowparam/rendersize) via **UBO** (glGetUniformLocation nunca
    chamado com esses nomes; glUniform4f/4fv idem) = padrão ES3, ok.
  - Nenhum shim nosso (glClear/Viewport/Framebuffer/UseProgram/DrawElements)
    altera comportamento (pass-through só log).
- **CONCLUSÃO**: pipeline 100% igual ao Android real. A sombra falha em **Mali**
  (X5M G310, e provavelmente todos nossos devices Mali) mas funciona em **Adreno**
  (celular) = incompatibilidade específica do driver Mali com a técnica de sombra
  diferida (sampler2DShadow/textureProj sobre depth texture / UBO). Não é bug de
  código nosso — é o driver Mali + a técnica da engine.
- Fix real exigiria: (a) trace GL do celular (Adreno) p/ comparar chamada-a-chamada,
  ou (b) reescrever a técnica de sombra (patch shader/engine) — arriscado. Ferramentas
  de diagnóstico deixadas opt-in: BULLY2_DUMP_SHADERS, SHADERCHK, UNIFLOG, SHADOWLOG.

## 11. 🎯 CAUSA-RAIZ NÍVEL ENGINE (2026-07-02, sessão 2 — disasm + runtime)

A "incompatibilidade Mali" da seção 10 foi rastreada até o mecanismo exato:

1. **A engine tem um caminho de sombra que SÓ liga em Adreno.**
   `RendererES::Initialize` (0x955be4) varre GL_EXTENSIONS e liga a flag
   `[renderer+2053]` quando acha **`GL_AMD_compressed_ATC_texture`** (extensão
   exclusiva Qualcomm/Adreno). Essa flag gateia (cbz @0x95aa8c em
   `RendererES3::BeginRendering`) a chamada de
   **`RenderTarget2DES3::BindAdrenoSubBuffer`** (0x95a14c), que cria um FBO
   privado re-anexando color/depth/stencil — o caminho que torna o
   depth-stencil da cena (`pp_depthstencil` = `framedepth`) utilizável pelo
   resolve de sombra. Em Mali: flag 0 → caminho nunca roda.
2. **Sem esse caminho, o resolve roda em feedback-loop** (provado com
   BULLY2_SHADOWTRACE): o pass de resolve (prog c/ sampler2DShadow) desenha no
   fbo da cena **amostrando a MESMA textura de depth (tex4) que está anexada
   ao fbo** — comportamento indefinido no spec. Adreno tolera; **Mali retorna
   0/lixo** → `frameSample=0` → shadowPos degenerado → resolve escreve 0 →
   `getshadow()=1-0=1` → zero sombra no jogo inteiro. Readback confirmou:
   resolve output 100% zerado (glReadPixels do fbo do resolve).
3. **Casters NÃO têm gate** — `ShadowSceneView::RenderView` desenha o shadow
   map (2048 depth) igualmente em qualquer GPU. O problema é só no resolve.
4. **Experimentos** (todos opt-in, default OFF):
   - `BULLY2_SHADOW_DEPTHFIX=1`: NOP no cbz @0x95aa8c → força
     BindAdrenoSubBuffer em Mali. O pipeline ACORDA (usuário viu um "quadrado
     gigante de sombra que anda junto" = frustum do shadow map com compare
     uniforme), mas a sombra por-objeto não sai: o subFBO re-anexa o depth
     TAMBÉM → feedback persiste → frameSample continua lixo.
   - `BULLY2_FEEDBACK_FIX=1`: blit do depth p/ textura-cópia + amostrar a
     cópia no draw do resolve (quebra o feedback de verdade). Sozinho e
     combinado com DEPTHFIX: ainda sem sombra visível.
   - `BULLY2_RESOLVE_FORCE=3` (saída = textureProj cru, sem shadowparam):
     ainda nada → o compare em si retorna constante em Mali. Há pelo menos
     mais um degrau driver-specific (formato/estado do sampler2DShadow ou
     conteúdo do shadow map no momento do resolve) NÃO fechado.
   - Simular Adreno? Spoof de GL_RENDERER só muda o perfprofile; a flag vem da
     extensão ATC, e fingir ATC faria a engine escolher texturas ATC que o
     Mali não decodifica. DEPTHFIX já é a simulação limpa — e não basta.
5. **RendererES2 (Mali-450/1GB) nunca terá essa sombra**: não overridea
   SetShadowTexture/SupportsDepthSampling (stubs `ret`); sampler2DShadow é
   ES3-only. O que existe em ES2 é a família `Shadows::*` (blob/decal).

## 12. DECISÃO (2026-07-02): opção "Shadows" ESCONDIDA do menu

Como a sombra diferida não renderiza fora de Adreno (e High ainda arrisca o
SSAO quebrado), a linha "Shadows" foi REMOVIDA do menu Settings→Display via
hook de `BullySettings::GetDisplayShadowOption` → 0 (mesmo mecanismo que o
jogo usa p/ esconder em device fraco). Validado na tela no X5M: Display mostra
só Brightness/Subtitles/Language/Clarity/Textures/Light. O rendering fica
IDÊNTICO ao validado (default interno continua Medium=2; mesh shadows
intactos). Escape hatch: `BULLY2_SHADOWS_MENU=show` reexibe a opção (com
`BULLY2_SHADOWS_MAX` como antes).

## 13. RendererES3 liberado p/ devices 2GB+ (2026-07-02)

O gate de RAM do modo ES3 auto (`es3_quality_mode`) baixou de 2600MB (classe
4GB) p/ **1700MB** (classe 2GB reporta ~1800-2000 de MemTotal): qualquer
device 2GB+ com contexto GL ES3 real ganha o RendererES3 (imagem mais rica,
highp preservado). Devices 1GB (~630-950MB) ficam no ES2 spoof, intocados.
Ajuste fino: `BULLY2_ES3_MIN_MB=<mb>`; escape: `BULLY2_RENDERER=es2` se algum
2GB (ex. Mali-G31) regredir. Shadow map 2048 auto continua só em ≥2600MB.

**Teste ES3 no R36S/Mali-G31 (ArkOS .110, 639MB, 2026-07-02):** `BULLY2_RENDERER=es3`
forçado — **RENDERIZA correto** (cutscene + gameplay 640x480, lighting/pós-processo
do caminho ES3 visíveis; a "tela preta com ES3 no G31" das notas antigas está
SUPERADA pelos fixes atuais do caminho ES3) e ficou estável 90s com ~206MB livres.
**PORÉM com lag pesado** (validado pelo usuário na TV) — o G31 não sustenta o
custo do RendererES3. Veredito: R36S/1GB fica no ES2 spoof (que já é o default
do piso 1700MB — nenhuma mudança necessária). O piso 2GB+ segue correto: o
limitador em 1GB é GPU+RAM, não só RAM.

Diagnósticos novos desta sessão (opt-in): BULLY2_SHADOWTRACE (mapa
fbo→attachments, textura por unidade, draws por (fbo,depth,color,depthmask)
por frame, detecção de feedback-loop, readback do resolve),
BULLY2_RESOLVEDUMP, BULLY2_RESOLVE_FORCE=1..5, BULLY2_FEEDBACK_FIX,
BULLY2_SHADOW_DEPTHFIX; tools/vgpad.c ganhou botões/hat/seq e há injetor de
eventos p/ pad físico (navegação de menu por SSH sem clonar o pad).
