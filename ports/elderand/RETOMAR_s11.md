# RETOMAR S11 - Elderand 1.3.22.51

Data do ponto de parada: 2026-06-26 03:28 Brazil/East.

## Estado atual

O port compila, sobe para o device e roda sem crash fatal ate o watchdog. O teste mais recente foi:

```sh
./run-device-test.sh 70 "ELD_HSFAKE=1 ELD_PLAYCORE=1 TER_JOBINLINE=1 TER_1CPU=1 CUP_TEXHALF=512 CUP_RENDERSCALE=2 CUP_DRAWCOUNT=1 TER_THRCENSUS=50"
```

Resultado:

- Build OK, binario `elderand` md5 `a64524c07d9819af264e884e87e3e049`.
- Device `192.168.31.154` recebeu e executou o binario.
- Progresso confirmado: `initJni OK`, `nativeRecreateGfxState OK`, `nativeRender`, frames rodando.
- Saida: `RUNEXIT rc=124`, ou seja, watchdog matou no tempo configurado, nao foi crash.
- Screenshot mais recente: `logs/frame_032859.png`, ainda preto uniforme.
- Log FPS ainda mostra `draws/f=0`, entao ainda nao ha draw GL real chegando no framebuffer.

## O que ja foi destravado

- Harness de teste ajustado para SSH/SCP direto via `run-device-test.sh` apontando por padrao para `root@192.168.31.154`.
- `run_test.sh` agora para `emustation`, limpa instancias antigas, faz deploy e captura framebuffer de forma mais estavel.
- `run.sh` e `run_test.sh` criam symlinks para `userdata/il2cpp/Metadata` e `userdata/il2cpp/Resources`, evitando falhas de discovery dos recursos IL2CPP.
- JNI foi endurecido:
  - `jstring` estavel e limite ampliado.
  - method IDs agora usam nome + assinatura.
  - reflection basica (`Class.forName`, `getClass`, `getName`, `To/FromReflectedMethod`) implementada.
  - `System.load`, `findLibrary`, `NewString`, `GetStringChars` e variantes `CallStaticVoidMethodV/A` cobertas.
  - campos de `ApplicationInfo` e modo `ELD_PLAYCORE=1` adicionados para testar Play Asset Delivery fake.
- Patches/neutralizacoes IL2CPP:
  - `FMODUnity.RuntimeManager` neutralizado por API IL2CPP, sem hardcode de offset.
  - `TextMeshPro` neutralizado para evitar NRE por falta de `resources.assets`.
  - `VoxelBusters.EssentialKit/GameServices` neutralizado; o NRE em `GameManager.Awake` saiu do caminho.
- Unity/native:
  - patch de `SWAPPYWAIT` evita deadlock no frame pacing.
  - patch de `OPTCB` evita crash nativo em callback opcional.
  - `ELD_HSFAKE` em `pthread_fake.c` destrava handshake/cond wait do render.
  - logs de thread/job melhorados.
- Addressables:
  - `catalog.json` agora e servido por overlay em `/tmp`.
  - O overlay troca `\\` por `/` e substitui `Addressables.RuntimePath` por `/storage/roms/ports/elderand/assets/aa`.
  - Bundles de `assets/aa/Android/...` passaram a abrir por caminho absoluto real.

## Evidencia importante sobre assets

O APK `/home/nextos/Downloads/com.pid.elderand_1.3.22.51-APK_Award.apk` nao contem `level1`, `sharedassets1.assets` ou `resources.assets` soltos. Ele contem apenas:

- `assets/bin/Data/data.unity3d`
- `assets/bin/Data/datapack.unity3d`
- `assets/bin/Data/sharedassets1.resource`
- `assets/bin/Data/unity default resources`
- `assets/bin/Data/boot.config`
- `assets/bin/Data/Managed/...`

Com UnityPy (`/home/nextos/.upy-venv/bin/python`), foi confirmado:

