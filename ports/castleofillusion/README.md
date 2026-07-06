# castleofillusion — NextOS Android port

Port gerado por `nextos_ports_android`. `.so`: `libViewer_GP.so`.

## Game files (BYO — você fornece, do seu APK legítimo)
- `libViewer_GP.so` (de lib/arm64-v8a/)
- assets / OBB conforme o jogo

## Estado
- [ ] Resolver UNKNOWN em src/imports.gen.c (121 símbolos)
- [ ] JNI: package name + OBB path em jni_shim.c
- [ ] Paths/resolução/input em android_shim.c
- [ ] Testar no device Mali
