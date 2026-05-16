// Unit tests for Cache. Test the class in isolation — do NOT mount
// an overlay, do NOT invoke host-adapter callbacks. Cache is pure
// in-memory; no TempLayerEnvironment or NTFS needed.

#include "pch.h"
#include "Cache.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

// ---------------------------------------------------------------------------
// Test-only helper: spin yield() until steady_clock advances. Used by the LRU
// eviction test to guarantee distinct `lastAccess` timestamps between Puts
// (Cache.cpp:162 uses strict `<` — identical ticks would make eviction order
// depend on unordered_map iteration order). Deterministic; avoids sleep_for.
// ---------------------------------------------------------------------------
static void WaitForSteadyClockTick() {
    auto before = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() == before) {
        std::this_thread::yield();
    }
}

static ResolvedPath MakeFakeResolved(const std::wstring& path) {
    ResolvedPath r;
    r.absolutePath = path;
    r.source = LayerSource::Upper;
    return r;
}

TEST_CLASS(CacheTests) {
public:
    // --- Basic put/get ---

    TEST_METHOD(PutGet_SameKey_ReturnsPutValue) {
        Cache cache;
        ResolvedPath original = MakeFakeResolved(L"C:\\upper\\foo.txt");
        cache.Put(L"foo.txt", original);

        auto got = cache.Get(L"foo.txt");
        Assert::IsTrue(got.has_value());
        Assert::AreEqual(original.absolutePath, got->absolutePath);
    }

    TEST_METHOD(Get_KeyNotPresent_ReturnsNullopt_IncrementsMisses) {
        Cache cache;
        uint64_t missesBefore = cache.Misses();
        Assert::IsFalse(cache.Get(L"absent.txt").has_value());
        Assert::AreEqual(missesBefore + 1, cache.Misses());
    }

    TEST_METHOD(Get_KeyPresent_IncrementsHits) {
        Cache cache;
        cache.Put(L"foo.txt", MakeFakeResolved(L"C:\\upper\\foo.txt"));

        uint64_t hitsBefore = cache.Hits();
        cache.Get(L"foo.txt");
        Assert::AreEqual(hitsBefore + 1, cache.Hits());
    }

    TEST_METHOD(Size_ReflectsEntryCount) {
        Cache cache;
        Assert::AreEqual(size_t{0}, cache.Size());
        cache.Put(L"a", MakeFakeResolved(L"a"));
        cache.Put(L"b", MakeFakeResolved(L"b"));
        cache.Put(L"c", MakeFakeResolved(L"c"));
        Assert::AreEqual(size_t{3}, cache.Size());
    }

    TEST_METHOD(Put_OverwriteSameKey_DoesNotGrowSize) {
        Cache cache;
        cache.Put(L"k", MakeFakeResolved(L"first"));
        cache.Put(L"k", MakeFakeResolved(L"second"));
        Assert::AreEqual(size_t{1}, cache.Size());
        Assert::AreEqual(std::wstring(L"second"), cache.Get(L"k")->absolutePath);
    }

    // --- LRU eviction ---

    TEST_METHOD(Put_BeyondCapacity_EvictsLRU) {
        Cache cache(/*maxCapacity=*/2);

        cache.Put(L"a", MakeFakeResolved(L"a"));
        WaitForSteadyClockTick();

        cache.Put(L"b", MakeFakeResolved(L"b"));
        WaitForSteadyClockTick();

        // Touch A — its lastAccess is now the most recent
        cache.Get(L"a");
        WaitForSteadyClockTick();

        // Inserting C exceeds capacity; B is now LRU and should be evicted
        cache.Put(L"c", MakeFakeResolved(L"c"));

        Assert::IsTrue(cache.Get(L"a").has_value(), L"A should remain (recently touched)");
        Assert::IsFalse(cache.Get(L"b").has_value(), L"B should be evicted (LRU)");
        Assert::IsTrue(cache.Get(L"c").has_value(), L"C should be present (just inserted)");
    }

    // --- Invalidation ---

    TEST_METHOD(Invalidate_RemovesExactKey) {
        Cache cache;
        cache.Put(L"foo.txt", MakeFakeResolved(L"foo.txt"));
        cache.Invalidate(L"foo.txt");
        Assert::IsFalse(cache.Get(L"foo.txt").has_value());
    }

    TEST_METHOD(Invalidate_CascadesToDescendants) {
        Cache cache;
        cache.Put(L"a\\b\\c", MakeFakeResolved(L"c"));
        cache.Put(L"a\\b\\d", MakeFakeResolved(L"d"));
        cache.Put(L"a\\b", MakeFakeResolved(L"b"));

        cache.Invalidate(L"a\\b");

        Assert::IsFalse(cache.Get(L"a\\b").has_value(), L"Exact key removed");
        Assert::IsFalse(cache.Get(L"a\\b\\c").has_value(), L"Descendant removed");
        Assert::IsFalse(cache.Get(L"a\\b\\d").has_value(), L"Descendant removed");
    }

    TEST_METHOD(InvalidateWithAncestors_RemovesKeyAndAllAncestors) {
        Cache cache;
        cache.Put(L"a", MakeFakeResolved(L"a"));
        cache.Put(L"a\\b", MakeFakeResolved(L"ab"));
        cache.Put(L"a\\b\\c", MakeFakeResolved(L"abc"));

        cache.InvalidateWithAncestors(L"a\\b\\c");

        Assert::IsFalse(cache.Get(L"a").has_value(), L"Root ancestor should be removed");
        Assert::IsFalse(cache.Get(L"a\\b").has_value(), L"Parent should be removed");
        Assert::IsFalse(cache.Get(L"a\\b\\c").has_value(), L"Key itself should be removed");
    }

    TEST_METHOD(InvalidateWithAncestors_AlsoRemovesDescendants) {
        Cache cache;
        cache.Put(L"a\\b", MakeFakeResolved(L"ab"));
        cache.Put(L"a\\b\\c", MakeFakeResolved(L"abc"));
        cache.Put(L"a\\b\\d", MakeFakeResolved(L"abd"));

        cache.InvalidateWithAncestors(L"a\\b");

        Assert::IsFalse(cache.Get(L"a\\b").has_value());
        Assert::IsFalse(cache.Get(L"a\\b\\c").has_value(),
            L"Descendant should also be removed");
        Assert::IsFalse(cache.Get(L"a\\b\\d").has_value(),
            L"Descendant should also be removed");
    }

    // --- Clear ---

    TEST_METHOD(Clear_RemovesAllEntries_ResetsSize_PreservesHitsMisses) {
        Cache cache;
        cache.Put(L"a", MakeFakeResolved(L"a"));
        cache.Put(L"b", MakeFakeResolved(L"b"));
        cache.Get(L"a");             // 1 hit
        cache.Get(L"absent");         // 1 miss

        uint64_t hitsBefore = cache.Hits();
        uint64_t missesBefore = cache.Misses();

        cache.Clear();

        Assert::AreEqual(size_t{0}, cache.Size());
        Assert::IsFalse(cache.Get(L"a").has_value());
        // Clear intentionally preserves hit/miss counters (Cache.cpp:130-133
        // only clears `entries_`). The new Get call above adds a miss, so test
        // with that offset.
        Assert::AreEqual(hitsBefore, cache.Hits(), L"Hits should be preserved");
        Assert::AreEqual(missesBefore + 1, cache.Misses(),
            L"Previous misses preserved; new miss from post-clear Get adds one");
    }

    // --- Key normalization ---

    TEST_METHOD(Keys_NormalizedForLookup_CaseInsensitive) {
        Cache cache;
        cache.Put(L"Foo.TXT", MakeFakeResolved(L"val"));
        auto got = cache.Get(L"foo.txt");
        Assert::IsTrue(got.has_value(), L"Mixed-case lookup should hit");
    }

    TEST_METHOD(Keys_NormalizedForLookup_LeadingSlashIgnored) {
        Cache cache;
        cache.Put(L"\\a\\b", MakeFakeResolved(L"val"));
        auto got = cache.Get(L"a\\b");
        Assert::IsTrue(got.has_value(), L"Leading backslash should be normalized away");
    }

    // --- Concurrency ---

    TEST_METHOD(Concurrent_MixedReadersAndWriters_NoCrashAndBoundedSize) {
        constexpr size_t kCapacity = 50;
        constexpr size_t kKeySpace = 100;
        constexpr auto kDuration = std::chrono::milliseconds(500);

        Cache cache(kCapacity);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> opsDone{0};

        auto worker = [&](int seed, bool writer) {
            uint64_t local = 0;
            uint64_t x = static_cast<uint64_t>(seed) * 2654435761u;
            while (!stop.load(std::memory_order_relaxed)) {
                x = x * 6364136223846793005ull + 1442695040888963407ull;
                size_t idx = static_cast<size_t>(x % kKeySpace);
                std::wstring key = L"k" + std::to_wstring(idx);
                if (writer) {
                    cache.Put(key, MakeFakeResolved(key));
                } else {
                    cache.Get(key);
                }
                ++local;
            }
            opsDone.fetch_add(local, std::memory_order_relaxed);
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i) threads.emplace_back(worker, i, /*writer=*/true);
        for (int i = 0; i < 4; ++i) threads.emplace_back(worker, 100 + i, /*writer=*/false);

        std::this_thread::sleep_for(kDuration);
        stop.store(true, std::memory_order_relaxed);

        for (auto& t : threads) t.join();

        Assert::IsTrue(cache.Size() <= kCapacity,
            L"Size must stay within maxCapacity under contention");
        Assert::IsTrue(opsDone.load() > 0, L"Threads should complete some ops");
    }

    TEST_METHOD(Concurrent_InvalidateRace_NoCrashConsistent) {
        constexpr size_t kKeySpace = 30;
        constexpr auto kDuration = std::chrono::milliseconds(500);

        Cache cache(/*maxCapacity=*/1000);
        std::atomic<bool> stop{false};

        auto writer = [&](int seed) {
            uint64_t x = static_cast<uint64_t>(seed) * 2654435761u;
            while (!stop.load(std::memory_order_relaxed)) {
                x = x * 6364136223846793005ull + 1442695040888963407ull;
                size_t idx = static_cast<size_t>(x % kKeySpace);
                std::wstring key = L"a\\b\\k" + std::to_wstring(idx);
                cache.Put(key, MakeFakeResolved(key));
            }
        };
        auto invalidator = [&]() {
            uint64_t x = 12345u;
            while (!stop.load(std::memory_order_relaxed)) {
                x = x * 6364136223846793005ull + 1442695040888963407ull;
                size_t idx = static_cast<size_t>(x % kKeySpace);
                std::wstring key = L"a\\b\\k" + std::to_wstring(idx);
                cache.InvalidateWithAncestors(key);
            }
        };
        auto reader = [&](int seed) {
            uint64_t x = static_cast<uint64_t>(seed) * 2654435761u;
            while (!stop.load(std::memory_order_relaxed)) {
                x = x * 6364136223846793005ull + 1442695040888963407ull;
                size_t idx = static_cast<size_t>(x % kKeySpace);
                std::wstring key = L"a\\b\\k" + std::to_wstring(idx);
                cache.Get(key);
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < 3; ++i) threads.emplace_back(writer, i);
        for (int i = 0; i < 2; ++i) threads.emplace_back(invalidator);
        for (int i = 0; i < 3; ++i) threads.emplace_back(reader, 100 + i);

        std::this_thread::sleep_for(kDuration);
        stop.store(true, std::memory_order_relaxed);

        for (auto& t : threads) t.join();

        // Post-condition: cache state is internally consistent. Final size bounded
        // by key space (each key occupies at most one entry).
        Assert::IsTrue(cache.Size() <= kKeySpace,
            L"Size cannot exceed the number of distinct keys used");
    }
};

} // namespace LayerMountTests
