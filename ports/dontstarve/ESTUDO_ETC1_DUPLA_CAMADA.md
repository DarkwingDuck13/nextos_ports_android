# Estudo: ETC2 para ETC1 em dupla camada

## Objetivo

Transformar texturas ETC2 com alpha em duas texturas ETC1:

- camada 1: cor RGB em ETC1;
- camada 2: alpha em ETC1 grayscale, usando o canal vermelho como alpha no shader.

Isso existe porque ETC1 roda nativo no Mali-450/GLES2, mas nao tem alpha. A "dupla camada" simula alpha usando duas amostras de textura.

## Estado atual do port

O port ja faz o caminho seguro:

- ETC2 sem alpha: decodifica e sobe como ETC1, economizando bastante VRAM.
- ETC2 com alpha: decodifica e sobe como RGBA4444.
- `DONTSTARVE_TEX_DOWNSCALE=4` esta ativo no `DontStarve.sh`.
- O jogo ja esta com imagem, chao, audio e controles funcionando.

O ponto caro hoje e textura ETC2 com alpha, porque ela vira RGBA4444. Isso e 16 bits por pixel depois do downscale. Em dupla camada ETC1, ficaria 8 bits por pixel: 4 bpp para cor + 4 bpp para alpha.

## Beneficios se der certo

### 1. Menos VRAM nas texturas com alpha

RGBA4444 usa 16 bpp. ETC1 dupla camada usa 8 bpp.

Na pratica:

- corta pela metade o custo das texturas que hoje viram RGBA4444;
- reduz pressao no Mali-450;
- reduz chance de sumir textura quando o jogo troca de cena ou carrega mundo;
- ajuda especialmente em atlases grandes com alpha.

### 2. Menos RAM geral e menos risco de travar

O jogo ja mostrou que e pesado no carregamento do mundo. Se as texturas com alpha ficarem menores na GPU, o sistema respira melhor.

Beneficio esperado:

- menos picos de memoria;
- menos chance de matar processo por falta de RAM;
- mais estabilidade em gameplay longo;
- mais margem para manter audio, logs e watchdog sem sufocar o jogo.

### 3. Melhor que RGBA4444 em alguns cenarios

RGBA4444 tem so 4 bits por canal. Isso pode criar banding e perda visivel em degradês.

ETC1 dupla camada pode preservar melhor a cor RGB do que RGBA4444, porque a cor fica em ETC1 RGB separado e o alpha fica separado.

Pode melhorar:

- fundos e mascaras grandes;
- cipos, musgos, grama e overlays;
- partes de cenario com transicao suave;
- sprites grandes com borda suave, dependendo do alpha.

### 4. Menos dependencia de RGBA em GLES2

No Mali-450, ETC1 e o caminho mais natural. Se boa parte do jogo virar ETC1 nativo, o port fica mais alinhado com o hardware.

Isso tende a dar:

- upload menor;
- textura mais barata;
- menos gargalo de banda;
- comportamento mais previsivel no device alvo.

### 5. Pode permitir reduzir menos o jogo

Hoje o downscale forte ajuda a caber na RAM. Se a dupla camada funcionar bem, talvez algumas texturas possam usar downscale menor sem estourar memoria.

Exemplo de ganho possivel:

- manter `downscale=4` para seguranca, mas com mais estabilidade;
- ou testar `downscale=3` em grupos especificos sem travar;
- ou deixar UI/fonte melhor enquanto cenario continua leve.

## O que nao e automatico

ETC1 nao tem alpha. Entao nao da para simplesmente trocar `GL_COMPRESSED_RGBA8_ETC2_EAC` por `GL_ETC1_RGB8_OES` e esperar funcionar.

Para ETC1 dupla camada funcionar, o runtime precisa:

1. decodificar a textura ETC2 original;
2. gerar ou carregar uma ETC1 de cor;
3. gerar ou carregar uma ETC1 de alpha;
4. subir a cor no texture id original;
5. criar um texture id extra para o alpha;
6. alterar shader para ler a textura de alpha;
7. bindar essa textura de alpha em uma unidade extra antes do draw.

Ou seja: e viavel, mas nao e uma troca simples de formato.

## Melhor caminho tecnico

### Fase 1: medir primeiro

Escanear as 195 texturas `.tex` do jogo e classificar:

- ETC2 sem alpha: ja podem continuar indo para ETC1 normal;
- ETC2 com alpha simples/recorte: candidatas boas;
- ETC2 com alpha suave: candidatas com risco de bloco;
- UI/fonte/texto: melhor manter RGBA4444 no comeco;
- terreno/cenario grande: melhor alvo inicial.

### Fase 2: prototipo pequeno

