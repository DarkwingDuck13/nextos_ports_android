# Plano: shim WebGPU(wgpu C API) → GLES2 para Dusklight (Mali-450)

## Por quê
O binário Android do Dusklight (engine Aurora) renderiza via **Dawn/WebGPU**, compilado com os
backends **Vulkan(1050 sym) + Null(156 sym)** apenas — **sem OpenGL ES**. No Mali-450 (sem Vulkan,
GLES2) só roda o backend **Null** (descarta render) → tela preta (título `... [Null]`).

Como o engine fala com o Dawn pela **API C `webgpu.h`** (276 funções `wgpu*` definidas no binário,
chamadas via `@plt` ⇒ **interceptáveis por GOT-hook**, igual fizemos com `Android_JNI_GetEnv`), o
caminho é **interpor essas chamadas e reimplementar WebGPU sobre GLES2**. Bônus: o **Tint**
(tradutor WGSL→GLSL, 253 símbolos) já está embarcado — útil p/ os shaders.

Status: F2 já resolvido (engine roda + cria contexto GL EGL/GLES no Mali + carrega UI). Ver STATUS.md.

## Estratégia
GOT-hook de TODAS as `wgpu*` que o engine chama → nossas implementações em `wgpu_gles2.c`.
Nossas funções devolvem **handles fake** (structs nossas) e mantêm o estado GLES2. O Dawn fica
inerte (bypassado). Reusa o contexto GLES2 que o SDL3-Android já criou no Mali fbdev.

### Sequência mínima de INIT (mapeada do disasm de aurora::webgpu::initialize):
```
wgpuCreateInstance            -> handle instance fake
wgpuInstanceCreateSurface     -> wrap da EGL surface atual (nosso fbdev_window)
wgpuInstanceRequestAdapter    -> callback(adapter fake)   [async; ver wgpuInstanceWaitAny]
wgpuInstanceWaitAny           -> completa os futures pendentes na hora
wgpuAdapterGetInfo/Limits/Features -> reporta limites GLES2 (sem compute/storage)
wgpuAdapterRequestDevice      -> callback(device fake)
wgpuDeviceGetQueue            -> queue fake
wgpuSurfaceGetCapabilities    -> formats = {BGRA8/RGBA8 Unorm}
wgpuSurfaceConfigure          -> guarda w/h/format; usa o default framebuffer (fb0) do GLES2
wgpuDeviceCreateTexture/View/Sampler/BindGroup(Layout)/RenderPipeline/Buffer -> objetos GLES2
```

### Por-frame (de aurora_begin_frame / aurora_end_frame):
```
wgpuSurfaceGetCurrentTexture  -> textura "swapchain" = framebuffer default (FBO 0)
wgpuDeviceCreateCommandEncoder
wgpuCommandEncoderBeginRenderPass -> bind FBO + glClear (loadOp) + glViewport
wgpuRenderPassEncoderSetPipeline/SetBindGroup/SetVertexBuffer/SetIndexBuffer
wgpuRenderPassEncoderDraw[Indexed] -> glDrawArrays/glDrawElements
wgpuRenderPassEncoderEnd
wgpuCommandEncoderFinish / wgpuQueueSubmit -> executa os comandos enfileirados
wgpuSurfacePresent            -> eglSwapBuffers (apresenta no fb0)  ← IMAGEM AQUI
```

## Marcos (cada um = um teste no device, com captura de fb0)
- **M1 — CLEAR COLOR (primeira imagem):** implementar só o caminho até Present fazendo `glClearColor`
  + `glClear` + `eglSwapBuffers` (ignorar pipelines/draws). Stubbar todo o resto retornando handles
  válidos. Meta: **fb0 não-preto (cor sólida) = primeira evidência de imagem**.
- **M2 — quad texturizado / UI:** RenderPipeline + shader (WGSL→GLSL via Tint OU shader simples nosso),
  vertex/index buffers, bind groups (1 textura+sampler) → desenhar a UI de prelaunch (logo.png).
- **M3 — cena 3D:** uniforms, depth, múltiplas pipelines, blend, render targets. Twilight Princess.

## Riscos / notas
- WebGPU é stateless/explícito; GLES2 é stateful. Mapear render pass → bind FBO + estados.
- Sem compute/storage no GLES2 — se o engine exigir compute, stubbar (TP é fixed-ish, deve dar).
- WGSL→GLSL: tentar usar o Tint embarcado; se complicado, interceptar wgpuDeviceCreateShaderModule
  e gerar GLSL ES nós mesmos a partir do WGSL (ou cache).
- Handles fake precisam de refcount coerente (Add/Release) p/ não quebrar o engine.
- A antiga "Route A" (wgpu-compat from-source) NÃO renderizou — aqui temos a vantagem do binário
  já inicializar GL no Mali e do Tint embarcado; focar em M1 (clear) antes de tudo.

## Primeiro passo de código
Criar `src/wgpu_gles2.c` + registrar GOT-hooks (lista das wgpu* via so_find_rel_addr_safe num loop).
Implementar M1 (instance/adapter/device/queue/surface + present=swap+clear). Testar fb0.
