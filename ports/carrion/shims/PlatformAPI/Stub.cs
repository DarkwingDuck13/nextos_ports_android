// Clean-room desktop stub of PlatformAPI — replaces the Android/Steam/GOG backends.
// No Google/Steam/JNI. Storage is filesystem-backed so save/config work.
using System;
using System.IO;
[assembly: System.Reflection.AssemblyVersion("1.0.0.0")]

namespace PlatformAPI {

    public enum PlatformLanguage {
        English = 0, Polish = 1, German = 2, Spanish = 3, French = 4,
        BrazilianPortuguese = 5, Russian = 6, Japanese = 7, Korean = 8,
        TraditionalChinese = 9, SimplifiedChinese = 10
    }
    public enum StorageCategory { SAVEGAME = 0, CONFIG = 1 }
    public enum PlatformAchievement {
        BEAT_LAB = 0, BEAT_JUNKYARD = 1, BEAT_MINES = 2, BEAT_JUNKYARD2 = 3,
        BEAT_JUNGLE = 4, BEAT_WATER = 5, BEAT_BUREAU = 6, BEAT_WATER2 = 7,
        BEAT_NUCLEAR = 8, BEAT_WEAPONS = 9, BEAT_BUNKER = 10, BREAK_FREE = 11,
        FIRST_HEAD_RIPPED_OFF = 12, FIRST_HUMAN_EATEN = 13, FIND_FIRST_SECRET = 14,
        FIND_3_SECRETS = 15, FIND_6_SECRETS = 16, FIND_ALL_SECRETS = 17,
        BEAT_SEQUENCE0 = 18, BEAT_SEQUENCE1 = 19, BEAT_SEQUENCE2 = 20, COUNT = 21
    }
    public enum ItemVisibility { Public = 0, FriendsOnly = 1, Private = 2, Unlisted = 3 }
    public enum ItemUploadStatus { Invalid = 0, Preparing = 1, Uploading = 2, Finalizing = 3 }
    public enum UGCError { NO_ERROR = 0, CREATE_FAIL = 1, UPDATE_START_FAIL = 2, UPDATE_FAIL = 3, UPLOAD_FAIL = 4, SUBMIT_FAIL = 5 }

    public delegate void OnLogMessage(string message);
    public delegate void OnUserChange(User newUser);
    public delegate void OnUserCanceled();
    public delegate void OnUGCError(UGCError error);
    public delegate void OnCreateItem(bool success);
    public delegate void OnSubmitItem(bool success);

    public class User {
        public bool IsValid { get; protected set; }
        public string Name { get; protected set; } = "Player";
        public string DisplayName { get; protected set; } = "Player";
        public virtual void SetUser(string name, string displayName, bool isValid) {
            Name = name; DisplayName = displayName; IsValid = isValid;
        }
    }

    public abstract class Achievements {
        public abstract void RefreshAchievements();
        public abstract void ClearAchievements();
        public abstract void UnlockAchievement(PlatformAchievement achievement);
    }

    public abstract class UGC {
        public abstract bool IsUpdatingItem { get; }
        public abstract bool Supported { get; }
        public abstract void CreateNewMod();
        public abstract void StartModUpdate(ulong modId);
        public abstract void SetModMetadata(string metadata);
        public abstract void SetModTitle(string title);
        public abstract void SetModDescription(string description);
        public abstract void SetModTags(string[] tags);
        public abstract void SetModVisibility(ItemVisibility visibility);
        public abstract void SetModContentDirectory(string dir);
        public abstract void SetModPreviewImage(string image);
        public abstract void CleanUpModUpdate();
        public abstract void SubmitModUpdate(string changeNote);
        public abstract ItemUploadStatus GetUploadStatus(ref ulong done, ref ulong total);
        public abstract string[] GetSubscribedMods();
    }

    public abstract class StorageContext : IDisposable {
        public abstract int ItemsCount { get; }
        public abstract BinaryReader GetStorageReader(int saveItem, ref int totalSize);
        public abstract BinaryWriter GetStorageWriter(int saveItem);
        public abstract void Dispose();
    }

