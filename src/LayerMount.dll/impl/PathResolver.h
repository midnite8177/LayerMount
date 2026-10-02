#pragma once

#include "LayerMount.h"

namespace LayerMount {

class WhiteoutManager;
class Cache;

class PathResolver {
public:
    PathResolver(const LayerConfig& config,
                 WhiteoutManager& whiteoutMgr,
                 Cache& cache);

    ResolvedPath ResolvePath(const std::wstring& relativePath) const;

    // Returns the first visible lower that holds the path. An upper entry at
    // the path itself does not hide it; a whiteout or a non-directory at an
    // ancestor in the upper does.
    ResolvedPath ResolveLowerPath(const std::wstring& relativePath) const;

    // The lower walk that ResolvePath runs serves as the lower hit, so a
    // path that the overlay resolves into the lowers is walked once.
    CreateResolution ResolveForCreate(const std::wstring& relativePath) const;

    bool ExistsInUpper(const std::wstring& relativePath) const;

    std::wstring GetUpperPath(const std::wstring& relativePath) const;

    // Detect type conflicts across layers (file vs. directory).
    // Returns true if a conflict was detected.
    bool HasTypeConflict(const std::wstring& relativePath) const;

    const LayerConfig& Config() const { return config_; }

private:
    // Internal resolution with redirect depth tracking. When lowerWalk is
    // not null, it receives ResolveLowerPath's result for relativePath itself
    // if the call learns it: after the walk of the lowers runs, or when a
    // non-directory ancestor in the upper hides the lowers.
    ResolvedPath ResolvePathInternal(const std::wstring& relativePath,
                                     int redirectDepth,
                                     std::optional<ResolvedPath>* lowerWalk) const;

    enum class UpperHiding {
        None,
        Whiteout,
        OpaqueMarker,
        NonDirectory,
    };

    // What in the upper hides the path from the lowers, after the upper's
    // probe of the path missed with upperProbeError: a whiteout for the path
    // or for an ancestor, an opaque ancestor, or a non-directory ancestor.
    UpperHiding HidingInUpper(const std::wstring& normalized, DWORD upperProbeError) const;

    // Returns the first lower at or after firstLower, in priority order,
    // that holds the path. Returns an empty result when no visible lower
    // holds it. The path must be normalized and safe. A whiteout for the
    // path or for an ancestor in a lower hides the path in that lower and in
    // every deeper lower. An opaque ancestor or a non-directory ancestor in
    // a lower hides the path in the deeper lowers only.
    ResolvedPath FindInLowers(const std::wstring& normalized, size_t firstLower) const;

    // Logs the first visible lower below the hit that holds the path with
    // the other type, file or directory. The hit stays the result.
    void LogTypeConflictInDeeperLowers(const std::wstring& normalized,
                                       const ResolvedPath& hit) const;

    static constexpr int kMaxRedirectDepth = 40;

    const LayerConfig& config_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
};

}
