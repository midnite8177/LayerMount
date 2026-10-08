#include "PathResolver.h"
#include "DirectoryMerge.h"
#include "WhiteoutManager.h"
#include "MetadataStore.h"
#include "Cache.h"
#include "LayerPath.h"

#include <algorithm>
#include <vector>

namespace LayerMount {

namespace {

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

std::vector<std::wstring> SplitComponents(const std::wstring& path) {
    std::vector<std::wstring> components;
    size_t start = 0;
    while (start < path.size()) {
        size_t end = path.find(L'\\', start);
        if (end == std::wstring::npos) {
            end = path.size();
        }
        if (end > start) {
            components.push_back(path.substr(start, end - start));
        }
        start = end + 1;
    }
    return components;
}

std::wstring JoinComponents(const std::vector<std::wstring>& components, size_t count) {
    std::wstring joined;
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            joined.push_back(L'\\');
        }
        joined.append(components[i]);
    }
    return joined;
}

std::vector<std::wstring> ResolveRelativeTarget(std::vector<std::wstring> parent,
                                                std::wstring target) {
    std::replace(target.begin(), target.end(), L'/', L'\\');
    if (!target.empty() && target.front() == L'\\') {
        parent.clear();
    }
    for (std::wstring& component : SplitComponents(target)) {
        if (component == L"..") {
            if (!parent.empty()) {
                parent.pop_back();
            }
        } else if (component != L".") {
            parent.push_back(std::move(component));
        }
    }
    return parent;
}

// Puts the view path of target, the target of the relative link at
// components[linkIndex], in place of the components up to and including
// the link.
void SpliceLinkTarget(std::vector<std::wstring>* components,
                      size_t linkIndex,
                      const std::wstring& target) {
    std::vector<std::wstring> spliced = ResolveRelativeTarget(
        std::vector<std::wstring>(components->begin(), components->begin() + linkIndex),
        target);
    spliced.insert(spliced.end(), components->begin() + linkIndex + 1, components->end());
    *components = std::move(spliced);
}

}

PathResolver::PathResolver(ConfigRef config,
                           WhiteoutManager& whiteoutMgr,
                           Cache& cache)
    : config_(config.Get())
    , whiteoutMgr_(whiteoutMgr)
    , cache_(cache) {
}

bool PathResolver::IsReservedPath(const std::wstring& normalized) const {
    return IsReservedOverlayPath(config_, whiteoutMgr_, normalized);
}

bool PathResolver::IsResolvablePath(const std::wstring& normalized) const {
    return IsSafeRelativePath(normalized) && !IsReservedPath(normalized);
}

std::optional<std::wstring> PathResolver::ResolvableNormalized(
    const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);
    if (normalized.empty() || !IsResolvablePath(normalized)) {
        return std::nullopt;
    }
    return normalized;
}

ResolvedPath PathResolver::ResolvePath(const std::wstring& relativePath) const {
    return ResolvePathInternal(relativePath, 0, nullptr);
}

