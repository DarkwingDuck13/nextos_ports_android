# 🧷 Ponteiros: handles, GOT/PLT e trampolins

Se há um conceito que atravessa **todo** o so-loader, é o ponteiro. O jogo Android e o nosso runtime Linux só conversam porque concordamos sobre o que cada ponteiro significa. Esta receita junta as técnicas centradas em ponteiro que aparecem em vários ports.

## 1. Ponteiro como handle (a base da ponte pthread)
A Bionic guarda um `pthread_mutex_t` em 40 bytes; a Glibc em 48. Em vez de traduzir campo a campo, tratamos os bytes que o jogo nos deu como **storage de um ponteiro (8 B)**: alocamos o mutex real da Glibc no heap e escrevemos **o endereço dele** dentro do espaço bionic.
* No `lock`/`unlock`, lemos esse ponteiro e chamamos a Glibc com segurança.
* Como distinguir "ainda não inicializado" de "já tem ponteiro"? O inicializador estático bionic (`PTHREAD_MUTEX_INITIALIZER`) é um **valor mágico pequeno**; um ponteiro de heap é **grande**. Se o storage parece mágico, inicializamos sob demanda. (Ver `kit_essencial/core/pthread_bridge.c`.)
* Vale igual para `cond`, `rwlock`, `once`. Um **hashmap de ponteiros** mapeia handle-do-jogo → objeto-real.

## 2. Por que `pthread_t` "simplesmente funciona"
No ARM64, tanto Bionic quanto Glibc representam `pthread_t` como **um ponteiro opaco de 8 bytes**. Os tamanhos batem → `pthread_self()`/`pthread_create()` costumam funcionar **sem ponte**. É a exceção feliz: quando a representação já é um ponteiro do mesmo tamanho, não há ABI a traduzir.

## 3. GOT/PLT: resolver import = escrever um ponteiro de função
Carregar o `.so` é, no fundo, preencher a **GOT** (Global Offset Table) com os ponteiros certos. Para cada símbolo que o jogo importa (`malloc`, `glDrawArrays`, `__android_log_print`...), escrevemos na GOT o endereço da nossa implementação (real da libc, shim, ou stub). É assim que `imports.gen.c` "religa" o jogo ao nosso mundo.
* **Stub** = ponteiro pra uma função vazia (quando o jogo não precisa do símbolo pra abrir).
* **Shim** = ponteiro pra um wrapper nosso (ex.: `my_sigaction`, `getenv`).
* Trocar o comportamento de uma função do jogo = sobrescrever **um ponteiro** na GOT (hook por GOT, o jeito limpo).

## 4. Hook por trampolim quando não dá pra mexer na GOT (a lição do Bully)
Às vezes precisamos interceptar uma função **interna** do `.so` (não um import) — ex.: colisão com `NvAPK` no Bully. Aí o hook é por **trampolim**: sobrescrevemos as primeiras instruções da função com um salto pro nosso código e guardamos as instruções originais num **pool de trampolins** (cada entrada é executável e tem o seu ponteiro). O `hook_arm64` gerencia esse pool para que vários hooks coexistam sem se pisar.
* Cuidado: o pool precisa estar em memória **executável** (`mmap` com `PROT_EXEC`) e perto o suficiente pro alcance do salto ARM64.

## 5. Objetos fake do JNI são só ponteiros com significado combinado
No Fake JNI, quando o jogo chama `getAssets()` ou `getClassLoader()`, devolvemos um **ponteiro fake** que *nós* sabemos interpretar depois. O jogo trata como opaco; nós, quando ele devolve esse ponteiro numa chamada seguinte, reconhecemos qual objeto era. É um handle, não um objeto Java real.

## 6. Relocações: o carregador é uma máquina de escrever ponteiros
Antes de o jogo rodar, o nosso `so_util` aplica as **relocações** do `.so` — e relocação é, literalmente, *escrever o ponteiro certo num lugar*:
* **`R_AARCH64_RELATIVE`:** soma a base de carga a um offset → ponteiro absoluto (auto-referências do próprio `.so`).
* **`R_AARCH64_GLOB_DAT` / `R_AARCH64_JUMP_SLOT`:** escrevem na GOT o ponteiro do símbolo importado (dados / função).
* **`R_AARCH64_ABS64`:** ponteiro absoluto de 64 bits para um símbolo.
* **Armadilha real (Castlevania SOTN):** alguns `ABS64` apontam para símbolos **UNDEF** (externos que não existem no nosso mundo). Se você não resolver, escreve `NULL` → o jogo segue o ponteiro → SIGSEGV. **Fix:** resolver esses imports UNDEF (libc/shim/stub) **na tabela combinada** antes de aplicar a relocação, em vez de deixar zero.

## 7. O canário e o TLS: ponteiros que o jogo lê "por baixo"
O `.so` bionic espera estruturas que ele acessa via **TLS** (`tpidr_el0`), não por argumento:
* **Stack canary (SOTN):** o prólogo de cada função lê o *stack guard* em `tpidr_el0 + 0x28` (layout da Bionic). Na Glibc o canário fica noutro offset → o jogo lê lixo, compara no epílogo e **aborta** (`__stack_chk_fail`). **Fix:** montar um bloco TLS no formato bionic e pôr um canário válido em `+0x28`, com o `tpidr` apontando pra ele.
* **`stdio __sF` (SOTN):** a Bionic expõe `stdin/stdout/stderr` como uma **tabela de ponteiros** (`__sF`). O jogo pega `&__sF[1]` etc. Se esse ponteiro não existir/estiver errado, qualquer `fprintf` quebra. **Fix:** fornecer a tabela `__sF` com ponteiros válidos pros nossos `FILE*`.
* **`dlopen` self-ref (SOTN):** o jogo faz `dlopen(NULL)`/`dlopen("libitself.so")` esperando **um handle** que resolva os próprios símbolos. Devolvemos um ponteiro de handle que o nosso `dlsym` sabe consultar contra a tabela combinada.

> Padrão comum: o jogo nunca te passa esses ponteiros — ele os **lê de um lugar combinado** (TLS, tabela global, GOT). Achar *qual* ponteiro ele lê e *de onde* é metade do trabalho de um port que "crasha cedo sem motivo".

## 8. Ponteiros no diagnóstico de crash
PC e LR no crash handler são **ponteiros dentro do `.so`**. Subtraindo o `text_base` viram **offsets** que você casa com `nm`/`objdump`. O stack scan procura, na pilha, valores que *parecem ponteiros de retorno* para dentro do `.text` do jogo — reconstruindo o caminho. (Ver [troubleshooting 01](../troubleshooting/01-diagnostico-de-crash.md).)

---
*Resumo: handle = ponteiro com significado combinado (mutex, JNI), import = ponteiro na GOT, hook interno = trampolim num pool executável, e todo crash se lê como ponteiro−text_base. Errar o tamanho/significado de um ponteiro é a causa-raiz da maioria dos corrompimentos de memória.*
