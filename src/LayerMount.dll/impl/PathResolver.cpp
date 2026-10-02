#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "MetadataStore.h"
#include "Cache.h"
#include "LayerPath.h"

namespace LayerMount {

namespace {

bool IsResolvablePath(const std::wstring& normalized) {
    return IsSafeRelativePath(normalized) && !IsReservedRelativePath(normalized);
}

bool IsDirectory(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ParentDir(const std::wstring& normalized) {
    return std::filesystem::path(normalized).parent_path().wstring();
}

LowerVisibility LowersBelowParentOf(const WhiteoutManager& whiteoutMgr,
                                    const std::wstring& layerPath,
                                    const std::wstring& normalized) {
    const std::wstring parent = ParentDir(normalized);
    return LowersBelow(LayerDirectory{whiteoutMgr, layerPath, parent});
}

}

PathResolver::PathResolver(ConfigRef config,
                           WhiteoutManager& whiteoutMgr,
                           Cache& cache)
    : config_(config.Get())
    , whiteoutMgr_(whiteoutMgr)
    , cache_(cache) {
}

ResolvedPath PathResolver::ResolvePath(const std::wstring& relativePath) const {
    return ResolvePathInternal(relativePath, 0, nullptr);
}

ResolvedPath PathResolver::ResolvePathInternal(const std::wstring& relativePath,
                                                int redirectDepth,
                                                std::optional<ResolvedPath>* lowerWalk) const {
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

    std::wstring upperFullPath = JoinDirPath(config_.upperPath, normalized);
    DWORD upperAttrs = GetFileAttributesW(upperFullPath.c_str());
    if (upperAttrs != INVALID_FILE_ATTRIBUTES) {
        LayerMountMetadata metadata = MetadataStore::ReadLayerMountMetadata(upperFullPath, &config_);
        if (!metadata.redirect.empty()) {
            return ResolvePathInternal(metadata.redirect, redirectDepth + 1, nullptr);
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

    switch (HidingInUpper(normalized)) {
    case UpperHiding::Whiteout: {
        ResolvedPath whiteout;
        whiteout.isWhiteout = true;
        cache_.Put(normalized, whiteout);
        return whiteout;
    }
    case UpperHiding::NonDirectoryOrLink:
        if (lowerWalk != nullptr) {
            *lowerWalk = ResolvedPath{};
        }
        [[fallthrough]];
    case UpperHiding::OpaqueMarker: {
        ResolvedPath notFound;
        cache_.Put(normalized, notFound);
        return notFound;
    }
    case UpperHiding::None:
        break;
    }

    const ResolvedPath lowerResult = FindInLowers(normalized, 0);
    if (lowerWalk != nullptr) {
        *lowerWalk = lowerResult;
    }
    if (!lowerResult.Found()) {
        return {};
    }

    LogTypeConflictInDeeperLowers(normalized, lowerResult);
    cache_.Put(normalized, lowerResult);
    return lowerResult;
}

PathResolver::UpperHiding PathResolver::HidingInUpper(const std::wstring& normalized) const {
    if (whiteoutMgr_.HasWhiteout(normalized, config_.upperPath) ||
        whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, config_.upperPath)) {
        return UpperHiding::Whiteout;
    }
    switch (LowersBelowParentOf(whiteoutMgr_, config_.upperPath, normalized)) {
    case LowerVisibility::HiddenByOpaqueMarker:
        return UpperHiding::OpaqueMarker;
    case LowerVisibility::HiddenByNonDirectoryOrLink:
        return UpperHiding::NonDirectoryOrLink;
    case LowerVisibility::Visible:
        break;
    }
    return UpperHiding::None;
}

void PathResolver::LogTypeConflictInDeeperLowers(const std::wstring& normalized,
                                                 const ResolvedPath& hit) const {
    const bool hitIsDir = IsDirectory(hit.attributes);
    const std::wstring parent = ParentDir(normalized);
    ResolvedPath visible = hit;
    for (;;) {
        const size_t lowerIndex = static_cast<size_t>(visible.lowerIndex);
        if (LowersBelow(LayerDirectory{whiteoutMgr_, config_.lowerPaths[lowerIndex], parent}) !=
            LowerVisibility::Visible) {
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

ResolvedPath PathResolver::ResolveLowerPath(const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);
    if (normalized.empty()) return {};

    if (!IsResolvablePath(normalized)) {
        return {};
    }

    if (whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, config_.upperPath) ||
        HasNonDirectoryOrLinkSelfOrAncestorInLayer(config_.upperPath, ParentDir(normalized))) {
        return {};
    }

    return FindInLowers(normalized, 0);
}

CreateResolution PathResolver::ResolveForCreate(const std::wstring& relativePath) const {
    const std::wstring normalized = NormalizePath(relativePath);
    std::optional<ResolvedPath> lowerWalk;
    CreateResolution resolution;
    resolution.overlayHit = ResolvePathInternal(normalized, 0, &lowerWalk);
    resolution.whiteoutAtPath = whiteoutMgr_.HasWhiteout(normalized, config_.upperPath);
    resolution.lower = lowerWalk.has_value() ? *lowerWalk : ResolveLowerPath(normalized);
    return resolution;
}

ResolvedPath PathResolver::FindInLowers(const std::wstring& normalized,
                                        size_t firstLower) const {
    const std::wstring parent = ParentDir(normalized);
    for (size_t i = firstLower; i < config_.lowerPaths.size(); ++i) {
        const std::wstring& lowerPath = config_.lowerPaths[i];

        if (whiteoutMgr_.HasWhiteout(normalized, lowerPath)) {
            break;
        }
        if (whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, lowerPath)) {
            break;
        }
        if (HasLinkUnderHigherLayerEntry(config_, i, parent)) {
            break;
        }

        std::wstring fullPath = JoinDirPath(lowerPath, normalized);
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

        if (LowersBelowParentOf(whiteoutMgr_, lowerPath, normalized) != LowerVisibility::Visible) {
            break;
        }
    }

    return {};
}

bool PathResolver::ExistsInUpper(const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);
    std::wstring fullPath = JoinDirPath(config_.upperPath, normalized);
    return GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring PathResolver::GetUpperPath(const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);
    if (normalized.empty()) return config_.upperPath;
    return JoinDirPath(config_.upperPath, normalized);
}

std::wstring PathResolver::GetUpperPathForNewEntry(const CallerPath& path) const {
    return BuildUpperPathPreserveCase(config_.upperPath, path.Text());
}

std::wstring PathResolver::GetUpperPathForCopyUp(const std::wstring& relativePath,
                                                 const ResolvedPath& lowerSource) const {
    return WithStoredLeafName(GetUpperPath(relativePath), lowerSource.absolutePath);
}

std::wstring PathResolver::GetStoredUpperPath(const std::wstring& relativePath) const {
    const std::wstring upperPath = BuildUpperPathPreserveCase(config_.upperPath, relativePath);
    return WithStoredLeafName(upperPath, upperPath);
}

bool PathResolver::HasTypeConflict(const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);

    std::vector<DWORD> foundAttrs;

    std::wstring upperPath = JoinDirPath(config_.upperPath, normalized);
    DWORD upperAttrs = GetFileAttributesW(upperPath.c_str());
    if (upperAttrs != INVALID_FILE_ATTRIBUTES) {
        foundAttrs.push_back(upperAttrs);
    }

    for (const std::wstring& lowerRoot : config_.lowerPaths) {
        std::wstring lowerPath = JoinDirPath(lowerRoot, normalized);
        DWORD lowerAttrs = GetFileAttributesW(lowerPath.c_str());
        if (lowerAttrs != INVALID_FILE_ATTRIBUTES) {
            foundAttrs.push_back(lowerAttrs);
        }
    }

    if (foundAttrs.size() < 2) return false;

    bool firstIsDir = IsDirectory(foundAttrs[0]);
    for (size_t i = 1; i < foundAttrs.size(); ++i) {
        bool isDir = IsDirectory(foundAttrs[i]);
        if (isDir != firstIsDir) {
            return true;
        }
    }

    return false;
}

}
