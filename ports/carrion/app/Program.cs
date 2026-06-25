using System;
public static class CarrionMain {
    public static void Main(string[] args) {
        Console.Error.WriteLine("[carrion] starting; assets=" + (Environment.GetEnvironmentVariable("CARRION_ASSETS")??"(default)"));
        if (Environment.GetEnvironmentVariable("CARRION_FCE") == "1") {
            AppDomain.CurrentDomain.FirstChanceException += (s, e) => {
                var ex = e.Exception;
                // only log interesting ones (skip noisy expected ones)
                Console.Error.WriteLine("[FCE] " + ex.GetType().Name + ": " + ex.Message);
                var st = ex.StackTrace;
                if (st != null) {
                    foreach (var line in st.Split('\n')) {
                        if (line.Contains("Monster") || line.Contains("Carrion") || line.Contains("MonoGame") || line.Contains("Microsoft.Xna"))
                            Console.Error.WriteLine("    " + line.Trim());
                    }
                }
            };
        }
        AppDomain.CurrentDomain.UnhandledException += (s, e) => {
            Console.Error.WriteLine("[UNHANDLED] " + e.ExceptionObject);
        };
        try {
            var activity = new Microsoft.Xna.Framework.AndroidGameActivity();
            Console.Error.WriteLine("[carrion] creating EngineSystem...");
            var game = new Monster.EngineSystem(activity);
            Console.Error.WriteLine("[carrion] EngineSystem created; Run()...");
            game.Run();
            Console.Error.WriteLine("[carrion] Run() returned.");
        } catch (Exception ex) {
            Console.Error.WriteLine("[carrion] FATAL: " + ex);
            throw;
        }
    }
}
