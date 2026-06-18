# Streets of Rage 4 — PortMaster (aarch64 / Mali-450)

Port NATIVO do Streets of Rage 4 (v1.4.5, MonoGame/.NET9) para aarch64/Linux,
no padrão PortMaster. Otimizado para o Mali-450 (GLES2) com texturas convertidas
para **ETC1** (o Mali lê ETC1 nativo).

## BYO-DATA — você põe o seu APK (igual Bully / Dysmantle)

Este port **não acompanha os dados do jogo**. Você usa a **sua cópia legal**:

1. Copie o **APK do Streets of Rage 4 v1.4.5** para a pasta
   **`ports/sor4/`** (a mesma pasta deste port).
2. Abra "Streets of Rage 4" no menu de Ports.
3. Na **primeira vez**, abre uma janela (o *progressor*) que pergunta, no controle:
   - **Downscale das texturas** — `3` (RECOMENDADO, ~1GB de RAM) / `2` (mais nítido) / `1` (qualidade total).
   - **Modo de conversão** — `RÁPIDA` (usa todos os núcleos) / `LOW MEMORY` (lenta, pouca RAM).
4. Aí ele faz tudo sozinho, com **porcentagem na tela**:
   - **extrai** do seu APK: texturas (.xnb), áudio (.wem/.bnk), a libWwise e o
     código do jogo (`SOR4.dll`, que fica no *assembly-store* do APK);
   - **patcha** o `SOR4.dll` pra rodar nativo;
   - **converte** as texturas ASTC→ETC1 (com o downscale escolhido);
   - **apaga o APK** no fim (só após sucesso) pra liberar espaço.

Isso roda **uma única vez**. Depois o jogo abre direto.

- Botão **A** = SIM (opção em destaque).  Botão **B** = NÃO (próxima opção).
- Precisa de espaço livre temporário (~4 GB durante a 1ª execução; o APK é apagado depois).

## Controles

Mapeados via `gptokeyb` (`sor4.gptk`): A = ataque, B = pulo, X = especial,
Y = corrida, Start/Select = menus, D-pad/analógico = movimento.

## Porter

felc18-blip. Host .NET próprio (`sor4host`) + MonoGame GLES2; conversor ETC1
(`sor4texconv`) e patcher Cecil rodam pelo runtime embutido no host — não precisa
de .NET instalado no aparelho. A extração do `SOR4.dll` do assembly-store
(XABA/LZ4) é feita em Python puro (`sor4_apkextract.py`), sem dependências.
