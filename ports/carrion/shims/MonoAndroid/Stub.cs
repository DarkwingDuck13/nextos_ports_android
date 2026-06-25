// Minimal desktop stub of Mono.Android — only the surface Carrion/PlatformAPI reference.
// AssetManager is backed by the real filesystem (assets root). Everything else is no-op.
using System;
using System.IO;

namespace Android.Runtime {
    public interface IJavaObject { IntPtr Handle { get; } }
    public enum JniHandleOwnership { DoNotTransfer = 0, TransferLocalRef = 1, TransferGlobalRef = 2 }

    [AttributeUsage(AttributeTargets.All, AllowMultiple = true)]
    public sealed class RegisterAttribute : Attribute {
        public RegisterAttribute(string name) { Name = name; }
        public RegisterAttribute(string name, string signature, string connector) { Name = name; }
        public string Name { get; set; }
    }
    [AttributeUsage(AttributeTargets.All, AllowMultiple = true)]
    public sealed class NamespaceMappingAttribute : Attribute {
        public string Managed { get; set; }
        public string Java { get; set; }
    }
    public struct XAPeerMembers {
        public XAPeerMembers(string jniName, Type managed) { }
    }
    public static class JNINativeWrapper {
        public static Delegate CreateDelegate(Delegate dlg) => dlg;
    }
    public static class JNIEnv {
        public static IntPtr Handle => IntPtr.Zero;
        public static void DeleteLocalRef(IntPtr handle) { }
        public static IntPtr NewString(string text) => IntPtr.Zero;
        public static IntPtr NewString(char[] text, int length) => IntPtr.Zero;
        public static IntPtr NewArray(int[] a) => IntPtr.Zero;
        public static IntPtr NewArray(string[] a) => IntPtr.Zero;
        public static IntPtr NewArray<T>(T[] a) => IntPtr.Zero;
        public static T[] GetArray<T>(IntPtr ptr) => Array.Empty<T>();
        public static void CopyArray<T>(IntPtr src, T[] dest) { }
        public static void CopyArray<T>(T[] src, IntPtr dest) { }
        public static void CopyArray(IntPtr src, int[] dest) { }
        public static void CopyArray(IntPtr src, string[] dest) { }
    }
}

namespace Java.Lang {
    public class Object : Android.Runtime.IJavaObject, IDisposable {
        public IntPtr Handle { get; private set; }
        public void SetHandle(IntPtr value, Android.Runtime.JniHandleOwnership transfer) { Handle = value; }
        public static T GetObject<T>(IntPtr jnienv, IntPtr handle, Android.Runtime.JniHandleOwnership transfer) where T : class => null;
        public static T GetObject<T>(IntPtr handle, Android.Runtime.JniHandleOwnership transfer) where T : class => null;
        public virtual void Dispose() { }
    }
    public static class JavaSystem {
        public static void LoadLibrary(string libName) {
            // On desktop the native libs are loaded by the runtime/NativeLibrary; no-op here.
            Console.Error.WriteLine($"[Mono.Android stub] JavaSystem.LoadLibrary({libName}) ignored");
        }
    }
}

namespace Java.Interop {
    public static class TypeManager {
        public static string LookupTypeMapping(string[] mappings, string javaType) => null;
        public static void RegisterPackages(string[] packages, System.Converter<string, Type>[] lookup) { }
    }
}

namespace Android.Content.Res {
    // Filesystem-backed AssetManager. Root resolved from CARRION_ASSETS or <basedir>/assets.
    public class AssetManager : Java.Lang.Object {
        internal static string Root = ResolveRoot();
        static string ResolveRoot() {
            var env = Environment.GetEnvironmentVariable("CARRION_ASSETS");
            if (!string.IsNullOrEmpty(env) && Directory.Exists(env)) return env;
            var bd = Path.Combine(AppContext.BaseDirectory, "assets");
            return Directory.Exists(bd) ? bd : AppContext.BaseDirectory;
        }
        public virtual string[] List(string path) {
            try {
                var full = Path.Combine(Root, path ?? "");
                if (!Directory.Exists(full)) return Array.Empty<string>();
                var dirs = Directory.GetDirectories(full);
                var files = Directory.GetFiles(full);
                var res = new string[dirs.Length + files.Length];
                int i = 0;
                foreach (var d in dirs) res[i++] = Path.GetFileName(d);
                foreach (var f in files) res[i++] = Path.GetFileName(f);
                return res;
            } catch { return Array.Empty<string>(); }
        }
        public virtual Stream Open(string path) {
            var full = Path.Combine(Root, path ?? "");
            return new FileStream(full, FileMode.Open, FileAccess.Read);
        }
    }
}

namespace Android.Content {
    public abstract class Context : Java.Lang.Object {
        private static readonly Android.Content.Res.AssetManager _assets = new Android.Content.Res.AssetManager();
        public virtual Android.Content.Res.AssetManager Assets => _assets;
    }
    public class ContextWrapper : Context { }
}

namespace Android.Views {
    public class ContextThemeWrapper : Android.Content.ContextWrapper { }

    public class InputDevice : Java.Lang.Object {
        public virtual int ControllerNumber => 0;
        public virtual bool IsExternal => false;
        public virtual string Name => "";
        public static InputDevice GetDevice(int id) => null;
        public static int[] GetDeviceIds() => Array.Empty<int>();
    }
}

namespace Android.Media {
    public class AudioDeviceInfo : Java.Lang.Object { }
}

namespace Android.App {
    public class Application : Android.Content.ContextWrapper {
        private static readonly DesktopContext _ctx = new DesktopContext();
        public static Android.Content.Context Context => _ctx;
        private sealed class DesktopContext : Android.Content.Context { }
    }

    public class Activity : Android.Views.ContextThemeWrapper {
        public virtual bool MoveTaskToBack(bool nonRoot) => true;
    }
}
