# ESTUDO — RAM / Streaming / Draw Distance do Bully AE (2026-07-01)

Estudo grande de otimização para PortMaster em devices de 1GB (pior caso: R36S-clone
"R36T/K36S" com **639MB utilizáveis** + zram de sistema 512MB — não tocar no zram).
Cruzamento de 4 fontes: nosso código bully2, símbolos do `libGame.so`, o port GTA SA
do acervo (referência que roda perfeito em resolução máxima) e os ports abertos
bully_vita / bully-NX / gtasa_vita.

## 1. A DESCOBERTA CENTRAL — RAM reportada via GetDeviceType

O engine War Drum **se autodimensiona pela RAM que o loader reporta** em
`GetDeviceType()` (JNI): bits `[31:6]` = RAM em MB, bits `[5:2]` = tier (3),
bit 0 = phone. Com **<256MB o engine liga o modo low-memory interno** (pools de
streaming/TXD menores, texturas menores, densidades menores — tudo nativo).

- bully_vita (Vita, 512MB reais): reporta **160MB** → o jogo INTEIRO cabe.
- bully-NX (Switch): reporta 1024MB.
- **Nós reportávamos 2048MB hardcoded** → engine gastava como celular de 2GB.

Fix aplicado (`jni_shim.c`, `device_ram_report_mb()`): auto por `/proc/meminfo` —
`<800MB→192`, `<1280→256`, `<2048→512`, senão `1024`. Override:
`BULLY2_DEVICE_RAM_MB`. Log `[devtype]`.
**Teste no R36S usa 256 (pedido do usuário: simular a classe 1GB).**

O bully-NX usa o MESMO build arm64 v1.4.311 que nós (ApplyDisplay 0x10344dc bate) —
offsets deles transferem direto.

## 2. O que já tínhamos (validado, manter)

- **Texture profile Low/Medium/High** (half exato 256/512/full em `my_glTexImage2D`,
  descarta mips>0 + halve level 0 + re-mipmap trilinear). Economia >130MB no Medium.
  `texture_profile.cfg` persiste; menu in-game Textures/Light via `bully2_patch.zip`.
- **EVICT=onlow + TidyUpTextureMemory(force)** (default): estável ~220MB GL sem swap.
- **Patch de draw distance** `BULLY2_STREAM_DISTANCE_PCT` (bytecode em
  `CStreaming::AddModelsToRequestList`, offsets 0x34..0x84): 60% Medium / 50% Low /
  nativo High.
- **Streaming nativo ATIVO**: NvAPK/ZIPFile do próprio libGame + CDStreamThread;
  data_0..4.zip via OS_ZipAdd. Não hookamos o caminho quente.
- Rejeitados (NÃO repetir): EVICT=native agressivo (sig11 0x10a8), EVICT_MEMOBJ
  (thrash), TEX_BUDGET_HOOK 96MB (mais reload), LOWMEM_PROCESS (subiu RSS),
  TEX_SCALE_PCT arbitrário (tela preta), unload total de residentes (render preto).

## 3. Novos patches desta sessão (s-atual)

1. **GetDeviceType RAM-aware** (item 1) — o patch nº1 dos ports do TheFloW.
2. **isPhone força 1** (`maybe_force_phone_flag`, .bss VA 0x125da04, frames
   30/300/900, só escreve se ler 0/1; `BULLY2_FORCE_PHONE=0` desliga) — evita
   efeitos classe desktop (receita bully-NX).
3. **Pin de threads** (`pin_engine_thread` no `my_OS_ThreadLaunch`): GameMain→core1,
   RenderThread→core2, **CDStreamThread→core3 (streaming nunca disputa com render)**.
   Auto em >=4 cores; `BULLY2_THREAD_PIN=0` desliga. Receita bully_vita/gtasa_vita.
4. **Limitador de FPS opcional** (`BULLY2_FPS_LIMIT=30`, pacing absoluto no present,
   egl_shim.c). Default off. CPU medida no baseline: ~145% de 400%.

## 4. Baseline medido (R36S, texturas low, ANTES dos novos patches)

Sessão real de gameplay 2026-07-01 19:40–19:46 (perfil low ativo, RAM 256 ainda NÃO):
- RSS estável **~215MB**, swap de sistema ~465-470MB usado (estável, sem leak),
  CPU ~145%, saída limpa por SELECT+START após ~38k frames. Sem OOM.

## 5. Alvos futuros mapeados no libGame.so (dynsym completo, 49.938 símbolos)