    public abstract class Storage {
        public abstract StorageContext CreateReadContext(StorageCategory category, string[] storageItemIds);
        public abstract StorageContext CreateWriteContext(StorageCategory category, string[] storageItemIds);
        public abstract bool StorageItemExists(StorageCategory category, string storageItemId);
        public virtual void SetModMode(string mod) { }
    }

    public class StorageContextInReadModeException : Exception { }
    public class StorageContextInWriteModeException : Exception { }
    public class StorageContextAlreadyInUseException : Exception { }
    public class StorageContextInvalidSaveItemException : Exception { }

    public abstract class Platform {
        protected Platform() { }

        private OnUserChange _onUserChange;
        private OnUserCanceled _onUserCanceled;
        private static OnLogMessage _onLogMessage;
        public event OnUserChange OnUserChange { add { _onUserChange += value; } remove { _onUserChange -= value; } }
        public event OnUserCanceled OnUserCanceled { add { _onUserCanceled += value; } remove { _onUserCanceled -= value; } }
        public static event OnLogMessage OnLogMessage { add { _onLogMessage += value; } remove { _onLogMessage -= value; } }

        public abstract string Name { get; }
        public abstract bool IsStorageLoading { get; }
        public abstract bool IsChangingUserProfile { get; }
        public abstract Achievements Achievements { get; }
        public abstract User CurrentUser { get; }
        public abstract Storage Storage { get; }
        public abstract UGC UGC { get; }
        public abstract void ChangeUser(int playerIndex);
        public abstract void Update();
        public abstract void Shutdown();
        public abstract bool IsGamepadPaired(int playerIndex);
        public abstract PlatformLanguage DetectLanguage();
        public abstract bool CanRestart { get; }
        public abstract void Restart();

        public virtual DateTime PlatformLocalTime => DateTime.Now;
        public virtual bool IsPurchased() => true;
        public virtual bool IsAuthenticatedOrGuestMode(ref bool failed) { failed = false; return true; }

        protected void InvokeOnUserChange() { _onUserChange?.Invoke(CurrentUser); }
        protected void InvokeOnUserCanceled() { _onUserCanceled?.Invoke(); }
        protected static DateTime UnixTimeStampToDateTime(double unixTimeStamp)
            => new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddSeconds(unixTimeStamp).ToLocalTime();
        private static void Log(string message) { _onLogMessage?.Invoke(message); }
        public static void InitializeInternals() { }
        public static Platform CreatePlatform() => new PlatformAPI.Android.PlatformAndroid(null);
    }

    // ---- Concrete desktop implementations (filesystem-backed) ----

    internal sealed class StubAchievements : Achievements {
        public override void RefreshAchievements() { }
        public override void ClearAchievements() { }
        public override void UnlockAchievement(PlatformAchievement achievement) {
            Console.Error.WriteLine($"[PlatformAPI] achievement unlocked: {achievement}");
        }
    }

    internal sealed class StubUGC : UGC {
        public override bool IsUpdatingItem => false;
        public override bool Supported => false;
        public override void CreateNewMod() { }
        public override void StartModUpdate(ulong modId) { }
        public override void SetModMetadata(string metadata) { }
        public override void SetModTitle(string title) { }
        public override void SetModDescription(string description) { }
        public override void SetModTags(string[] tags) { }
        public override void SetModVisibility(ItemVisibility visibility) { }
        public override void SetModContentDirectory(string dir) { }
        public override void SetModPreviewImage(string image) { }
        public override void CleanUpModUpdate() { }
        public override void SubmitModUpdate(string changeNote) { }
        public override ItemUploadStatus GetUploadStatus(ref ulong done, ref ulong total) { done = 0; total = 0; return ItemUploadStatus.Invalid; }
        public override string[] GetSubscribedMods() => Array.Empty<string>();
    }

