#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "MetadataADS.h"
#include "Cache.h"

namespace LayerMount {

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

// ---------------------------------------------------------------------------
// ResolvePathInternal — full algorithm with redirect depth tracking
//
// Algorithm:
//   1. Guard against circular redirects
//   2. Normalize path
//   3. Check cache
//   4. Check upper layer (+ redirect resolution via ADS metadata)
//   5. Check upper-layer whiteout
//   6. Check opaque ancestors in upper layer
//   7. Iterate lower layers with per-layer whiteout + opaque checks
//   8. Type conflict detection
//   9. Return not-found
// ---------------------------------------------------------------------------

ResolvedPath PathResolver::ResolvePathInternal(const std::wstring& relativePath,
                                                int redirectDepth) const {
    // 1. Circular redirect guard
    if (redirectDepth > kMaxRedirectDepth) {
        return {};
    }

    // 2. Normalize
    std::wstring normalized = NormalizePath(relativePath);
    if (normalized.empty()) {
        // Root path — resolve to upper layer root
        return ResolvedPath{
            config_.upperPath,
            LayerSource::Upper,
            -1,
            false,
            GetFileAttributesW(config_.upperPath.c_str())
        };
    }

    // 2b. Reject any unsafe relative path (parent-traversal or drive/stream
    // qualified). Windows canonicalizes concatenations like `upper\..\esc`
    // up and out of the layer root, so the resolver must never return a
    // path outside its layer trees. Treat unsafe inputs as not-found.
    if (!IsSafeRelativePath(normalized)) {
        return {};
    }

    // 2c. Hide the reserved `.overlay` sidecar subtree from merged-view
    // resolution. Sidecar records live under `<upper>\.overlay\` on non-ADS
    // hosts; leaving them visible here would let callers open, read, or
    // operate on internal metadata through the mount.
    if (IsReservedRelativePath(normalized)) {
        return {};
    }

    // 3. Check cache
    auto cached = cache_.Get(normalized);
    if (cached.has_value()) {
        return cached.value();
    }

    // 4. Check upper layer
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

    // 5. Check upper-layer whiteout
    if (whiteoutMgr_.HasWhiteout(normalized, config_.upperPath)) {
        ResolvedPath whiteout;
        whiteout.isWhiteout = true;
        cache_.Put(normalized, whiteout);
        return whiteout;
    }

    // 5b. A whiteout marker at any ancestor in upper hides every descendant
    // from the lower layers too — the deleted directory is gone for everything
    // below it.
    if (whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, config_.upperPath)) {
        ResolvedPath whiteout;
        whiteout.isWhiteout = true;
        cache_.Put(normalized, whiteout);
        return whiteout;
    }

    // 6. Check opaque ancestors in upper layer
    if (whiteoutMgr_.HasOpaqueAncestor(normalized)) {
        // An ancestor directory in the upper layer is opaque —
        // all lower layers are hidden for this subtree
        ResolvedPath notFound;
        cache_.Put(normalized, notFound);
        return notFound;
    }

    // 7. Iterate lower layers in priority order
    ResolvedPath lowerResult;

    for (size_t i = 0; i < config_.lowerPaths.size(); ++i) {
        const std::wstring& lowerPath = config_.lowerPaths[i];

        // 7a. Per-lower-layer whiteout: whiteout in layer N hides layers N+1...
        if (whiteoutMgr_.HasWhiteout(normalized, lowerPath)) {
            break;
        }

        // 7a'. Same whiteout-by-ancestor rule applied to this layer: if a
        // deleted-directory marker sits at any ancestor in this layer, neither
        // this layer nor any deeper layer can surface content below it.
        if (whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, lowerPath)) {
            break;
        }

        // 7b. Per-lower-layer opaque ancestor
        if (whiteoutMgr_.HasOpaqueAncestorInLayer(normalized, lowerPath)) {
            break;
        }

        // 7c. Check if file exists in this lower layer
        std::wstring lowerFullPath = lowerPath + L"\\" + normalized;
        DWORD lowerAttrs = GetFileAttributesW(lowerFullPath.c_str());
        if (lowerAttrs != INVALID_FILE_ATTRIBUTES) {
            lowerResult = ResolvedPath{
                lowerFullPath,
                LayerSource::Lower,
                static_cast<int>(i),
                false,
                lowerAttrs
            };
            break;
        }
    }

    // 8. Type conflict detection
    // If we found a result in a lower layer, check for type conflicts
    if (lowerResult.Found()) {
        // Upper layer was already checked (not found), so no conflict with upper.
        // Check remaining lower layers for type conflicts.
        bool resultIsDir = (lowerResult.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        for (size_t j = static_cast<size_t>(lowerResult.lowerIndex) + 1;
             j < config_.lowerPaths.size(); ++j) {
            std::wstring otherPath = config_.lowerPaths[j] + L"\\" + normalized;
            DWORD otherAttrs = GetFileAttributesW(otherPath.c_str());
            if (otherAttrs != INVALID_FILE_ATTRIBUTES) {
                bool otherIsDir = (otherAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (resultIsDir != otherIsDir) {
                    OutputDebugStringW(
                        (L"[LayerMount] Type conflict for path '" + normalized +
                         L"': " + (resultIsDir ? L"directory" : L"file") +
                         L" in layer " + std::to_wstring(lowerResult.lowerIndex) +
                         L" vs " + (otherIsDir ? L"directory" : L"file") +
                         L" in layer " + std::to_wstring(j) + L"\n").c_str());
                    break;  // Only log the first conflict
                }
            }
        }

        cache_.Put(normalized, lowerResult);
        return lowerResult;
    }

    // 9. Not found in any layer
    return {};
}

// ---------------------------------------------------------------------------
// ResolveLowerPath — skip upper layer
// ---------------------------------------------------------------------------

ResolvedPath PathResolver::ResolveLowerPath(const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);
    if (normalized.empty()) return {};

    // Same traversal-rejection as ResolvePathInternal.
    if (!IsSafeRelativePath(normalized)) {
        return {};
    }

    // Same reserved-subtree rejection as ResolvePathInternal.
    if (IsReservedRelativePath(normalized)) {
        return {};
    }

    // An upper-layer whiteout on any ancestor short-circuits lower iteration —
    // the whole subtree is logically deleted from the merged view.
    if (whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, config_.upperPath)) {
        return {};
    }

    for (size_t i = 0; i < config_.lowerPaths.size(); ++i) {
        const std::wstring& lowerPath = config_.lowerPaths[i];

        // Per-lower-layer whiteout
        if (whiteoutMgr_.HasWhiteout(normalized, lowerPath)) {
            break;
        }

        // Per-lower-layer whiteout at any ancestor
        if (whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, lowerPath)) {
            break;
        }

        // Per-lower-layer opaque ancestor
        if (whiteoutMgr_.HasOpaqueAncestorInLayer(normalized, lowerPath)) {
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
