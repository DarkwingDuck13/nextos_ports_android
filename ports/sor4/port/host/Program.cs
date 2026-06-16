using System; using System.IO; using System.Reflection;
using System.Runtime.Loader;
class Host {
  static void L(string s){ Console.Error.WriteLine("[host] "+s); Console.Error.Flush(); }
  static void Main(){
    string dir = AppContext.BaseDirectory;
    AssemblyLoadContext.Default.Resolving += (ctx, name) => {
      string p = Path.Combine(dir, name.Name + ".dll");
      if (File.Exists(p)) return ctx.LoadFromAssemblyPath(p);
      L("NAO resolvido: "+name.FullName); return null;
    };
    AppDomain.CurrentDomain.UnhandledException += (s,e)=>{ L("UNHANDLED: "+e.ExceptionObject); };
    try {
      L("carregando SOR4.dll");
      var sor4 = AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(dir,"SOR4.dll"));
      // registra esta thread como a main thread do jogo (senao asset loader toma caminho errado)
      var utils = sor4.GetType("CommonLib.utils", true);
      utils.GetMethod("set_as_main_thread", BindingFlags.Public|BindingFlags.Static).Invoke(null, null);
      L("set_as_main_thread OK (thread="+System.Threading.Thread.CurrentThread.Name+")");
      // teste isolado pos set_as_main_thread
      try {
        var a2 = Microsoft.Xna.Framework.Game.Activity;
        L("[teste] Activity="+a2.GetType().Name);
        Android.Content.Context cx2 = a2;
        var am3 = cx2.Assets;
        L("[teste] ctx.Assets="+(am3==null?"null":am3.GetType().Name));
        var st3 = am3.Open("gui/mobile/left_filler");
        L("[teste] Open left_filler len="+(st3==null?-1:st3.Length));
      } catch(Exception tx){ L("[teste] EXC: "+tx); }
      // teste DIRETO da chamada que crasha: asset_cache.get<TextureProxy>(path)
      try {
        L("[t2] asset_cache.get<TextureProxy> via reflection...");
        var ac = sor4.GetType("CommonLib.asset_cache", true);
        var tp = sor4.GetType("CommonLib.TextureProxy", true);
        var getM = ac.GetMethod("get", BindingFlags.Public|BindingFlags.Static);
        L("[t2] getM="+getM+" tp="+tp);
        var gm = getM.MakeGenericMethod(tp);
        L("[t2] invocando...");
        var res = gm.Invoke(null, new object[]{ "gui/mobile/left_filler" });
        L("[t2] OK res="+(res==null?"null":res.GetType().Name));
      } catch(Exception t2x){ L("[t2] EXC: "+t2x.GetType().Name+": "+t2x.Message); var ie=t2x.InnerException; while(ie!=null){ L("[t2] INNER: "+ie.GetType().Name+": "+ie.Message); ie=ie.InnerException; } }
      var xna = sor4.GetType("CommonLib.xna", true);
      L("CreateGame()...");
      xna.GetMethod("CreateGame", BindingFlags.Public|BindingFlags.Static).Invoke(null, null);
      var gf = xna.GetField("game", BindingFlags.Public|BindingFlags.NonPublic|BindingFlags.Static);
      var game = (Microsoft.Xna.Framework.Game) gf.GetValue(null);
      L("game="+game+"; Run()...");
      game.Run();
      L("Run retornou");
    } catch (Exception e){ L("EXC: "+e); if(e.InnerException!=null) L("INNER: "+e.InnerException); }
  }
}