| Alvo | Símbolo/endereço | Nota |
|---|---|---|
| Budget macro de streaming | `g_StreamingHeapSize` 0x13d8854 (.bss) | ver como é derivado da RAM reportada antes de patchar na mão |
| Reserva de vertex buffer | `TextureHeapHelper::ms_reservedVertextBufferMemoryInBytes` 0x124c848 (.data) = **50MB fixos** | candidato a corte; medir efeito real |
| Pools | imediatos dentro de `CPools::Initialise` 0xcd9208 | 51 pools; provável que low-memory mode já reduza |
| LOD dists | `CVisibilityPlugins::ms_pedLodDist` 0x146ba54, `ms_vehicleLod0Dist` 0x146ba50, `CSimpleModelInfo::SetLodDistances` 0xeb4238 | não há slider no jogo; escala por hook se precisar |
| Far clip | `RwCameraSetFarClipPlane` 0xb3fb68 + timecycle `m_FogReduction` 0x1477e9c | via de dados = timecyc |
| 30fps nativo | `RendererES::SetSync` 0x9584d8 com `VSYNC_TWICE` | alternativa ao pacing no swap |
| Gate fixo | `CStreaming::IsThereEnoughFreeMemory` 0xec59b4 = teto hardcoded 10MB/objeto | cuidado, não deriva do heap |
| Cache .idx do zip | hook `_ZN7ZIPFile11SortEntriesEv` (bully_vita) | boot 3× mais rápido; TODO |
| Struct BullySettings | +0x1c shadow, +0x20 resolution (0=High!), +0x38 shadow_profile, +0xac effects_level; `ApplyDisplay` 0x10344dc, `Load` 0x1034a14 | forçar clarity/sombras por fora do menu (bully-NX) |

## 6. Lições do GTA SA do acervo (por que ele roda bem)

- Mesmo esqueleto so-loader; streaming controlado por `stream.ini` (`memory 13500`,
  `vehicles 12`) + JNI `GetAvailableMemory` — análogo exato do nosso GetDeviceType.
- **`drop_highest_lod 1` ligado até na config de qualidade máxima.**
- Config performance: `decal_limit 0`, `debris_limit 0.5`, `character_shadows 0`.
- Launcher: governor performance + `drop_caches` antes de iniciar (sem zram).
- Resolução AUTO (`screen_width -1`) via hook `OS_ScreenGetWidth/Height` (já fazemos).

## 7. Multi-device / PortMaster (pendências)

- Autodetect por RAM já cobre heap de engine (via RAM report). Perfil de textura
  default = low (instalação limpa) — ok para 1GB.
- `drawbuffers_safe` autodetect é só `/sys/module/mali/version` (Utgard); revisar
  para Bifrost/Panfrost.
- Endereços fixos (0x103xxxx, 0x125da04) valem só para o build v1.4.311 — todos com
  fallback de símbolo ou verificação de sanidade antes de escrever.
- glibc: binário deployado no R36S = build glibc230 (`build-glibc230.sh`, Docker
  Ubuntu 19.10 arm64). Mali-450 usa `build.sh` (toolchain NextOS).

## 8. Descobertas do disasm (s-atual, build arm64 v1.4.311)

- **Esse build NÃO consulta `GetDeviceType` via JNI** (log de GetMethodID completo:
  só app-glue — splash/movie/playlist/http/rockstar/vibrate). O RAM-report fica
  como defesa p/ outros builds, mas não é a alavanca aqui.
- **`isPhone` vem de campo Java**: `AND_SystemInitialize` faz
  `GetFieldID(DeviceInfo, "isPhone", "Z")` + `GetBooleanField` → grava .bss
  0x125da04. Nossos slots de field eram ret0 → isPhone=0 → **efeitos classe
  desktop estavam LIGADOS**. Corrigido pelo força-1 (frames 30/300/900).
  Classe: `com/rockstargames/oswrapper/DeviceInfo`; campos: width, height, isPhone.
- **`g_StreamingHeapSize` é vestigial**: zero relocations, zero adrp p/ o VA.
  Não é alavanca nesse build.
- **`TextureHeapHelper::GetTotalGraphicsMemoryOfSystem` retorna 512MB
  HARDCODED** (0x20000000, edde08). O engine acha que sempre tem 512MB de
  VRAM → nunca despeja por conta própria → o EVICT=onlow sintético + half-res
  GL são mesmo o mecanismo certo (e o TEX_BUDGET_HOOK de 96MB falhou porque o
  engine compara contra esses getters fixos em mais de um lugar).
- Gate `IsThereEnoughFreeMemory` = teto fixo 10MB/objeto (cmp #0xa00,lsl#12).

## 9. Fontes locais dos ports de referência

- `/tmp/.../scratchpad/{gtasa_vita,bully_vita,bully-NX}` (clones; copiar se quiser manter)
- GTA SA acervo: `/home/nextos/gta-sa-deploy/`, `/home/nextos/ports-staging/gtasa-extract/`
