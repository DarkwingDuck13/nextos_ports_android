// Minimal desktop stub of Plugin.InAppBilling — reports the full game ("buyfullversion") as OWNED,
// so Carrion unlocks the full version (the APK is the unlocked/full build).
using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
[assembly: System.Reflection.AssemblyVersion("9.0.0.0")]

namespace Plugin.InAppBilling {
    public enum ItemType { InAppPurchase = 0, InAppPurchaseConsumable = 1, Subscription = 2 }
    public enum PurchaseState { Purchased = 0, Canceled = 1, Purchasing = 2, Failed = 3, Restored = 4, Deferred = 5, PaymentPending = 6, Unknown = 7 }

    public class InAppBillingPurchaseException : Exception {
        public InAppBillingPurchaseException(string message) : base(message) { }
    }

    public class InAppBillingProduct {
        public string ProductId { get; set; }
        public string Name { get; set; }
        public string Description { get; set; }
        public string CurrencyCode { get; set; } = "USD";
        public long MicrosPrice { get; set; } = 0;
        public string LocalizedPrice { get; set; } = "";
    }

    public class InAppBillingPurchase {
        public string Id { get; set; }
        public string TransactionIdentifier { get; set; }
        public string ProductId { get; set; }
        public PurchaseState State { get; set; }
        public string PurchaseToken { get; set; }
        public DateTime TransactionDateUtc { get; set; }
        public override bool Equals(object obj) => obj is InAppBillingPurchase o && o.ProductId == ProductId && o.Id == Id;
        public override int GetHashCode() => (ProductId ?? "").GetHashCode();
        public static bool operator ==(InAppBillingPurchase a, InAppBillingPurchase b) {
            if (ReferenceEquals(a, b)) return true;
            if (a is null || b is null) return false;
            return a.ProductId == b.ProductId && a.Id == b.Id;
        }
        public static bool operator !=(InAppBillingPurchase a, InAppBillingPurchase b) => !(a == b);
    }

    // Only the members Carrion actually calls.
    public interface IInAppBilling {
        Task<bool> ConnectAsync(bool enablePendingPurchases = true, CancellationToken cancellationToken = default);
        Task DisconnectAsync(CancellationToken cancellationToken = default);
        Task<IEnumerable<InAppBillingProduct>> GetProductInfoAsync(ItemType itemType, string[] productIds, CancellationToken cancellationToken = default);
        Task<IEnumerable<InAppBillingPurchase>> GetPurchasesAsync(ItemType itemType, CancellationToken cancellationToken = default);
        Task<InAppBillingPurchase> PurchaseAsync(string productId, ItemType itemType, string obfuscatedAccountId = null, string obfuscatedProfileId = null, string subOfferToken = null, CancellationToken cancellationToken = default);
        Task<IEnumerable<System.ValueTuple<string, bool>>> FinalizePurchaseAsync(string[] transactionIdentifier, CancellationToken cancellationToken = default);
    }

    internal sealed class StubBilling : IInAppBilling {
        private const string FullGameSku = "buyfullversion";
        private static InAppBillingPurchase OwnedFullGame() => new InAppBillingPurchase {
            Id = "stub-full", TransactionIdentifier = "stub-full", ProductId = FullGameSku,
            State = PurchaseState.Purchased, PurchaseToken = "stub", TransactionDateUtc = new DateTime(2024, 1, 1, 0, 0, 0, DateTimeKind.Utc)
        };
        public Task<bool> ConnectAsync(bool enablePendingPurchases = true, CancellationToken ct = default) => Task.FromResult(true);
        public Task DisconnectAsync(CancellationToken ct = default) => Task.CompletedTask;
        public Task<IEnumerable<InAppBillingProduct>> GetProductInfoAsync(ItemType itemType, string[] productIds, CancellationToken ct = default) {
            var list = new List<InAppBillingProduct> { new InAppBillingProduct { ProductId = FullGameSku, Name = "Carrion", Description = "Full game", CurrencyCode = "USD", MicrosPrice = 0, LocalizedPrice = "" } };
            return Task.FromResult<IEnumerable<InAppBillingProduct>>(list);
        }
        public Task<IEnumerable<InAppBillingPurchase>> GetPurchasesAsync(ItemType itemType, CancellationToken ct = default) {
            var list = new List<InAppBillingPurchase> { OwnedFullGame() };
            return Task.FromResult<IEnumerable<InAppBillingPurchase>>(list);
        }
        public Task<InAppBillingPurchase> PurchaseAsync(string productId, ItemType itemType, string a = null, string b = null, string c = null, CancellationToken ct = default)
            => Task.FromResult(OwnedFullGame());
        public Task<IEnumerable<System.ValueTuple<string, bool>>> FinalizePurchaseAsync(string[] ids, CancellationToken ct = default) {
            var list = new List<System.ValueTuple<string, bool>>();
            if (ids != null) foreach (var i in ids) list.Add((i, true));
            return Task.FromResult<IEnumerable<System.ValueTuple<string, bool>>>(list);
        }
    }

    public static class CrossInAppBilling {
        private static readonly IInAppBilling _instance = new StubBilling();
        public static bool IsSupported => true;
        public static IInAppBilling Current => _instance;
        public static void Dispose() { }
    }
}
