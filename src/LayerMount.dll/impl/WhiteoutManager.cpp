#include "WhiteoutManager.h"
#include "MetadataADS.h"
#include "Cache.h"
#include "../abi/EventEmitter.h"

namespace LayerMount {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

WhiteoutManager::WhiteoutManager(const LayerConfig& config, Cache* cache)
    : config_(config)
    , cache_(cache) {
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

bool WhiteoutManager::IsWhiteoutName(const std::wstring& fileName) {
    const size_t prefixLen = wcslen(kWhiteoutPrefix);
    return fileName.size() >= prefixLen &&
           fileName.compare(0, prefixLen, kWhiteoutPrefix) == 0;
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
    std::wstring whRelative = GetWhiteoutFileName(relativePath);
    return layerPath + L"\\" + whRelative;
}

// ---------------------------------------------------------------------------
// Whiteout detection
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Whiteout creation/removal (upper layer only)
// ---------------------------------------------------------------------------

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
    // CopyUp::EnsureCopyUpPrivileges) so a parent directory that inherited
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

// ---------------------------------------------------------------------------
// Opaque directory support
// ---------------------------------------------------------------------------

bool WhiteoutManager::IsOpaque(const std::wstring& dirRelativePath) const {
    return IsOpaqueInLayer(dirRelativePath, config_.upperPath);
}

bool WhiteoutManager::IsOpaqueInLayer(const std::wstring& dirRelativePath,
                                       const std::wstring& layerPath) const {
    std::wstring dirFullPath = layerPath + L"\\" + dirRelativePath;

    // Check ADS marker (fast)
    if (MetadataADS::HasOpaqueADS(dirFullPath, &config_)) {
        return true;
    }

    // Check sentinel file
    std::wstring opqPath = dirFullPath + L"\\" + kOpaqueMarkerFile;
    return GetFileAttributesW(opqPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WhiteoutManager::SetOpaque(const std::wstring& dirRelativePath) {
    std::wstring dirFullPath = config_.upperPath + L"\\" + dirRelativePath;

    // Ensure the directory itself exists
    EnsureDirectoryExists(dirFullPath);

    // Write both markers for compatibility
    bool adsOk = MetadataADS::SetOpaqueADS(dirFullPath, &config_);

    // Create sentinel file. Use FILE_FLAG_BACKUP_SEMANTICS for the same
    // reason as CreateWhiteout above: the target directory may have an
    // inherited DENY-WRITE ACE we need to bypass via SE_RESTORE_NAME.
    std::wstring opqPath = dirFullPath + L"\\" + kOpaqueMarkerFile;
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

    // Invalidate cache — opaque hides entire subtree
    if (cache_) {
        cache_->Invalidate(NormalizePath(dirRelativePath));
    }

    return adsOk || fileOk;
}

bool WhiteoutManager::RemoveOpaque(const std::wstring& dirRelativePath) {
    std::wstring dirFullPath = config_.upperPath + L"\\" + dirRelativePath;

    // RemoveOpaqueADS removes the marker from both the ADS and the sidecar
    // backend; on non-ADS hosts the sidecar is the authoritative marker. If
    // either fails for any reason other than the marker being already absent,
    // the directory is still logically opaque and we must not report success.
    const bool adsOk = MetadataADS::RemoveOpaqueADS(dirFullPath, &config_);

    std::wstring opqPath = dirFullPath + L"\\" + kOpaqueMarkerFile;
    bool legacyOk = true;
    if (!DeleteFileW(opqPath.c_str())) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            legacyOk = false;
        }
    }

    // Invalidate cache regardless — partial removals still change directory
    // state, and future lookups must not observe the stale opaque cache entry.
    if (cache_) {
        cache_->Invalidate(NormalizePath(dirRelativePath));
    }

    return adsOk && legacyOk;
}

// ---------------------------------------------------------------------------
// Opaque inheritance
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Directory enumeration support
// ---------------------------------------------------------------------------

std::vector<std::wstring> WhiteoutManager::ListWhiteoutsInDirectory(
    const std::wstring& dirRelativePath,
    const std::wstring& layerPath,
    bool* ok) const {

    std::vector<std::wstring> result;
    std::wstring searchPath = layerPath + L"\\" + dirRelativePath + L"\\*";

    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        // ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND: no entries, success.
        // Anything else is a real enumeration failure.
        if (ok != nullptr) {
            *ok = (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND);
        }
        return result;
    }

    const size_t prefixLen = wcslen(kWhiteoutPrefix);

    do {
        std::wstring name(findData.cFileName);

        // Skip . and ..
        if (name == L"." || name == L"..") continue;

        // Check for .wh. prefix
        if (name.size() >= prefixLen &&
            name.compare(0, prefixLen, kWhiteoutPrefix) == 0) {
            // Skip the opaque marker — it's not a file whiteout
            if (name == kOpaqueMarkerFile) continue;

            // Strip .wh. prefix to get the original filename
            result.push_back(name.substr(prefixLen));
        }
    } while (FindNextFileW(hFind, &findData));

    // FindNextFileW returning false terminates enumeration either normally
    // (ERROR_NO_MORE_FILES) or with a real I/O / sharing failure. Treat any
    // non-normal terminal as a failed enumeration so directory merge can
    // refuse to expose a potentially-incomplete whiteout set.
    const DWORD terminalErr = ::GetLastError();
    FindClose(hFind);

    if (ok != nullptr) {
        *ok = (terminalErr == ERROR_NO_MORE_FILES);
    }
    return result;
}

} // namespace LayerMount