Nao converter tudo de uma vez. Comecar por uma textura grande e importante, por exemplo terrain/ground/overlays.

O teste ideal:

- manter o jogo visualmente igual;
- confirmar que o rio nao fica quadrado;
- confirmar que o chao e mascaras continuam certos;
- medir log de memoria antes/depois.

### Fase 3: sidecar offline

O melhor e gerar cache fora do jogo, nao encodar tudo em runtime.

Formato recomendado:

- `cor.etc1`: RGB comprimido ETC1;
- `alpha.etc1`: alpha replicado em RGB e comprimido como ETC1;
- metadados: largura, altura, mip, hash da textura original.

O runtime so carrega e sobe. Isso evita pico de CPU/RAM no device.

### Fase 4: shader allowlist

Converter apenas shaders conhecidos primeiro:

- ground;
- road;
- ground overlay;
- cenario grande;
- depois sprites/animacoes se necessario.

Manter fallback RGBA4444 para qualquer shader nao entendido.

## Riscos

### Alpha pode ficar blocado

ETC1 e bloco 4x4. Alpha suave pode ganhar quadradinhos. Em cenario grande isso pode passar. Em fonte, UI, botoes e particulas pode ficar feio.

Por isso UI/fonte devem ficar fora da primeira conversao.

### Shader precisa de sampler extra

Cada textura com alpha dupla precisa de mais uma unidade de textura.

O GLES2 geralmente tem limite baixo. O Mali costuma ter margem, mas shaders como ground ja usam varias texturas. Precisa testar por shader.

### Pode quebrar mistura de alpha

Alguns shaders usam RGB premultiplicado, outros usam alpha normal. Se aplicar uma regra unica em tudo, pode criar:

- borda preta;
- objeto quadrado;
- brilho errado;
- transparencia invertida;
- rio ou overlay cortado.

### Converter tudo de uma vez e perigoso

O jogo ja esta praticamente rodando. Uma conversao global pode quebrar muita coisa ao mesmo tempo e dificultar saber onde o erro nasceu.

## Recomendacao para este port

Fazer ETC1 dupla camada sim, mas em etapas.

Ordem recomendada:

1. escanear e rankear texturas com alpha por tamanho;
2. escolher 1 ou 2 alvos grandes de cenario;
3. criar sidecar ETC1 cor + ETC1 alpha;
4. alterar shader so desses alvos;
5. comparar screenshot e memoria;
6. expandir para mais texturas apenas se ficar visualmente correto.

Nao recomendo converter tudo de uma vez agora. O ganho e real, mas o risco visual tambem e real. O caminho certo e pegar primeiro as texturas grandes que mais economizam memoria e deixar UI/fonte/particulas no fallback RGBA4444 ate provar que a dupla camada fica boa.

## Conclusao

Se der certo, o ganho principal e memoria: texturas com alpha caem de 16 bpp para 8 bpp depois do downscale, com possibilidade de mais estabilidade e menos travamento no mundo.

O ganho secundario e qualidade: em alguns cenarios a cor pode ficar melhor que RGBA4444.

O custo e complexidade: precisa sidecar, shader alterado e bind extra de alpha. Portanto e uma tecnica boa para atacar os maiores gargalos, nao para aplicar cegamente em 100% do jogo logo de inicio.

## Resultado do primeiro prototipo

Implementado no runtime com chave:

- `DONTSTARVE_ETC1_ALPHA=0`: modo seguro, padrao do launcher.
- `DONTSTARVE_ETC1_ALPHA=ground`: modo experimental para tiles de chao.

O modo experimental reconhece os tiles de chao por hash do mip principal KTEX, entao nao depende mais do path do asset chegar ate o `glCompressedTexImage2D`. Quando o mip 0 bate na tabela, o texture id e marcado e os mips seguintes tambem viram dupla camada.

Resultado visto em log:

- shaders de ground entram com `ground fix applied +etc1a`;
- uploads aparecem como `ETC1A hash:ground-tile`;
- economia acumulada passou de 2 MB no carregamento inicial de tiles com `DONTSTARVE_TEX_DOWNSCALE=4`.

Limite atual:

- o primeiro teste visual mostrou chao preto/branco quando o shader dependia da sidecar antes de ela existir;
- isso foi corrigido com `DS_ALPHA0_ENABLED`, mas ainda falta validacao visual completa em gameplay;
- por seguranca, o launcher foi deixado com `DONTSTARVE_ETC1_ALPHA=0`.

Proximo passo seguro:

1. testar manualmente `DONTSTARVE_ETC1_ALPHA=ground` em gameplay;
2. se o chao ficar correto, testar `DONTSTARVE_TEX_DOWNSCALE=3`;
3. so depois considerar ativar `ground` como padrao.
