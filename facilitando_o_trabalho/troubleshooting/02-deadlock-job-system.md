# 🧊 Deadlock do Job-System (Unity / engines multi-thread)

Sintoma típico: o jogo abre, mostra menu, entra na fase — e **congela ao andar/carregar**, com a CPU ora em 100%, ora ociosa. Quase sempre é o **job-system** da engine batendo na nossa ponte de semáforo/pthread.

## 1. O mecanismo
Engines (Unity Baselib, motores próprios) usam um pool de workers coordenado por **semáforos**. Sob Bionic, `sem_init`/`sem_post`/`sem_wait` têm semântica que a nossa ponte precisa replicar **exatamente**. Erro comum:
* Um produtor faz `post` antes de alguém esperar; nosso shim "reinicia" o semáforo e **perde** o post → o worker dorme pra sempre em `sem_wait` que ninguém mais vai acordar. **Deadlock.**

## 2. O fix do "só reinicia se drenado" (lição RE4 / SOR4)
No `sh_sem_init` (ou equivalente), **só** reinicialize o semáforo se ele estiver **drenado**; se houver `post` pendente, **preserve** a contagem. Isso transformou `SEMBREAK 40→0` (RE4): de 40 travas por boot para zero.

## 3. Diagnóstico no device (gdb)
1. Pare o ES, rode sob `gdb --args ./jogo`.
2. No freeze, `Ctrl-C` → `thread apply all bt`.
3. Procure o par produtor/consumidor: um worker parado em `pthread_cond_wait`/`sem_wait` (consumidor que não acorda) e descubra **quem** deveria postar. No RE4 os endereços eram `acquire=libunity+0x1567e4`, `release=+0x15690c`, `creator=+0x14f528`.
4. Olhe `/proc/<tid>/stack` — thread em estado **D** (uninterruptible) = travada em I/O ou futex.

## 4. O trade-off fatal (quando não há fix fácil)
Cuidado: mexer em `NPROC`/prioridade pode trocar um problema por outro. No RE4: `NPROC=1` → 0.14 fps; `NPROC=3` → crash; `NPROC=0` → deadlock; semáforo "rápido" → crash; `SEMBREAK off` → freeze. Quando todo ajuste leva a um beco, o fix de verdade é **RE profundo** da Baselib (a corrida exata) — documente e decida se vale.

## 5. Variante "handshake de startup" (Elderand)
Às vezes não é o pool inteiro, e sim **um** handshake único de inicialização (ex.: o `gfxdeviceworker`/Choreographer espera um produtor que só roda no startup). O nosso `doFrame` via JNI não dispara o produtor **nativo**. Aí o caminho é achar o produtor startup-only (via frida na bancada) e alimentá-lo — não adianta tunar threads.

---
*Resumo: congelou ao carregar = job-system. Preserve `post` pendente no sem_init, ache o par produtor/consumidor no gdb, e desconfie de trade-offs que só empurram o bug.*
