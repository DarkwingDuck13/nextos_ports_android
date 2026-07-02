# 📡 Pegando logs: jnitrace (antes do port) e shim IL2CPP (durante)

Duas lupas complementares mineradas do Bogodroid neo ([`ports/bogodroid/`](../../ports/bogodroid/)). A primeira responde "**quais stubs JNI o jogo vai pedir?**" antes de escrever uma linha; a segunda responde "**o que o runtime IL2CPP está fazendo agora?**" quando o port já roda mas se comporta errado.

## 1. jnitrace — mapear a superfície JNI no Android real
Roda o jogo num Android com root (nosso device de referência: **Moto G100 via USB**) e grava TODAS as chamadas JNI. Adeus descobrir stub na base do crash (o `vendorName` do Chrono Trigger custou uma sessão; isso aqui teria mostrado na hora).

```sh
# no PC
pip install frida-tools jnitrace
# frida-server no device (baixar o binário arm64 do release do Frida)
adb push frida-server /data/local/tmp/ && adb shell "su -c '/data/local/tmp/frida-server &'"
# traçar o jogo (deixa rodar até a gameplay, depois CTRL-C)
jnitrace -l libunity.so -l libil2cpp.so -o trace.json com.pacote.dojogo
# consolidar em árvore classe→métodos/assinaturas
python3 ports/bogodroid/upstream-neo/tools/jnitrace_parse.py trace.json > parsed.json
```

O `parsed.json` é a **lista de compras do fake-JNI**: cada classe/método/assinatura que o jogo realmente usa. Pra jogos não-Unity, trocar os `-l` pelas libs do jogo.

📦 Já temos um trace pronto de um jogo Unity real pra estudar sem montar nada: `ports/bogodroid/upstream-neo/tools/unity_traces/parsed.json` (e o bruto `jnitrace.json`, 406k linhas).

## 2. Shim de log IL2CPP — raio-X do runtime em execução
O `gen_il2cpp_log_shim.py` parseia o header oficial `il2cpp-api-functions.h` e **gera automaticamente** um wrapper de log pra TODA a API IL2CPP (~300 funções): cada `il2cpp_*` chamado pela libunity aparece no log com argumentos. Teria encurtado muito o debug do `Time.time` congelado do FF9.

```sh
# 1. conseguir o il2cpp-api-functions.h DA VERSÃO do jogo
#    (install do Unity Editor: Editor/Data/il2cpp/libil2cpp/il2cpp-api-functions.h,
#     ou github.com/Unity-Technologies no tag da versão — ver Data/globalgamemanagers pro nº exato)
# 2. gerar o shim
python3 ports/bogodroid/upstream-neo/tools/gen_il2cpp_log_shim.py   # ajustar API_HEADER/OUT_* no topo
```

**Integração no nosso loader:** o gerado é uma tabela de wrappers `nome_il2cpp → wrapper_que_loga_e_encaminha`. No nosso so-loader isso entra como as outras interceptações: resolver os símbolos `il2cpp_*` reais na `libil2cpp.so` carregada e apontar a GOT da `libunity.so` pros wrappers (receita [11 — ponteiros/GOT](../receitas/11-ponteiros-handles-e-hooks.md)). Deixar atrás de env var (`PORT_LOG_IL2CPP=1`) — é verboso demais pra ficar sempre ligado.

> Código **gerado** pela ferramenta é nosso (saída de gerador não herda GPL, como saída de GCC); só a ferramenta em si é GPL e fica no canto vendorado.

## 3. As armadilhas de sempre ao ler log
- **stdout é bufferado** e engana em crash/hang — o que você vê no terminal PARA antes do ponto real. Ler o arquivo de log (`debug.log`) e/ou `fflush` no wrapper (lição do FF7).
- Log verboso muda timing e pode **esconder race** — se o bug some com log ligado, é corrida de thread (ver [troubleshooting 02](02-deadlock-job-system.md)).
- No device, logar pra arquivo em `/storage/roms/ports/<jogo>/` (vfat, espaço de sobra), nunca pro rootfs.

---
*Fluxo recomendado pra port Unity novo: jnitrace no Moto G100 → escrever fake-JNI pela lista → boot seguindo a [receita 12](../receitas/12-unity-bootstrap-render-gc.md) → se travar em runtime, ligar o shim IL2CPP.*
