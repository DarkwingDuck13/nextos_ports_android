# Legend of Mana — Estudo: personagens/logo/fundos PRETOS (2026-07-06)

Continuação do checkpoint bom descrito em `STUDY.md` (logos iniciais, loading,
menu com fundo e animações OK; falta título "Legend of Mana", personagens da
seleção pretos, alguns fundos/elementos pretos).

## TL;DR — causa raiz mais provável (com evidência)

**Canal errado de textura single-channel em GLES2.** O engine M2, rodando no
nosso contexto GLES2, sobe os atlases de índice/máscara como `GL_ALPHA`
(0x1906), mas TODOS os shaders embutidos no `libmain.so` leem essas texturas
pelos canais `.r`/`.g` — nunca `.a`. Em GLES2, sampler de `GL_ALPHA` retorna
`(0, 0, 0, v)`, ou seja `.r = .g = 0` SEMPRE. Resultado: índice de CLUT = 0 e
máscara de texto = 0 em toda a cadeia PSX/CLUT → sprite/logo/texto sai preto
(ou invisível), enquanto a forma do quad continua aparecendo.

Tudo que está PRETO passa pelo caminho PSX/CLUT ou texto-HD-via-CLUT
(`FS_FS4psx_FS4_psxclut`, `TransferPSXClut`, `DrawMeshPSXClut`,
`MESWORK_LoadClutForHdText`). Tudo que FUNCIONA passa pelo caminho HD/ASTC já
resolvido com decode em CPU (`libsor4astc.so`).

Por que funciona no Android real: aparelho real de 2021 dá contexto GLES3 e o
engine usa formatos tipo `GL_R8` (dado no `.r`, como os shaders esperam).
Nosso EGL shim cria ES2 → engine cai no caminho `GL_ALPHA`, que nunca casa com
shaders `.r`/`.g`. É um caminho que a Square provavelmente nunca testou.

## Evidências coletadas (logs do estado bom + binário)

1. `logs/legendofmana-debug-20260706-good.log`:
   - Só existem DOIS tipos de upload de textura com dados:
     - ASTC 8x8 (`fmt=0x93b7`, 120+ uploads) → decodificado na CPU, FUNCIONA
       (é o menu/fundos HD que aparecem).
     - `GL_ALPHA` (`ifmt=0x1906`, 41 uploads) → atlases grandes: 2048x2040,
       2048x2048 (vários), 2048x2032, 2048x1584, 1024x400 (série repetida =
       ring de superfícies de texto), 512x496, 256x192.
   - `glTexSubImage2D`: **zero chamadas** no run inteiro (wrapper loga e nada
     apareceu).
   - Só **um FBO** criado (`glGenFramebuffers = 1`) → transfers de CLUT/VRAM
     são feitos desenhando via FBO (não há upload de paleta por TexImage em
     formato colorido — nenhum upload RGBA/5551 com dados no log!). A paleta
     provavelmente é desenhada no CLUT-texture via vertex colors
     (`a_color`/`clut_data_addr`).
   - Nenhum `glCompileShader FAILED` nem `glLinkProgram FAILED` → com o patch
     highp→mediump os 38 shaders compilam.
   - 80x `cleared stale error 0x500` (GL_INVALID_ENUM pendente) em bursts no
     init de thread de render — secundário, rastrear depois.

2. Shader psxclut completo extraído do `libmain.so` (strings):

```glsl
precision mediump float;
#define HIGHP highp
...
uniform sampler2D u_tex;      // página de textura PSX (índices)
uniform sampler2D u_texClut;  // atlas de paletas
uniform HIGHP float u_rclutWSize;
uniform HIGHP vec2 u_clutAddr;
...
HIGHP vec4 ltc = texture2D(u_tex, lt) * u_rclutWSize;
ltc = texture2D(u_texClut, u_clutAddr + vec2(ltc.r, 0));  // <- lê .r !!
```

   E o caminho de texto HD: `src.a *= texture2D(u_texUnitIdAlpha, alphaCoord).g;`
   (lê `.g`!).

3. Contagem de canais lidos por `texture2D(...)` em TODOS os shaders do
   binário: **2090x `.g`, 4x `.r`, 2268x `.rgb`, 0x `.a`** (leitura direta).
   O engine assume que textura de 1 canal aparece em `.r/.g` (estilo
   LUMINANCE/R8) — nunca em `.a`.

4. `GL_ALPHA` em GLES2 sampleia `(0,0,0,v)` por spec → `.r=.g=0`. Índice CLUT
   vira 0 → entrada 0 da paleta (preto/transparente) → **personagem preto,
   logo "Legend of Mana" preto sobre fundo preto (parece sumido), fundos de
   elementos PSX pretos**. A cadeia também contamina o composite: se a página
   é blitada para o VRAM-texture RGBA com o shader de blit
   (`gl_FragColor = texture2D(u_texUnitId, v)`), grava `(0,0,0,v)` no destino
   e o `.r` continua 0 no segundo salto.

## Correção proposta (experimento nº 1 da próxima sessão)