- `data.unity3d` tem `level0`/boot e objetos iniciais.
- `datapack.unity3d` tem os dados seguintes, incluindo strings como `level1.resS`, `level2.resS` e `sharedassets1`.

Conclusao: o muro ativo nao e arquivo ausente no APK. O muro e a Unity nao montar/usar `datapack.unity3d` como asset pack.

## Ultimo experimento

Foi testado `ELD_PLAYCORE=1`. Isso mudou o comportamento inicial:

```text
jni_shim: ApplicationInfo.splitPublicSourceDirs -> [/storage/roms/ports/elderand]
jni_shim: playCoreApiMissing -> false (Play Core fake/PAD)
jni_shim: getAssetPackState(UnityDataAssetPack) -> COMPLETED
jni_shim: getAssetPackState(UnityStreamingAssetsPack) -> COMPLETED
```

Mesmo assim, o log ainda nao mostra `datapack.unity3d` sendo aberto. Depois dos callbacks, a Unity continua tentando:

```text
assets/bin/Data/sharedassets1.assets
assets/bin/Data/level1
assets/bin/Data/level1.resS
```

e todos esses caminhos dao `stat64-MISS`. Isso explica a tela preta sem crash: a cena/pack seguinte nao entra, e o render loop segue vazio.

## Proximo passo recomendado

Prioridade 1: fazer a Unity montar `datapack.unity3d`.

Caminhos provaveis:

1. Localizar no `libunity.so` do Elderand a rotina equivalente ao mount de Play Asset Delivery/`UnityDataAssetPack`. No Pixelcup havia um hook `PC_PADHOOK`, mas os offsets sao de outro Unity e nao devem ser reutilizados cegamente.
2. Instrumentar mais o caminho PAD: procurar chamadas para `getAssetPackPath`, `getAssetsPath`, `sourceDir`, `splitSourceDirs`, `splitPublicSourceDirs` e confirmar por que o callback `COMPLETED` nao vira abertura de `datapack.unity3d`.
3. Testar um hook nativo especifico de Elderand no mount do asset pack, apontando `UnityDataAssetPack` para `/storage/roms/ports/elderand/assets/bin/Data/datapack.unity3d`.
4. Como experimento rapido, testar redirect de `level1`/`sharedassets1.assets` para `datapack.unity3d`, mas isso e menos correto; pode falhar porque `datapack.unity3d` e um UnityFS com varios arquivos internos, nao um `level1` cru.
5. Depois que `datapack.unity3d` abrir, reavaliar `draws/f`. Se ainda ficar `0`, o proximo alvo passa a ser render/shader; se os draws aparecerem e a tela seguir preta, investigar shader/FBO.

## Comandos uteis

Rodar o teste principal:

```sh
cd /home/nextos/nextos_ports_android/ports/elderand
./run-device-test.sh 70 "ELD_HSFAKE=1 ELD_PLAYCORE=1 TER_JOBINLINE=1 TER_1CPU=1 CUP_TEXHALF=512 CUP_RENDERSCALE=2 CUP_DRAWCOUNT=1 TER_THRCENSUS=50"
```

Ver sinais do muro atual:

```sh
rg -n "playCoreApiMissing|getAssetPackState|getAssetPackPath|datapack\\.unity3d|level1|sharedassets1|draws/f|AA-CATALOG|VOXELNUKE" logs/_eld_device.log
```

Confirmar conteudo UnityPy:

```sh
/home/nextos/.upy-venv/bin/python -c "import UnityPy; env=UnityPy.load('/home/nextos/elderand-build/deploy_1.3.22.51/assets/bin/Data/datapack.unity3d'); print(len(env.objects)); print(sorted(set(str(o.type.name) for o in env.objects))[:80])"
```

## Arquivos principais alterados nesta etapa

- `src/main.c`
- `src/jni_shim.c`
- `src/pthread_fake.c`
- `run_test.sh`
- `run-device-test.sh`
- `run.sh`
- `.gitignore`
- `elderand`

Os logs grandes e screenshots novos ficam em `ports/elderand/logs/`, mas foram deixados fora do commit.
