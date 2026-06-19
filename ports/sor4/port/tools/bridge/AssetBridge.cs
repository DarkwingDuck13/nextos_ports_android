using System; using System.IO; using System.Runtime.CompilerServices;
using Android.Content.Res;
namespace SOR4Bridge {
  public static class AssetBridge {
    // SOR4/Mali-450: o log de SUCESSO por-asset rodava ~4400x/sessao (cada Open).
    // Gateado por SOR4_MGLOG=1 (default off); as linhas de MISS/ERRO seguem sempre.
    static readonly bool Dbg = Environment.GetEnvironmentVariable("SOR4_MGLOG") == "1";
    public static void Log(string m){ if(Dbg){ Console.Error.WriteLine("[GLOG] "+m); Console.Error.Flush(); } }
    public static string Root = Environment.GetEnvironmentVariable("SOR4_ASSETS") ?? Path.Combine(AppContext.BaseDirectory,"assets");
    static AssetManager _am;
    public static AssetManager GetAssets() { { if(_am==null){ if(Dbg) Console.Error.WriteLine("[asset] GetAssets: criando AssetManager uninitialized"); _am=(AssetManager)RuntimeHelpers.GetUninitializedObject(typeof(AssetManager)); if(Dbg) Console.Error.WriteLine("[asset] GetAssets ok"); } return _am; } }
    public static Stream Open(string name) {
      foreach (var cand in new[]{ name, name+".xnb" }) {
        string p = Path.Combine(Root, cand);
        if (File.Exists(p)) { if(Dbg) Console.Error.WriteLine("[asset] "+name+" -> "+cand); return File.OpenRead(p); }
      }
      // fallback 1: path absoluto (o jogo as vezes manda CWD-path sem a '/' inicial, ex: fontes)
      foreach (var p in new[]{ name, "/"+name }) {
        if (Path.IsPathRooted(p) && File.Exists(p)) { if(Dbg) Console.Error.WriteLine("[asset] "+name+" -> ABS "+p); return File.OpenRead(p); }
      }
      // fallback 2: por basename dentro de Root (fontes .ttf/.otf ficam na raiz de gameassets)
      string bn = Path.GetFileName(name);
      foreach (var cand in new[]{ bn, bn+".xnb" }) {
        string p = Path.Combine(Root, cand);
        if (!string.IsNullOrEmpty(bn) && File.Exists(p)) { if(Dbg) Console.Error.WriteLine("[asset] "+name+" -> BN "+cand); return File.OpenRead(p); }
      }
      Console.Error.WriteLine("[asset MISS] "+name+" (root="+Root+")");
      throw new FileNotFoundException("asset: "+name);
    }
    public static string[] List(string dir) {
      if(Dbg){ Console.Error.WriteLine("[asset] List chamado: "+dir); Console.Error.Flush(); }
      try {
        string p = Path.Combine(Root, dir ?? "");
        if (!Directory.Exists(p)) { Console.Error.WriteLine("[asset List MISS dir] "+dir); return new string[0]; }
        var names = new System.Collections.Generic.List<string>();
        foreach (var f in Directory.GetFileSystemEntries(p)) names.Add(Path.GetFileName(f));
        return names.ToArray();
      } catch (Exception e) { Console.Error.WriteLine("[asset List ERR] "+dir+": "+e.Message); return new string[0]; }
    }
  }
}
