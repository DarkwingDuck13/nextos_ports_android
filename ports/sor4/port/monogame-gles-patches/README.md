# Patches MonoGame GLES p/ Mali-450 (SoR4 port)

Base: MonoGame 3.8.4 source (tarball). Build híbrido **SDL windowing (DESKTOPGL) + render GLES**.

## Como construir o MonoGame.Framework.dll (GLES, net9, v3.8.3.1)
1. Baixar MonoGame 3.8.4 source + popular submódulos StbImageSharp/StbImageWriteSharp
   (ou usar PackageReference StbImageSharp/StbImageWriteSharp como no csproj).
2. `python3 apply.py <MonoGame>/MonoGame.Framework`
3. Copiar `MonoGame.Framework.SOR4GLES.csproj` p/ `<MonoGame>/MonoGame.Framework/`
4. `dotnet build MonoGame.Framework.SOR4GLES.csproj -c Release -o out`

## Pontos-chave
- csproj define `OPENGL;GLES;...;DESKTOPGL` (híbrido) + StbImage via NuGet (sem STBSHARP_INTERNAL).
- Pede contexto SDL **ES profile 2.0**; BoundApi=ES; FBO via core GLES2.
- Desktop-only (PolygonMode) gateado `&& !GLES`. Outros desktop-only (MapBuffer/DrawBuffer)
  só em GetData/RenderTarget — tratar se crashar.
- Validado: renderiza (Clear) 20 frames no Mali-450 MP via sdl2-compat→SDL3-mali, EXIT=0.
