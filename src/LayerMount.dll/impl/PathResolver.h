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

    // Check if a path exists in the upper layer specifically
    bool ExistsInUpper(const std::wstring& relativePath) const;

    // Get the absolute path where a file would be written in the upper layer
    std::wstring GetUpperPath(const std::wstring& relativePath) const;

    // Detect type conflicts across layers (file vs. directory).
    // Returns true if a conflict was detected. Logs via OutputDebugStringW.
    bool HasTypeConflict(const std::wstring& relativePath) const;

    const LayerConfig& Config() const { return config_; }

private:
    // Internal resolution with redirect depth tracking
    ResolvedPath ResolvePathInternal(const std::wstring& relativePath,
                                     int redirectDepth) const;

    static constexpr int kMaxRedirectDepth = 40;

    const LayerConfig& config_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
};

} // namespace LayerMount
