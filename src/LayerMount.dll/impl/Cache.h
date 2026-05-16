#pragma once

#include "LayerMount.h"

#include <shared_mutex>
#include <chrono>
#include <unordered_map>
#include <atomic>

namespace LayerMount {

class Cache {
public:
    explicit Cache(size_t maxCapacity = 10000);

    // Thread-safe read (shared lock). Returns nullopt on miss.
    std::optional<ResolvedPath> Get(const std::wstring& relativePath) const;

    // Thread-safe write (exclusive lock). Triggers LRU eviction if over capacity.
    void Put(const std::wstring& relativePath, const ResolvedPath& resolved);

    // Invalidate a single path AND all descendants (exclusive lock).
    void Invalidate(const std::wstring& relativePath);

    // Invalidate path + all descendants + all ancestors up to root (exclusive lock).
    // Single-pass over the cache — O(n) with one lock acquisition.
    void InvalidateWithAncestors(const std::wstring& relativePath);

    // Clear entire cache (exclusive lock).
    void Clear();

    // Statistics
    size_t Size() const;
    uint64_t Hits() const;
    uint64_t Misses() const;

private:
    struct CacheEntry {
        ResolvedPath resolved;
        // Stored as raw rep (ticks from steady_clock) so Get can update
        // lastAccess under the shared lock via std::atomic_ref without
        // racing with concurrent readers. steady_clock::time_point's rep
        // is an integral type; use that directly.
        std::chrono::steady_clock::rep lastAccessTicks;
    };

    void EvictLRU();  // called under exclusive lock when size > maxCapacity

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::wstring, CacheEntry> entries_;
    size_t maxCapacity_;

    mutable std::atomic<uint64_t> hits_{0};
    mutable std::atomic<uint64_t> misses_{0};
};

} // namespace LayerMount
