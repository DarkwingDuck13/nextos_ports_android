#!/usr/bin/env python3
# Aplica os patches GLES no source do MonoGame 3.8.4 p/ rodar no Mali-450 (GLES2).
# Uso: python3 apply.py <caminho_MonoGame/MonoGame.Framework>
import sys,os
root=sys.argv[1] if len(sys.argv)>1 else "."
def patch(rel, old, new):
    p=os.path.join(root,rel); s=open(p).read()
    assert old in s, f"padrao nao encontrado em {rel}"
    open(p,"w").write(s.replace(old,new,1)); print("patched",rel)

# 1) contexto OpenGL ES (Mali-450 = GLES 2.0) em vez de GL 2.1 desktop
patch("Platform/GraphicsDeviceManager.SDL.cs",
"""            Sdl.GL.SetAttribute(Sdl.GL.Attribute.DoubleBuffer, 1);
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMajorVersion, 2);
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMinorVersion, 1);""",
"""            Sdl.GL.SetAttribute(Sdl.GL.Attribute.DoubleBuffer, 1);
#if GLES
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextProfileMAsl, 4); // SDL_GL_CONTEXT_PROFILE_ES
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMajorVersion, 2);
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMinorVersion, 0);
#else
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMajorVersion, 2);
            Sdl.GL.SetAttribute(Sdl.GL.Attribute.ContextMinorVersion, 1);
#endif""")

# 2) BoundApi = ES no caminho SDL quando GLES
patch("Platform/Graphics/OpenGL.SDL.cs",
"""        static partial void LoadPlatformEntryPoints()
        {
            BoundApi = RenderApi.GL;
        }""",
"""        static partial void LoadPlatformEntryPoints()
        {
#if GLES
            BoundApi = RenderApi.ES;
#else
            BoundApi = RenderApi.GL;
#endif
        }""")

# 3) PolygonMode (desktop-only, nil no GLES) nao deve ser chamado no build hibrido
patch("Platform/Graphics/States/RasterizerState.OpenGL.cs",
"#if WINDOWS || DESKTOPGL",
"#if (WINDOWS || DESKTOPGL) && !GLES")
print("OK - todos os patches aplicados")
