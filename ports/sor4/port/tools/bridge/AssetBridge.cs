using System; using System.IO; using System.Runtime.CompilerServices;
using Android.Content.Res;
namespace SOR4Bridge {
  public static class AssetBridge {
    public static void Log(string m){ Console.Error.WriteLine("[GLOG] "+m); Console.Error.Flush(); }
    public static string Root = Environment.GetEnvironmentVariable("SOR4_ASSETS") ?? Path.Combine(AppContext.BaseDirectory,"assets");
    static AssetManager _am;
    public static AssetManager GetAssets() { { if(_am==null){ Console.Error.WriteLine("[asset] GetAssets: criando AssetManager uninitialized"); Console.Error.Flush(); _am=(AssetManager)RuntimeHelpers.GetUninitializedObject(typeof(AssetManager)); Console.Error.WriteLine("[asset] GetAssets ok"); Console.Error.Flush(); } return _am; } }
    public static Stream Open(string name) {
      foreach (var cand in new[]{ name, name+".xnb" }) {
        string p = Path.Combine(Root, cand);
        if (File.Exists(p)) { Console.Error.WriteLine("[asset] "+name+" -> "+cand); return File.OpenRead(p); }
      }
      Console.Error.WriteLine("[asset MISS] "+name+" (root="+Root+")");
      throw new FileNotFoundException("asset: "+name);
    }
    public static string[] List(string dir) {
      Console.Error.WriteLine("[asset] List chamado: "+dir); Console.Error.Flush();
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