ViewLookup PathResolver::ViewPathThroughLinks(const std::wstring& path,
                                              FinalLink finalLink) const {
    const std::wstring callerPath = NormalizePathPreserveCase(path);
    const size_t colon = callerPath.find(L':');
    const std::wstring streamSuffix =
        colon == std::wstring::npos ? std::wstring() : callerPath.substr(colon);
    std::vector<std::wstring> components = SplitComponents(callerPath.substr(0, colon));
    if (std::find(components.begin(), components.end(), L"..") != components.end()) {
        return ViewLookup{STATUS_SUCCESS, ViewPath(callerPath, NormalizePath(callerPath))};
    }
    components.erase(std::remove(components.begin(), components.end(), L"."), components.end());

    int hops = 0;
    size_t walked = 0;
    while (walked < components.size()) {
        if (walked + 1 == components.size() && finalLink == FinalLink::Keep) {
            break;
        }
        const ResolvedPath shown = ResolvePath(JoinComponents(components, walked + 1));
        if (!shown.Found()) {
            break;
        }
        if ((shown.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            std::wstring target;
            const ReparseLink link = ReadReparseLink(shown.absolutePath, &target);
            if (link == ReparseLink::OtherLink) {
                break;
            }
            if (link == ReparseLink::RelativeSymlink) {
                if (++hops > kMaxLinkHops) {
                    return ViewLookup{STATUS_REPARSE_POINT_NOT_RESOLVED, ViewPath({}, {})};
                }
                SpliceLinkTarget(&components, walked, target);
                walked = 0;
                continue;
            }
        }
        if (!IsDirectory(shown.attributes)) {
            break;
        }
        ++walked;
    }
    std::wstring viewPath = JoinComponents(components, components.size()) + streamSuffix;
    std::wstring normalized = NormalizePath(viewPath);
    return ViewLookup{STATUS_SUCCESS, ViewPath(std::move(viewPath), std::move(normalized))};
}

ResolvedPath PathResolver::ResolvePathInternal(const std::wstring& relativePath,
                                                int redirectDepth,
                                                std::optional<ResolvedPath>* lowerWalk) const {
    if (redirectDepth > kMaxRedirectDepth) {
        return {};
    }

    std::wstring normalized = NormalizePath(relativePath);
    if (normalized.empty()) {
        return ResolveRoot();
    }

    if (!IsResolvablePath(normalized)) {
        return {};
    }

    auto cached = cache_.Get(normalized);
    if (cached.has_value()) {
        return cached.value();
    }

    std::optional<ResolvedPath> inUpper = ResolveInUpper(normalized, redirectDepth);
    if (inUpper.has_value()) {
        return std::move(*inUpper);
    }

    switch (HidingInUpper(normalized)) {
    case UpperHiding::Whiteout: {
        ResolvedPath whiteout;
        whiteout.isWhiteout = true;
        cache_.Put(normalized, whiteout);
        return whiteout;
    }
    case UpperHiding::NonDirectoryOrLink:
    case UpperHiding::OpaqueMarker: {
        if (lowerWalk != nullptr) {
            *lowerWalk = ResolvedPath{};
        }
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

ResolvedPath PathResolver::ResolveRoot() const {
    return ResolvedPath{
        config_.upperPath.Text(),
        LayerSource::Upper,
        -1,
        false,
        GetFileAttributesW(config_.upperPath.ForWin32().c_str())
    };
}

std::optional<ResolvedPath> PathResolver::ResolveInUpper(const std::wstring& normalized,
                                                         int redirectDepth) const {
    // upperFullPath keeps the Text() form, because the sidecar store finds a
    // record only under the path text that wrote it. Only the Win32 call gets
    // the extended form.
    std::wstring upperFullPath = JoinDirPath(config_.upperPath.Text(), normalized);
    DWORD upperAttrs = GetFileAttributesW(WithExtendedPrefix(upperFullPath).c_str());
    if (upperAttrs == INVALID_FILE_ATTRIBUTES) {
        return std::nullopt;
    }
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

PathResolver::UpperHiding PathResolver::HidingInUpper(const std::wstring& normalized) const {
    if (whiteoutMgr_.HasWhiteout(normalized, config_.upperPath.Text())) {
        return UpperHiding::Whiteout;
    }
    return UpperAncestorHiding(normalized);
}

PathResolver::UpperHiding PathResolver::UpperAncestorHiding(const std::wstring& normalized) const {
    if (whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, config_.upperPath.Text())) {
        return UpperHiding::Whiteout;
    }
    switch (LowersBelowParentOf(whiteoutMgr_, config_.upperPath.Text(), normalized)) {
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
        const LayerDirectory lower{whiteoutMgr_, config_.lowerPaths[lowerIndex].Text(), parent};
        if (LowersBelow(lower) != LowerVisibility::Visible) {
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
    const std::optional<std::wstring> normalized = ResolvableNormalized(relativePath);
    if (!normalized.has_value() || UpperAncestorHiding(*normalized) != UpperHiding::None) {
        return {};
    }
    return FindInLowers(*normalized, 0);
}

CreateResolution PathResolver::ResolveForCreate(const std::wstring& relativePath) const {
    const std::wstring normalized = NormalizePath(relativePath);
    std::optional<ResolvedPath> lowerWalk;
    CreateResolution resolution;
    resolution.overlayHit = ResolvePathInternal(normalized, 0, &lowerWalk);
    resolution.whiteoutAtPath = whiteoutMgr_.HasWhiteout(normalized, config_.upperPath.Text());
    resolution.lower = lowerWalk.has_value() ? *lowerWalk : ResolveLowerPath(normalized);
    return resolution;
}

ResolvedPath PathResolver::FindInLowers(const std::wstring& normalized,
                                        size_t firstLower) const {
    const std::wstring parent = ParentDir(normalized);
    for (size_t i = firstLower; i < config_.lowerPaths.size(); ++i) {
        const HostPath& lowerPath = config_.lowerPaths[i];

        if (whiteoutMgr_.HasWhiteout(normalized, lowerPath.Text())) {
            break;
        }
        if (whiteoutMgr_.HasWhitedOutAncestorInLayer(normalized, lowerPath.Text())) {
            break;
        }
        if (HasLinkUnderHigherLayerEntry(config_, i, parent)) {
            break;
        }

        std::wstring fullPath = JoinDirPath(lowerPath.Text(), normalized);
        DWORD attrs = GetFileAttributesW(WithExtendedPrefix(fullPath).c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            return ResolvedPath{
                fullPath,
                LayerSource::Lower,
                static_cast<int>(i),
                false,
                attrs
            };
        }

        if (LowersBelowParentOf(whiteoutMgr_, lowerPath.Text(), normalized) !=
            LowerVisibility::Visible) {
            break;
        }
    }

    return {};
}

bool PathResolver::ExistsInUpper(const std::wstring& relativePath) const {
    return UpperAttributes(relativePath) != INVALID_FILE_ATTRIBUTES;
}

DWORD PathResolver::UpperAttributes(const std::wstring& relativePath) const {
    return GetFileAttributesW(WithExtendedPrefix(GetUpperPath(relativePath)).c_str());
}

std::wstring PathResolver::GetUpperPath(const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);
    if (normalized.empty()) return config_.upperPath.Text();
    return JoinDirPath(config_.upperPath.Text(), normalized);
}

std::wstring PathResolver::GetUpperPathForNewEntry(const CallerPath& path) const {
    return BuildUpperPathPreserveCase(config_.upperPath.Text(), path.Text());
}

std::wstring PathResolver::GetUpperPathForCopyUp(const std::wstring& relativePath,
                                                 const ResolvedPath& lowerSource) const {
    return WithStoredLeafName(GetUpperPath(relativePath), lowerSource.absolutePath);
}

std::wstring PathResolver::GetStoredUpperPath(const std::wstring& relativePath) const {
    const std::wstring upperPath =
        BuildUpperPathPreserveCase(config_.upperPath.Text(), relativePath);
    return WithStoredLeafName(upperPath, upperPath);
}

std::wstring PathResolver::GetUpperPathForRenameSource(const std::wstring& relativePath) const {
    const std::wstring normalized = NormalizePath(relativePath);
    return ExistsInUpper(normalized)
        ? GetStoredUpperPath(relativePath)
        : GetUpperPathForCopyUp(normalized, ResolveLowerPath(normalized));
}

bool PathResolver::HasTypeConflict(const std::wstring& relativePath) const {
    std::wstring normalized = NormalizePath(relativePath);

    std::vector<DWORD> foundAttrs;

    std::wstring upperPath = JoinDirPath(config_.upperPath.ForWin32(), normalized);
    DWORD upperAttrs = GetFileAttributesW(upperPath.c_str());
    if (upperAttrs != INVALID_FILE_ATTRIBUTES) {
        foundAttrs.push_back(upperAttrs);
    }

    for (const HostPath& lowerRoot : config_.lowerPaths) {
        std::wstring lowerPath = JoinDirPath(lowerRoot.ForWin32(), normalized);
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