No `glTexImage2D_wrap` (src/imports.c): quando `format==GL_ALPHA (0x1906)`,
`type==GL_UNSIGNED_BYTE` e `pixels!=NULL`, expandir CPU-side para
`GL_LUMINANCE_ALPHA` (2 bytes/texel, `L = A = v`) e subir com
`ifmt=fmt=GL_LUMINANCE_ALPHA`. Assim:

- `.r/.g/.b = v` → índice CLUT e máscara de texto passam a funcionar;
- `.a = v` → qualquer uso legítimo do alpha continua correto.

Regras do experimento:

- **NÃO** converter alocações com `pixels==NULL` (podem ser attachment de FBO;
  LUMINANCE_ALPHA não é color-renderable em GLES2 — converter isso quebraria o
  FBO). Só uploads com dados.
- Aplicar o mesmo em `glTexSubImage2D_wrap` (hoje não é chamado, mas se passar
  a ser, precisa casar com o formato da alocação — cuidado: se a alocação
  original foi `GL_ALPHA` NULL e depois vier SubImage, será preciso realocar
  a textura como LA no primeiro SubImage, ou converter também alocações NULL
  de GL_ALPHA que NUNCA são usadas como attachment — instrumentar antes).
- Custo de RAM: dobra os atlases (~2048x2048: 4→8 MB cada; estimativa total
  +30~60 MB). Se apertar no S905L (852 MB), fallback: `GL_LUMINANCE` puro
  (`.a=1`; os shaders leem máscara por `.g`, então provavelmente suficiente).

Resultado esperado: título "Legend of Mana", personagens da seleção e
elementos pretos passam a renderizar. Se aparecerem com CORES ERRADAS/banding,
ver seção "segunda ordem" abaixo.

## Correção secundária (fazer junto ou logo depois)

O patch highp→mediump hoje é aplicado a **todos** os shaders, inclusive
vertex. No Mali-450 o GP (vertex) é fp32 e aceita `highp` numa boa — só o PP
(fragment) que não tem highp. Patch ideal: consultar
`glGetShaderiv(shader, GL_SHADER_TYPE)` e patchear **só fragment shaders**.
Vertex com mediump degrada `v_texCoord` em atlas de 2048 texels (fp16 não
endereça 2048 posições) — pode ser a causa de sampling impreciso/borrado
mesmo depois do fix de canal. Testar em passo separado do experimento nº 1
(uma mudança por vez, com rollback).

## Segunda ordem — precisão fp16 no lookup dependente

`u_clutAddr + vec2(ltc.r * u_rclutWSize, 0)` roda em mediump (fp16) no PP.
Se o CLUT-texture tiver ≤512 texels de largura, as coordenadas
`(k+0.5)/256` são exatas em fp16 e não há problema. Se for 1024+, índices
altos podem errar a entrada da paleta (cores trocadas/banding). Mitigação, SE
o sintoma aparecer: patch cirúrgico só no shader psxclut (detectar por
`u_texClut` no source em glShaderSource) reescrevendo a aritmética para
formas exatas em fp16, e/ou forçar caminho `u_useBilinear=0` (1 lookup em vez
de 4). Primeiro medir o tamanho real do CLUT-texture (instrumentação abaixo).

## Instrumentação a adicionar (read-only, não muda comportamento)

1. Logar também uploads com `pixels==NULL` (hoje o logger pula): ifmt/size de
   TODAS as alocações → identifica o CLUT-texture e o VRAM-texture.
2. Logar `glFramebufferTexture2D` (attachment/texture) e
   `glCheckFramebufferStatus` → confirma que os transfers CLUT via FBO estão
   completos no Mali.
3. (Opcional) Primeiros ~200 `glDrawElements`: program id + textura bound na
   unit 0/1 → correlaciona o draw preto com a textura fonte.
4. Rastrear origem do `0x500`: drenar erro com log em mais wrappers de init.

## O que continua PROIBIDO (herdado do checkpoint)

- Não preservar highp globalmente (PP não compila / vira ruído).
- Não reativar glTextureSafeDefaults; não forçar GL_NEAREST global; não
  no-opar glGenerateMipmap; não interceptar blend/FBO/attrib/enable/disable
  globalmente.
- Uma mudança por vez, sempre com rollback pro checkpoint
  `checkpoints/legendofmana-good-visual-20260706-1145-srcbin.tar.gz`.

## Sequência sugerida da próxima sessão

1. Rebuild do checkpoint intacto + rodar → confirmar estado bom na tela.
2. Adicionar instrumentação read-only (item acima) → 1 run → coletar mapa de
   texturas NULL/FBO (em especial tamanho do CLUT-texture).
3. Experimento nº 1: GL_ALPHA→LUMINANCE_ALPHA (só uploads com dados) → run →
   capturar framebuffer. Expectativa: logo + personagens aparecem.
4. Se cores erradas/banding → avaliar patch FS-only do highp (correção
   secundária) e depois patch cirúrgico do psxclut.
5. Se tudo OK visual → seguir o STUDY.md: gameplay/áudio/controles
   (OpenSL shim + M2HardKey_OnChange bitmask).

Build/deploy/captura: mesmos comandos do `STUDY.md` (device sagrado
`root@192.168.31.79`, deploy em `/storage/roms/ports/legendofmana`).
