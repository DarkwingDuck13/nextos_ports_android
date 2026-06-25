[assembly: System.Reflection.AssemblyVersion("1.0.0.0")]
namespace Microsoft.Maui.Devices {
    public struct DeviceIdiom {
        private readonly string _name;
        private DeviceIdiom(string name) { _name = name; }
        public static DeviceIdiom Unknown => new DeviceIdiom("Unknown");
        public static DeviceIdiom Phone => new DeviceIdiom("Phone");
        public static DeviceIdiom Tablet => new DeviceIdiom("Tablet");
        public static DeviceIdiom Desktop => new DeviceIdiom("Desktop");
        public static DeviceIdiom TV => new DeviceIdiom("TV");
        public static DeviceIdiom Watch => new DeviceIdiom("Watch");
        public static bool operator ==(DeviceIdiom a, DeviceIdiom b) => a._name == b._name;
        public static bool operator !=(DeviceIdiom a, DeviceIdiom b) => a._name != b._name;
        public override bool Equals(object obj) => obj is DeviceIdiom d && d._name == _name;
        public override int GetHashCode() => _name?.GetHashCode() ?? 0;
        public override string ToString() => _name;
    }
    public static class DeviceInfo {
        public static DeviceIdiom Idiom => DeviceIdiom.Desktop;
    }
}
