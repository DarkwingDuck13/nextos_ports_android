using System; using System.IO; using System.Reflection;
using System.Runtime.Loader;
class Host {
  static void L(string s){ Console.Error.WriteLine("[host] "+s); Console.Error.Flush(); }
  static void Main(){
    string dir = AppContext.BaseDirectory;
    AssemblyLoadContext.Default.Resolving += (ctx, name) => {
      var n = name.Name;
      // System.*/Microsoft.*/netstandard/mscorlib -> deixa o runtime resolver (forwards corretos)
      if (n.StartsWith("System.") || n.StartsWith("Microsoft.") || n=="System" || n=="netstandard" || n=="mscorlib" || n=="WindowsBase") {
        L("skip resolver p/ "+n+" (runtime)"); return null;
      }
      string p = Path.Combine(dir, n + ".dll");
      if (File.Exists(p)) { return ctx.LoadFromAssemblyPath(p); }
      L("NAO resolvido: "+name.FullName); return null;
    };
    AppDomain.CurrentDomain.UnhandledException += (s,e)=>{ L("UNHANDLED: "+e.ExceptionObject); };
    try {
      L("carregando SOR4.dll");
      var sor4 = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(dir,"SOR4.dll"));
      sor4.GetType("CommonLib.utils", true).GetMethod("set_as_main_thread", BindingFlags.Public|BindingFlags.Static).Invoke(null, null);
      L("set_as_main_thread OK");
      var xna = sor4.GetType("CommonLib.xna", true);
      L("CreateGame()...");
      xna.GetMethod("CreateGame", BindingFlags.Public|BindingFlags.Static).Invoke(null, null);
      var game = (Microsoft.Xna.Framework.Game) xna.GetField("game", BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Static).GetValue(null);
      L("Run()...");
      game.Run();
      L("Run retornou");
    } catch (Exception e){ L("EXC: "+e); if(e.InnerException!=null) L("INNER: "+e.InnerException); }
  }
}
