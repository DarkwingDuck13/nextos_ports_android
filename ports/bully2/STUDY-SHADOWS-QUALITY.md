# ESTUDO — Sombras e Qualidade Gráfica do Bully (2026-07-02)

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
