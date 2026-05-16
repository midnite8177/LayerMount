#include "Cache.h"

#include <algorithm>
#include <intrin.h>

namespace LayerMount {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Cache::Cache(size_t maxCapacity)
    : maxCapacity_(maxCapacity) {
}

// ---------------------------------------------------------------------------
// Get
// ---------------------------------------------------------------------------

std::optional<ResolvedPath> Cache::Get(const std::wstring& relativePath) const {
    std::wstring key = NormalizePath(relativePath);

    std::shared_lock lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        ++misses_;
        return std::nullopt;
    }

    ++hits_;
    // Refresh lastAccess atomically so concurrent Gets under the shared
    // lock do not race on the timestamp write. Using the Interlocked
    // intrinsic (C++17-compatible; std::atomic_ref is C++20-only) gives
    // a single atomic 64-bit store with no tear risk. The exact value
    // is approximate for LRU purposes -- correctness here is avoiding
    // the data race, not nanosecond-accurate ordering.
    const auto nowTicks = static_cast<LONG64>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    static_assert(sizeof(std::chrono::steady_clock::rep) == sizeof(LONG64),
                  "steady_clock::rep size must match LONG64 for _InterlockedExchange64");
    _InterlockedExchange64(
        reinterpret_cast<volatile LONG64*>(
            &const_cast<CacheEntry&>(it->second).lastAccessTicks),
        nowTicks);
    return it->second.resolved;
}

// ---------------------------------------------------------------------------
// Put
// ---------------------------------------------------------------------------

void Cache::Put(const std::wstring& relativePath, const ResolvedPath& resolved) {
    std::wstring key = NormalizePath(relativePath);

    const auto nowTicks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    std::unique_lock lock(mutex_);
    entries_[key] = CacheEntry{resolved, nowTicks};

    if (entries_.size() > maxCapacity_) {
        EvictLRU();
    }
}

// ---------------------------------------------------------------------------
// Invalidate — removes exact key + all descendants
// ---------------------------------------------------------------------------

void Cache::Invalidate(const std::wstring& relativePath) {
    std::wstring key = NormalizePath(relativePath);
    std::wstring prefix = key + L"\\";

    std::unique_lock lock(mutex_);
    entries_.erase(key);

    // Remove all descendants (prefix match)
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// InvalidateWithAncestors — single-pass: path + descendants + ancestors
// ---------------------------------------------------------------------------

void Cache::InvalidateWithAncestors(const std::wstring& relativePath) {
    std::wstring key = NormalizePath(relativePath);
    std::wstring descendantPrefix = key + L"\\";

    // Build the set of ancestor keys
    std::vector<std::wstring> ancestors;
    fs::path p(key);
    fs::path ancestor = p.parent_path();
    while (!ancestor.empty()) {
        ancestors.push_back(ancestor.wstring());
        fs::path next = ancestor.parent_path();
        if (next == ancestor) break;
        ancestor = next;
    }

    // Single pass under one exclusive lock
    std::unique_lock lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        const std::wstring& entryKey = it->first;

        bool shouldRemove = false;

        // Exact match
        if (entryKey == key) {
            shouldRemove = true;
        }
        // Descendant (starts with key\)
        else if (entryKey.compare(0, descendantPrefix.size(), descendantPrefix) == 0) {
            shouldRemove = true;
        }
        // Ancestor match
        else {
            for (const auto& anc : ancestors) {
                if (entryKey == anc) {
                    shouldRemove = true;
                    break;
                }
            }
        }

        if (shouldRemove) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void Cache::Clear() {
    std::unique_lock lock(mutex_);
    entries_.clear();
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

size_t Cache::Size() const {
    std::shared_lock lock(mutex_);
    return entries_.size();
}

uint64_t Cache::Hits() const {
    return hits_.load();
}

uint64_t Cache::Misses() const {
    return misses_.load();
}

// ---------------------------------------------------------------------------
// EvictLRU — find and remove the least recently accessed entry
// ---------------------------------------------------------------------------

void Cache::EvictLRU() {
    // Called under exclusive lock
    if (entries_.empty()) return;

    auto oldest = entries_.begin();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second.lastAccessTicks < oldest->second.lastAccessTicks) {
            oldest = it;
        }
    }
    entries_.erase(oldest);
}

} // namespace LayerMount
