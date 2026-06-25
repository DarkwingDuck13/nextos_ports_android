// Local desktop-port shim (NOT part of MonoGame; never submitted upstream).
// Carrion's EngineSystem ctor takes Microsoft.Xna.Framework.AndroidGameActivity.
// Provide a minimal type in the MonoGame.Framework assembly deriving from the
// Mono.Android desktop stub's Android.App.Activity (which exposes MoveTaskToBack).
namespace Microsoft.Xna.Framework
{
    public class AndroidGameActivity : global::Android.App.Activity
    {
    }
}
