# sor4 — Streets of Rage 4 (port NextOS)

Port de **Streets of Rage 4** (APK Android v1.4.5, engine **MonoGame/.NET**) para device
Mali-450 (NextOS). Caminho **gerenciado** (.NET nativo + MonoGame DesktopGL/gl4es), NÃO so-loader.

## Game files (BYO — você fornece, do seu APK legítimo)
- `lib/arm64-v8a/libassemblies.arm64-v8a.blob.so` (assembly store → DLLs do jogo)
- `assets/` (.xnb + banks Wwise)

## Estado
Ver `HANDOFF.md` (diário). Plano: `~/.agente/plans/polymorphic-weaving-leaf.md`.

## Estrutura
- `build/` — trabalho no host (assemblies extraídos, runtime, host). NÃO versionado (grande).
- `port/`  — launcher `.sh`, shims, config p/ o device.
