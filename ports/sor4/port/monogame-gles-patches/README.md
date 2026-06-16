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

## Stubs Android (FASE 3)
- `port/tools/stubber/` reescreve corpos de método → default e **retargeta corelib**
  System.Private.CoreLib → System.Runtime (p/ compilar contra os stubs no net9).
- Stubar (refs diretas do SOR4.dll): Mono.Android, Java.Interop, EOSSDK.Android,
  HelpshiftSDKx.Android, _Microsoft.Android.Resource.Designer, SharpFont.Core,
  Xamarin.Android.Google.BillingClient, Xamarin.Firebase.Config,
  Xamarin.Google.Android.Play.Core, Xamarin.GooglePlayServices.{Auth,Base,Basement,Games,Measurement.Api,Tasks}.
- MonoGame compat: `Platform/SOR4Compat.cs` adiciona `AndroidGameActivity : Android.App.Activity`
  (stub) + `Game.Activity`. csproj referencia stubs Mono.Android/Java.Interop.

## Host (port/host/)
- `sor4host`: AssemblyLoadContext.Resolving p/ resolver dlls do dir por nome simples (ignora versão/PKT).
- Boot: carrega SOR4.dll → `CommonLib.xna.CreateGame()` → `((Game)CommonLib.xna.game).Run()`.
  (replica MainActivity.OnCreate sem Android; SetContentView pulado, SDL faz a janela.)
