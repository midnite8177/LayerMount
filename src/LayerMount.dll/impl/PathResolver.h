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

    // Core resolution: relative path -> ResolvedPath
    // Follows redirect metadata, checks whiteouts and opaque ancestors.
    ResolvedPath ResolvePath(const std::wstring& relativePath) const;

    // Resolve only in lower layers (skip upper). Used by copy-up.
    ResolvedPath ResolveLowerPath(const std::wstring& relativePath) const;

    // Fills every field of CreateResolution for the path. The lower walk
    // that ResolvePath runs serves as the lower hit, so a path that the
    // merged resolution follows into the lowers is walked once.
    CreateResolution ResolveForCreate(const std::wstring& relativePath) const;

    // Check if a path exists in the upper layer specifically
    bool ExistsInUpper(const std::wstring& relativePath) const;

    // Get the absolute path where a file would be written in the upper layer
    std::wstring GetUpperPath(const std::wstring& relativePath) const;

    // Detect type conflicts across layers (file vs. directory).
    // Returns true if a conflict was detected. Logs via OutputDebugStringW.
    bool HasTypeConflict(const std::wstring& relativePath) const;

    const LayerConfig& Config() const { return config_; }

private:
    // Internal resolution with redirect depth tracking. When lowerWalk is
    // not null and the walk of the lowers for relativePath itself runs, it
    // receives that walk's result, which equals ResolveLowerPath's.
    ResolvedPath ResolvePathInternal(const std::wstring& relativePath,
                                     int redirectDepth,
                                     std::optional<ResolvedPath>* lowerWalk) const;

    // Returns the first lower at or after firstLower, in priority order,
    // that holds the path. Returns an empty result when no visible lower
    // holds it. The path must be normalized and safe. A whiteout for the
    // path or for an ancestor in a lower hides the path in that lower and in
    // every deeper lower. An opaque ancestor in a lower hides the path in
    // the deeper lowers only.
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

} // namespace LayerMount
