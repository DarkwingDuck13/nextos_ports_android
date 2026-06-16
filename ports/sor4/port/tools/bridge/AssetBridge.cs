using System; using System.IO; using System.Runtime.CompilerServices;
using Android.Content.Res;
namespace SOR4Bridge {
  public static class AssetBridge {
    public static string Root = Environment.GetEnvironmentVariable("SOR4_ASSETS") ?? Path.Combine(AppContext.BaseDirectory,"assets");
    static AssetManager _am;
    public static AssetManager GetAssets() { return _am ??= (AssetManager)RuntimeHelpers.GetUninitializedObject(typeof(AssetManager)); }
    public static Stream Open(string name) {
      foreach (var cand in new[]{ name, name+".xnb", Path.Combine(name) }) {
        string p = Path.Combine(Root, cand);
        if (File.Exists(p)) { Console.Error.WriteLine("[asset] "+name+" -> "+cand); return File.OpenRead(p); }
      }
      Console.Error.WriteLine("[asset MISS] "+name+" (root="+Root+")");
      throw new FileNotFoundException("asset: "+name);
    }
  }
}
