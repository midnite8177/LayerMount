#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "MetadataADS.h"
#include "Cache.h"

namespace LayerMount {

namespace {

bool IsResolvablePath(const std::wstring& normalized) {
    return IsSafeRelativePath(normalized) && !IsReservedRelativePath(normalized);
}

bool IsDirectory(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PathResolver::PathResolver(const LayerConfig& config,
                           WhiteoutManager& whiteoutMgr,
                           Cache& cache)
    : config_(config)
    , whiteoutMgr_(whiteoutMgr)
    , cache_(cache) {
}

// ---------------------------------------------------------------------------
// ResolvePath — public entry point
// ---------------------------------------------------------------------------

ResolvedPath PathResolver::ResolvePath(const std::wstring& relativePath) const {
    return ResolvePathInternal(relativePath, 0);
}

ResolvedPath PathResolver::ResolvePathInternal(const std::wstring& relativePath,
                                                int redirectDepth) const {
    if (redirectDepth > kMaxRedirectDepth) {
        return {};
    }

    std::wstring normalized = NormalizePath(relativePath);
    if (normalized.empty()) {
        return ResolvedPath{
            config_.upperPath,
            LayerSource::Upper,
            -1,
            false,
            GetFileAttributesW(config_.upperPath.c_str())
        };
    }

    if (!IsResolvablePath(normalized)) {
        return {};
    }

    auto cached = cache_.Get(normalized);
    if (cached.has_value()) {
        return cached.value();
    }

    std::wstring upperFullPath = config_.upperPath + L"\\" + normalized;
    DWORD upperAttrs = GetFileAttributesW(upperFullPath.c_str());
    if (upperAttrs != INVALID_FILE_ATTRIBUTES) {
        // Check for redirect in ADS metadata
        LayerMountMetadata metadata = MetadataADS::ReadLayerMountMetadata(upperFullPath, &config_);
        if (!metadata.redirect.empty()) {
            // Follow the redirect chain
            return ResolvePathInternal(metadata.redirect, redirectDepth + 1);
        }

        ResolvedPath result{
            upperFullPath,
            LayerSource::Upper,
            -1,
            false,
            upperAttrs
        };
        cache_.Put(normalized, result);
        return result;
    }

    // A whiteout for the path or for an ancestor in the upper hides the path
    // in every lower.
    if (whiteoutMgr_.HasWhiteout(normalized, config_.upperPath) ||
        whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, config_.upperPath)) {
        ResolvedPath whiteout;
        whiteout.isWhiteout = true;
        cache_.Put(normalized, whiteout);
        return whiteout;
    }

    if (whiteoutMgr_.HasOpaqueAncestor(normalized)) {
        // An ancestor directory in the upper layer is opaque —
        // all lower layers are hidden for this subtree
        ResolvedPath notFound;
        cache_.Put(normalized, notFound);
        return notFound;
    }

    const ResolvedPath lowerResult = FindInLowers(normalized, 0);
    if (!lowerResult.Found()) {
        return {};
    }

    LogTypeConflictInDeeperLowers(normalized, lowerResult);
    cache_.Put(normalized, lowerResult);
    return lowerResult;
}

void PathResolver::LogTypeConflictInDeeperLowers(const std::wstring& normalized,
                                                 const ResolvedPath& hit) const {
    const bool hitIsDir = IsDirectory(hit.attributes);
    ResolvedPath visible = hit;
    for (;;) {
        const size_t lowerIndex = static_cast<size_t>(visible.lowerIndex);
        if (whiteoutMgr_.HasOpaqueAncestorInLayer(normalized, config_.lowerPaths[lowerIndex])) {
            return;
        }
        visible = FindInLowers(normalized, lowerIndex + 1);
        if (!visible.Found()) {
            return;
        }
        const bool otherIsDir = IsDirectory(visible.attributes);
        if (hitIsDir != otherIsDir) {
            OutputDebugStringW(
                (L"[LayerMount] Type conflict for path '" + normalized +
                 L"': " + (hitIsDir ? L"directory" : L"file") +
                 L" in layer " + std::to_wstring(hit.lowerIndex) +
                 L" vs " + (otherIsDir ? L"directory" : L"file") +
                 L" in layer " + std::to_wstring(visible.lowerIndex) + L"\n").c_str());
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// ResolveLowerPath — skip upper layer
// ---------------------------------------------------------------------------

ResolvedPath PathResolver::ResolveLowerPath(const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);
    if (normalized.empty()) return {};

    if (!IsResolvablePath(normalized)) {
        return {};
    }

    // An upper-layer whiteout on any ancestor short-circuits lower iteration —
    // the whole subtree is logically deleted from the merged view.
    if (whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, config_.upperPath)) {
        return {};
    }

    return FindInLowers(normalized, 0);
}

ResolvedPath PathResolver::FindInLowers(const std::wstring& normalized,
                                        size_t firstLower) const {
    for (size_t i = firstLower; i < config_.lowerPaths.size(); ++i) {
        const std::wstring& lowerPath = config_.lowerPaths[i];

        if (whiteoutMgr_.HasWhiteout(normalized, lowerPath)) {
            break;
        }
        if (whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, lowerPath)) {
            break;
        }

        std::wstring fullPath = lowerPath + L"\\" + normalized;
        DWORD attrs = GetFileAttributesW(fullPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            return ResolvedPath{
                fullPath,
                LayerSource::Lower,
                static_cast<int>(i),
                false,
                attrs
            };
        }

        // This check must follow the probe. Before the probe, it hides the
        // entries of the lower that holds the opaque marker.
        if (whiteoutMgr_.HasOpaqueAncestorInLayer(normalized, lowerPath)) {
            break;
        }
    }

    return {};
}

// ---------------------------------------------------------------------------
// ExistsInUpper
// ---------------------------------------------------------------------------

bool PathResolver::ExistsInUpper(const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);
    std::wstring fullPath = config_.upperPath + L"\\" + normalized;
    return GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ---------------------------------------------------------------------------
// GetUpperPath
// ---------------------------------------------------------------------------

std::wstring PathResolver::GetUpperPath(const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);
    if (normalized.empty()) return config_.upperPath;
    return config_.upperPath + L"\\" + normalized;
}

// ---------------------------------------------------------------------------
// HasTypeConflict
// ---------------------------------------------------------------------------

bool PathResolver::HasTypeConflict(const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);

    // Collect attributes from all layers
    std::vector<std::pair<int, DWORD>> found;  // layer index (-1=upper), attributes

    std::wstring upperPath = config_.upperPath + L"\\" + normalized;
    DWORD upperAttrs = GetFileAttributesW(upperPath.c_str());
    if (upperAttrs != INVALID_FILE_ATTRIBUTES) {
        found.push_back({-1, upperAttrs});
    }

    for (size_t i = 0; i < config_.lowerPaths.size(); ++i) {
        std::wstring lowerPath = config_.lowerPaths[i] + L"\\" + normalized;
        DWORD lowerAttrs = GetFileAttributesW(lowerPath.c_str());
        if (lowerAttrs != INVALID_FILE_ATTRIBUTES) {
            found.push_back({static_cast<int>(i), lowerAttrs});
        }
    }

    if (found.size() < 2) return false;

    // Check if any pair disagrees on directory vs file
    bool firstIsDir = (found[0].second & FILE_ATTRIBUTE_DIRECTORY) != 0;
    for (size_t i = 1; i < found.size(); ++i) {
        bool isDir = (found[i].second & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir != firstIsDir) {
            return true;
        }
    }

    return false;
}

} // namespace LayerMount
