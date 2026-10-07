#include "WhiteoutManager.h"
#include "MetadataStore.h"
#include "Cache.h"
#include "EntryCopy.h"
#include "LayerPath.h"
#include "NtStatusUtil.h"
#include "../abi/EventEmitter.h"

#include <string_view>

namespace LayerMount {

namespace fs = std::filesystem;

namespace {

constexpr size_t kWhiteoutPrefixLength = std::wstring_view(kWhiteoutPrefix).size();

enum class LayerRoot {
    Probed,
    Skipped,
};

// Calls hasMarker on dir and then on each ancestor of dir, and returns true
// at the first call that does. dir is relative to the layer root, so the
// walk ends at the empty path, which is the root itself. hasMarker sees the
// root only when root is LayerRoot::Probed.
template <typename HasMarker>
bool AnyDirectoryUpToRoot(fs::path dir, LayerRoot root, const HasMarker& hasMarker) {
    for (; !dir.empty(); dir = dir.parent_path()) {
        if (hasMarker(dir.wstring())) {
            return true;
        }
    }
    return root == LayerRoot::Probed && hasMarker(std::wstring());
}

// Returns ERROR_SUCCESS, or the Win32 error of the failed create.
// FILE_FLAG_BACKUP_SEMANTICS lets SE_RESTORE_NAME pass an inherited
// DENY-WRITE ACE on the parent directory; without it, the create fails there.
DWORD CreateHiddenMarkerFile(const std::wstring& path) {
    HANDLE h = CreateFileW(
        WithExtendedPrefix(path).c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
            FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    CloseHandle(h);
    return ERROR_SUCCESS;
}

bool DeleteMarkerFile(const std::wstring& path) {
    return DeleteFileW(WithExtendedPrefix(path).c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool MarkerFileExists(const std::wstring& path) {
    return GetFileAttributesW(WithExtendedPrefix(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring MarkerDirectoryOf(const std::wstring& relativePath) {
    return NormalizePathPreserveCase(fs::path(relativePath).parent_path().wstring());
}

NTSTATUS MarkerDirectoryStatus(const LinkInView& link) {
    switch (link.stop) {
    case LinkStop::Link:
        return STATUS_NOT_A_DIRECTORY;
    case LinkStop::Unreadable:
        return STATUS_ACCESS_DENIED;
    case LinkStop::None:
        break;
    }
    return STATUS_SUCCESS;
}

}

WhiteoutManager::WhiteoutManager(ConfigRef config, Cache* cache)
    : config_(config.Get())
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
    const bool isOpaqueMarker =
        CompareStringOrdinal(entryName.c_str(), -1, kOpaqueMarkerFile, -1, TRUE) == CSTR_EQUAL;
    if (!IsWhiteoutName(entryName) || isOpaqueMarker) return std::nullopt;
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
    return HasWhiteoutUnderWalkedDirectoryInLayer(relativePath, layerPath) &&
           FindLinkOnPath(layerPath, MarkerDirectoryOf(relativePath)) == LinkOnPath::None;
}

bool WhiteoutManager::HasWhiteoutUnderWalkedDirectoryInLayer(const std::wstring& relativePath,
                                                             const std::wstring& layerPath) const {
    return MarkerFileExists(GetWhiteoutFullPath(layerPath, relativePath));
}

bool WhiteoutManager::HasWhiteoutInAnyLayer(const std::wstring& relativePath) const {
    if (HasWhiteout(relativePath, config_.upperPath)) {
        return true;
    }
    for (const auto& lower : config_.lowerPaths) {
        if (HasWhiteout(relativePath, lower)) {
            return true;
        }
    }
    return false;
}

NTSTATUS WhiteoutManager::CreateWhiteout(const std::wstring& relativePath,
                                         WhiteoutType type) {
    if (type == WhiteoutType::Opaque) {
        return SetOpaque(relativePath);
    }

    const NTSTATUS directoryStatus =
        MarkerDirectoryStatus(FindLinkInView(config_, *this, MarkerDirectoryOf(relativePath)));
    if (!NT_SUCCESS(directoryStatus)) {
        return directoryStatus;
    }

    std::wstring whPath = GetWhiteoutFullPath(config_.upperPath, relativePath);

    fs::path parentDir = fs::path(whPath).parent_path();
    if (!parentDir.empty()) {
        EnsureDirectoryExists(parentDir.wstring());
    }

    const DWORD markerError = CreateHiddenMarkerFile(whPath);
    if (markerError != ERROR_SUCCESS) {
        return NtStatusFromWin32(markerError);
    }

    if (cache_) {
        cache_->InvalidateWithAncestors(NormalizePath(relativePath));
    }

    if (events_ != nullptr) {
        events_->Emit(LM_EVT_WHITEOUT_CREATED, S_OK, relativePath.c_str(), nullptr);
    }
    return STATUS_SUCCESS;
}

NTSTATUS WhiteoutManager::RemoveWhiteout(const std::wstring& relativePath) {
    switch (FindLinkOnPath(config_.upperPath, MarkerDirectoryOf(relativePath))) {
    case LinkOnPath::Self:
    case LinkOnPath::Ancestor:
        return STATUS_SUCCESS;
    case LinkOnPath::Unreadable:
        return STATUS_ACCESS_DENIED;
    case LinkOnPath::None:
        break;
    }

    std::wstring whPath = GetWhiteoutFullPath(config_.upperPath, relativePath);

    if (!DeleteFileW(WithExtendedPrefix(whPath).c_str())) {
        const DWORD deleteErr = GetLastError();
        if (deleteErr != ERROR_FILE_NOT_FOUND) {
            return StatusFromWin32Error(deleteErr, ERROR_WRITE_FAULT);
        }
    }

    if (cache_) {
        cache_->InvalidateWithAncestors(NormalizePath(relativePath));
    }

    return STATUS_SUCCESS;
}

bool WhiteoutManager::HoldsOpaqueMarker(const std::wstring& dirFullPath) const {
    return MetadataStore::HasOpaqueMetadata(dirFullPath, &config_) ||
           MarkerFileExists(JoinDirPath(dirFullPath, kOpaqueMarkerFile));
}

bool WhiteoutManager::IsOpaque(const std::wstring& dirRelativePath) const {
    return IsOpaqueInLayer(dirRelativePath, config_.upperPath);
}

bool WhiteoutManager::IsOpaqueInLayer(const std::wstring& dirRelativePath,
                                       const std::wstring& layerPath) const {
    return HoldsOpaqueMarker(JoinDirPath(layerPath, dirRelativePath)) &&
           FindLinkOnPath(layerPath, NormalizePathPreserveCase(dirRelativePath)) ==
               LinkOnPath::None;
}

bool WhiteoutManager::IsOpaqueWalkedDirectoryInLayer(const std::wstring& dirRelativePath,
                                                      const std::wstring& layerPath) const {
    return HoldsOpaqueMarker(JoinDirPath(layerPath, dirRelativePath));
}

NTSTATUS WhiteoutManager::SetOpaque(const std::wstring& dirRelativePath) {
    const std::wstring normalized = NormalizePathPreserveCase(dirRelativePath);
    const NTSTATUS directoryStatus = MarkerDirectoryStatus(FindLinkInView(config_, *this, normalized));
    if (!NT_SUCCESS(directoryStatus)) {
        return directoryStatus;
    }

    std::wstring dirFullPath = JoinDirPath(config_.upperPath, normalized);

    EnsureDirectoryExists(dirFullPath);

    const NTSTATUS status = WriteOpaqueMarkers(dirFullPath);

    if (cache_) {
        cache_->Invalidate(NormalizePath(dirRelativePath));
    }
    return status;
}

NTSTATUS WhiteoutManager::SetOpaqueAtPath(const std::wstring& dirFullPath) {
    const DWORD attributes = ::GetFileAttributesW(WithExtendedPrefix(dirFullPath).c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        bool isLink = false;
        const NTSTATUS probe = IsDirectoryLink(dirFullPath, attributes, &isLink);
        if (!NT_SUCCESS(probe)) {
            return probe;
        }
        if (isLink) {
            return STATUS_NOT_A_DIRECTORY;
        }
    }
    return WriteOpaqueMarkers(dirFullPath);
}

NTSTATUS WhiteoutManager::WriteOpaqueMarkers(const std::wstring& dirFullPath) {
    const std::optional<EntryTimes> before = ReadEntryTimes(dirFullPath);

    const bool metadataOk = MetadataStore::SetOpaqueMetadata(dirFullPath, &config_);
    const DWORD markerError =
        CreateHiddenMarkerFile(JoinDirPath(dirFullPath, kOpaqueMarkerFile));

    if (before.has_value()) {
        WriteEntryTimes(dirFullPath, *before, std::nullopt);
    }

    // Either marker makes IsOpaqueInLayer report the directory opaque. A
    // layer image packs the `.wh..wh..opq` file but not the `:overlay.opaque`
    // stream.
    if (metadataOk || markerError == ERROR_SUCCESS) {
        return STATUS_SUCCESS;
    }
    return NtStatusFromWin32(markerError);
}

bool WhiteoutManager::RemoveOpaque(const std::wstring& dirRelativePath) {
    const std::wstring normalized = NormalizePathPreserveCase(dirRelativePath);
    const LinkOnPath link = FindLinkOnPath(config_.upperPath, normalized);
    if (link == LinkOnPath::Ancestor) {
        return true;
    }
    if (link == LinkOnPath::Unreadable) {
        return false;
    }

    std::wstring dirFullPath = JoinDirPath(config_.upperPath, normalized);

    const bool metadataOk = MetadataStore::RemoveOpaqueMetadata(dirFullPath, &config_);

    const bool fileOk =
        link == LinkOnPath::Self || DeleteMarkerFile(JoinDirPath(dirFullPath, kOpaqueMarkerFile));

    if (cache_) {
        cache_->Invalidate(NormalizePath(dirRelativePath));
    }

    return metadataOk && fileOk;
}

bool WhiteoutManager::HasOpaqueSelfOrAncestorInLayer(const std::wstring& dirRelativePath,
                                                      const std::wstring& layerPath) const {
    return AnyDirectoryUpToRoot(fs::path(dirRelativePath), LayerRoot::Probed,
                                [&](const std::wstring& dir) {
                                    return IsOpaqueInLayer(dir, layerPath);
                                });
}

bool WhiteoutManager::HasWhitedOutAncestorInLayer(const std::wstring& relativePath,
                                                   const std::wstring& layerPath) const {
    return AnyDirectoryUpToRoot(fs::path(relativePath).parent_path(), LayerRoot::Skipped,
                                [&](const std::wstring& dir) {
                                    return HasWhiteout(dir, layerPath);
                                });
}

}
