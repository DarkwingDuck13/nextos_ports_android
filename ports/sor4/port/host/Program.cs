using System; using System.IO; using System.Reflection;
using System.Runtime.Loader;
class Host {
  static void L(string s){ Console.Error.WriteLine("[host] "+s); Console.Error.Flush(); }
  static void Main(){
    string dir = AppContext.BaseDirectory;
    // resolve qualquer assembly do diretorio do app por nome simples (ignora versao/PKT)
    AssemblyLoadContext.Default.Resolving += (ctx, name) => {
      string p = Path.Combine(dir, name.Name + ".dll");
      if (File.Exists(p)) { L("resolve "+name.Name+" -> "+p); return ctx.LoadFromAssemblyPath(p); }
      L("NAO resolvido: "+name.FullName); return null;
    };
    AppDomain.CurrentDomain.UnhandledException += (s,e)=>{ L("UNHANDLED: "+e.ExceptionObject); };
    try {
      L("=== TESTE Game.Activity.Assets ===");
      try {
        var act = Microsoft.Xna.Framework.Game.Activity;
        L("act = "+(act==null?"NULL":act.GetType().FullName));
        var am = act.Assets;
        L("Assets = "+(am==null?"NULL":am.GetType().FullName));
        L("am.Open test...");
        try { var st = am.Open("gui/mobile/title_screen"); L("Open ok len="+(st==null?-1:st.Length)); } catch(Exception ox){ L("Open exc: "+ox.GetType().Name+" "+ox.Message); }
        L("test via Context slot (como o jogo)...");
        try { Android.Content.Context ctx = act; var am2 = ctx.Assets; L("ctx.Assets = "+(am2==null?"NULL":am2.GetType().FullName)); } catch(Exception cx){ L("ctx.Assets EXC: "+cx); }
      } catch (Exception tx) { L("TESTE EXC: "+tx); }
      L("carregando SOR4.dll");
      var sor4 = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(dir,"SOR4.dll"));
      var xna = sor4.GetType("CommonLib.xna", true);
      L("CommonLib.xna ok; CreateGame()...");
      xna.GetMethod("CreateGame", BindingFlags.Public|BindingFlags.Static).Invoke(null, null);
      L("CreateGame ok; pegando game");
      var gf = xna.GetField("game", BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Static);
      var game = (Microsoft.Xna.Framework.Game) gf.GetValue(null);
      L("game="+game+"; Run()...");
      game.Run();
      L("Run retornou (saiu)");
    } catch (Exception e){ L("EXC: "+e); if(e.InnerException!=null) L("INNER: "+e.InnerException); }
  }
}
