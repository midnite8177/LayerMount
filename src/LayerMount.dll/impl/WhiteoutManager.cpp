#include "WhiteoutManager.h"
#include "MetadataADS.h"
#include "Cache.h"
#include "LayerPath.h"
#include "../abi/EventEmitter.h"

#include <string_view>

namespace LayerMount {

namespace fs = std::filesystem;

namespace {

constexpr size_t kWhiteoutPrefixLength = std::wstring_view(kWhiteoutPrefix).size();

}

WhiteoutManager::WhiteoutManager(const LayerConfig& config, Cache* cache)
    : config_(config)
    , cache_(cache) {
}

bool WhiteoutManager::IsWhiteoutName(std::wstring_view fileName) {
    constexpr int prefixLength = static_cast<int>(kWhiteoutPrefixLength);
    return fileName.size() >= kWhiteoutPrefixLength &&
           CompareStringOrdinal(fileName.data(), prefixLength,
                                kWhiteoutPrefix, prefixLength,
                                TRUE) == CSTR_EQUAL;
}

std::wstring WhiteoutManager::GetWhitedOutName(const std::wstring& whiteoutName) {
    return whiteoutName.substr(kWhiteoutPrefixLength);
}

std::optional<std::wstring> WhiteoutManager::WhitedOutNameOfEntry(const std::wstring& entryName) {
    if (!IsWhiteoutName(entryName) || entryName == kOpaqueMarkerFile) return std::nullopt;
    return GetWhitedOutName(entryName);
}

std::wstring WhiteoutManager::GetWhiteoutFileName(const std::wstring& relativePath) {
    fs::path p(relativePath);
    fs::path parent = p.parent_path();
    std::wstring whName = std::wstring(kWhiteoutPrefix) + p.filename().wstring();
    if (parent.empty()) {
        return whName;
    }
    return (parent / whName).wstring();
}

std::wstring WhiteoutManager::GetWhiteoutFullPath(const std::wstring& layerPath,
                                                   const std::wstring& relativePath) {
    return JoinDirPath(layerPath, GetWhiteoutFileName(relativePath));
}

bool WhiteoutManager::HasWhiteout(const std::wstring& relativePath,
                                   const std::wstring& layerPath) const {
    std::wstring whPath = GetWhiteoutFullPath(layerPath, relativePath);
    return GetFileAttributesW(whPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WhiteoutManager::HasWhiteoutInAnyLayer(const std::wstring& relativePath) const {
    // Check upper layer first
    if (HasWhiteout(relativePath, config_.upperPath)) {
        return true;
    }
    // Check lower layers in priority order
    for (const auto& lower : config_.lowerPaths) {
        if (HasWhiteout(relativePath, lower)) {
            return true;
        }
    }
    return false;
}

bool WhiteoutManager::CreateWhiteout(const std::wstring& relativePath,
                                      WhiteoutType type) {
    if (type == WhiteoutType::Opaque) {
        return SetOpaque(relativePath);
    }

    std::wstring whPath = GetWhiteoutFullPath(config_.upperPath, relativePath);

    // Ensure parent directory exists
    fs::path parentDir = fs::path(whPath).parent_path();
    if (!parentDir.empty()) {
        EnsureDirectoryExists(parentDir.wstring());
    }

    // Create the whiteout marker as a hidden+system zero-byte file.
    // FILE_FLAG_BACKUP_SEMANTICS honors SE_RESTORE_NAME (enabled in
    // EnableFileSystemPrivileges) so a parent directory that inherited
    // a DENY-WRITE ACE from the lower layer does not block our ability to
    // drop the whiteout marker we own. Without this, renaming or deleting
    // a lower file under a restrictive parent DACL fails because the
    // engine can't persist the whiteout that would hide the lower entry.
    HANDLE h = CreateFileW(
        whPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
            FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);

    // Invalidate cache for the affected path
    if (cache_) {
        cache_->InvalidateWithAncestors(NormalizePath(relativePath));
    }

    // Notify the host that a whiteout marker was created. The event's
    // hr field is S_OK (informational); consumers
    // distinguish file vs directory whiteouts by the `type` argument
    // they passed, not by the event itself (which carries only the path).
    if (events_ != nullptr) {
        events_->Emit(LM_EVT_WHITEOUT_CREATED, S_OK, relativePath.c_str(), nullptr);
    }
    return true;
}

bool WhiteoutManager::RemoveWhiteout(const std::wstring& relativePath) {
    std::wstring whPath = GetWhiteoutFullPath(config_.upperPath, relativePath);

    if (!DeleteFileW(whPath.c_str())) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return false;
        }
    }

    if (cache_) {
        cache_->InvalidateWithAncestors(NormalizePath(relativePath));
    }

    return true;
}

bool WhiteoutManager::IsOpaque(const std::wstring& dirRelativePath) const {
    return IsOpaqueInLayer(dirRelativePath, config_.upperPath);
}

bool WhiteoutManager::IsOpaqueInLayer(const std::wstring& dirRelativePath,
                                       const std::wstring& layerPath) const {
    std::wstring dirFullPath = JoinDirPath(layerPath, dirRelativePath);

    if (MetadataADS::HasOpaqueADS(dirFullPath, &config_)) {
        return true;
    }

    std::wstring opqPath = JoinDirPath(dirFullPath, kOpaqueMarkerFile);
    return GetFileAttributesW(opqPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WhiteoutManager::SetOpaque(const std::wstring& dirRelativePath) {
    std::wstring dirFullPath = JoinDirPath(config_.upperPath, dirRelativePath);

    EnsureDirectoryExists(dirFullPath);

    bool adsOk = MetadataADS::SetOpaqueADS(dirFullPath, &config_);

    // A layer image packs the marker file but not the ADS marker, so the
    // file keeps the directory opaque in a lower unpacked from this upper.
    // FILE_FLAG_BACKUP_SEMANTICS lets SE_RESTORE_NAME pass an inherited
    // DENY-WRITE ACE on the directory; without it, the create fails there.
    std::wstring opqPath = JoinDirPath(dirFullPath, kOpaqueMarkerFile);
    HANDLE h = CreateFileW(
        opqPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
            FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);

    bool fileOk = (h != INVALID_HANDLE_VALUE);
    if (fileOk) {
        CloseHandle(h);
    }

    if (cache_) {
        cache_->Invalidate(NormalizePath(dirRelativePath));
    }

    return adsOk || fileOk;
}

bool WhiteoutManager::RemoveOpaque(const std::wstring& dirRelativePath) {
    std::wstring dirFullPath = JoinDirPath(config_.upperPath, dirRelativePath);

    const bool adsOk = MetadataADS::RemoveOpaqueADS(dirFullPath, &config_);

    std::wstring opqPath = JoinDirPath(dirFullPath, kOpaqueMarkerFile);
    bool legacyOk = true;
    if (!DeleteFileW(opqPath.c_str())) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            legacyOk = false;
        }
    }

    if (cache_) {
        cache_->Invalidate(NormalizePath(dirRelativePath));
    }

    return adsOk && legacyOk;
}

bool WhiteoutManager::HasOpaqueAncestor(const std::wstring& relativePath) const {
    fs::path p(relativePath);
    fs::path ancestor = p.parent_path();

    while (!ancestor.empty()) {
        if (IsOpaque(ancestor.wstring())) {
            return true;
        }
        fs::path next = ancestor.parent_path();
        if (next == ancestor) break;  // reached root
        ancestor = next;
    }

    return false;
}

bool WhiteoutManager::HasOpaqueAncestorInLayer(const std::wstring& relativePath,
                                                const std::wstring& layerPath) const {
    fs::path p(relativePath);
    fs::path ancestor = p.parent_path();

    while (!ancestor.empty()) {
        if (IsOpaqueInLayer(ancestor.wstring(), layerPath)) {
            return true;
        }
        fs::path next = ancestor.parent_path();
        if (next == ancestor) break;
        ancestor = next;
    }

    return false;
}

bool WhiteoutManager::HasOpaqueSelfOrAncestorInLayer(const std::wstring& dirRelativePath,
                                                      const std::wstring& layerPath) const {
    return IsOpaqueInLayer(dirRelativePath, layerPath) ||
        HasOpaqueAncestorInLayer(dirRelativePath, layerPath);
}

bool WhiteoutManager::HasWhitedOutAncestorInLayer(const std::wstring& relativePath,
                                                   const std::wstring& layerPath) const {
    fs::path p(relativePath);
    fs::path ancestor = p.parent_path();

    while (!ancestor.empty()) {
        if (HasWhiteout(ancestor.wstring(), layerPath)) {
            return true;
        }
        fs::path next = ancestor.parent_path();
        if (next == ancestor) break;
        ancestor = next;
    }

    return false;
}

}
