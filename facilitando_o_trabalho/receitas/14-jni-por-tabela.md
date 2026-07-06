# 🧩 JNI por Tabela (`nx_jni`) — chega de `switch` gigante

> **O que muda:** hoje cada port escreve um `jni_shim.c` à mão, com um `switch`
> enorme em `CallObjectMethodV` etc. Isso é lento de escrever e fácil de errar.
> O `nx_jni` troca isso por uma **tabela de dados**: você declara os métodos/campos
> que o jogo chama, e o core despacha. Menos código por port, menos bug, bring-up
> mais rápido. É a **nossa** versão (clean-room), pensada pro so-loader multiarch
> do NextOS — inspirada na ideia do FalsoJNI (Vita), mas sem código dele e sem
> amarra de plataforma. Roda igual em **Mali-450 (ARM64)** e **PC (x86_64)**.

Arquivos: [`kit_essencial/core/nx_jni.h`](../kit_essencial/core/nx_jni.h) + [`nx_jni.c`](../kit_essencial/core/nx_jni.c).

## A ideia em 30 segundos

```c
#include "nx_jni.h"

/* 1. handlers: recebem nx_ctx*, devolvem nx_jval */
static nx_jval m_getPackage(nx_ctx *c) { return nx_str(c, c->cfg->package_name); }
static nx_jval m_hasFeature(nx_ctx *c) { (void)nx_arg_obj(c); return nx_bool(0); }

/* 2. tabela: nome, assinatura, tipo de retorno, handler */
static const nx_method METHODS[] = {
  { "getPackageName", "()Ljava/lang/String;",          NX_OBJ,  m_getPackage },
  { "hasSystemFeature","(Ljava/lang/String;)Z",        NX_BOOL, m_hasFeature },
  NX_METHOD_END
};
static const nx_field FIELDS[] = { NX_FIELD_END };

/* 3. config do port */
static const nx_jni_config JNI = { "jp.konami.castlevania", 110, METHODS, FIELDS };
```

## Como plugar na vtable do seu port

O `nx_jni` **não** reimplementa a vtable inteira do `JNIEnv` (230+ slots — frágil).
Ele é a **camada de dispatch** que a sua vtable já-testada chama. No seu `jni_shim.c`:

```c
/* GetMethodID -> devolve um ID estável da tabela */
static void *my_GetMethodID(void *env, void *clazz, const char *n, const char *s) {
    (void)env; (void)clazz; return nx_method_id(&JNI, n, s);
}
/* CallObjectMethodV -> despacha pela tabela */
static void *my_CallObjectMethodV(void *env, void *obj, void *mid, va_list ap) {
    return nx_dispatch(&JNI, env, obj, mid, &ap, NULL).l;
}
static jint my_CallIntMethodV(void *env, void *obj, void *mid, va_list ap) {
    return nx_dispatch(&JNI, env, obj, mid, &ap, NULL).i;
}
/* ... idem CallBoolean/Long/Float/Void e GetFieldID/Get<T>Field via nx_dispatch_field */
```

Pronto. Adicionar suporte a um método novo = adicionar **uma linha** na tabela.

## Regras de ouro

- **Nome desconhecido não quebra o boot.** `nx_method_id` sempre devolve um ID
  válido; se o método não está na tabela, `nx_dispatch` cai num **default seguro**
  (0/NULL) e loga. Você só adiciona o que o jogo realmente exige pra avançar.
- **Bring-up:** rode com `NX_JNI_VERBOSE=1` pra ver TODA chamada JNI (nome +
  assinatura). Cada "nao-resolvido" no log é um candidato a virar linha na tabela.
- **Argumentos:** leia na ordem da assinatura com `nx_arg_obj/int/long/dbl`.
- **Strings:** devolva com `nx_str(c, "texto")`; leia argumentos-string com
  `nx_cstr(nx_arg_obj(c))`.
- **Static vs instância:** o mesmo handler serve; `c->obj` é o `this` (instância)
  ou a classe (static). Use se precisar distinguir.

## O que continua manual (de propósito)

O `nx_jni` cobre o **dispatch de métodos/campos** — o miolo repetitivo. Coisas que
ainda são por-port porque variam muito: montar a vtable (copie do template),
`FindClass`/proxies de interface, `NewObjectArray`, e casos com efeito colateral
pesado (Handler.post que roda Runnable, AssetManager.open). Comece pela tabela e
só desça pra vtable manual no que sobrar.

## Migração de um port existente

1. Crie `METHODS[]`/`FIELDS[]` e vá enchendo com o que o `switch` atual trata.
2. Aponte `GetMethodID`/`Call*` pro `nx_*`.
3. Rode com `NX_JNI_VERBOSE=1`; qualquer "nao-resolvido" que faltar → nova linha.
4. Apague o `switch` antigo quando a tabela cobrir tudo.

Não precisa migrar tudo de uma vez: os dois convivem (o que não estiver na tabela
cai no seu código antigo, se você chamar `nx_dispatch` só como fallback).