    internal sealed class FileStorageContext : StorageContext {
        private readonly string[] _paths;
        private readonly bool _write;
        private BinaryReader _r; private BinaryWriter _w; private FileStream _fs;
        public FileStorageContext(string[] paths, bool write) { _paths = paths; _write = write; if (write) foreach (var p in paths) { var d = Path.GetDirectoryName(p); if (!string.IsNullOrEmpty(d)) Directory.CreateDirectory(d); } }
        public override int ItemsCount { get { int n = 0; foreach (var p in _paths) if (File.Exists(p)) n++; return n; } }
        public override BinaryReader GetStorageReader(int saveItem, ref int totalSize) {
            Close();
            var p = _paths[saveItem];
            _fs = new FileStream(p, FileMode.Open, FileAccess.Read);
            totalSize = (int)_fs.Length;
            _r = new BinaryReader(_fs);
            return _r;
        }
        public override BinaryWriter GetStorageWriter(int saveItem) {
            Close();
            var p = _paths[saveItem];
            _fs = new FileStream(p, FileMode.Create, FileAccess.Write);
            _w = new BinaryWriter(_fs);
            return _w;
        }
        private void Close() { _r?.Dispose(); _w?.Dispose(); _fs?.Dispose(); _r = null; _w = null; _fs = null; }
        public override void Dispose() { Close(); }
    }

    internal sealed class FileStorage : Storage {
        private string _root;
        private string _mod = "";
        public FileStorage() {
            _root = Environment.GetEnvironmentVariable("CARRION_SAVE");
            if (string.IsNullOrEmpty(_root)) _root = Path.Combine(AppContext.BaseDirectory, "save");
            Directory.CreateDirectory(_root);
        }
        private string Dir(StorageCategory c) {
            var d = Path.Combine(_root, c.ToString(), _mod ?? "");
            Directory.CreateDirectory(d);
            return d;
        }
        private string PathFor(StorageCategory c, string id) => Path.Combine(Dir(c), id + ".dat");
        public override bool StorageItemExists(StorageCategory category, string storageItemId) => File.Exists(PathFor(category, storageItemId));
        public override StorageContext CreateReadContext(StorageCategory category, string[] storageItemIds) {
            var ps = new string[storageItemIds.Length];
            for (int i = 0; i < ps.Length; i++) ps[i] = PathFor(category, storageItemIds[i]);
            return new FileStorageContext(ps, false);
        }
        public override StorageContext CreateWriteContext(StorageCategory category, string[] storageItemIds) {
            var ps = new string[storageItemIds.Length];
            for (int i = 0; i < ps.Length; i++) ps[i] = PathFor(category, storageItemIds[i]);
            return new FileStorageContext(ps, true);
        }
        public override void SetModMode(string mod) { _mod = mod ?? ""; }
    }
}

namespace PlatformAPI.Android {
    public class PlatformAndroid : Platform {
        private readonly Achievements _ach = new StubAchievements();
        private readonly Storage _storage = new FileStorage();
        private readonly UGC _ugc = new StubUGC();
        private readonly User _user = new User();

        public PlatformAndroid(global::Android.App.Activity activity) {
            _user.SetUser("Player", "Player", true);
        }
        public bool UserAuthenticated { get; private set; } = true;
        public override string Name => "Desktop";
        public override bool IsStorageLoading => false;
        public override bool IsChangingUserProfile => false;
        public override Achievements Achievements => _ach;
        public override User CurrentUser => _user;
        public override Storage Storage => _storage;
        public override UGC UGC => _ugc;
        public override bool CanRestart => false;
        public static string GetLocalDataDirectory() {
            var d = Environment.GetEnvironmentVariable("CARRION_SAVE");
            if (string.IsNullOrEmpty(d)) d = Path.Combine(AppContext.BaseDirectory, "save");
            Directory.CreateDirectory(d);
            return d;
        }
        public override void ChangeUser(int playerIndex) { }
        public override PlatformLanguage DetectLanguage() => PlatformLanguage.English; // never Japanese
        public override bool IsGamepadPaired(int playerIndex) => true;
        public override void Restart() { }
        public override void Shutdown() { }
        public override void Update() { }
    }
}
