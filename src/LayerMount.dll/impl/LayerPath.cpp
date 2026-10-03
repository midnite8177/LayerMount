#include "LayerPath.h"
#include "MetadataStore.h"
#include "NtStatusUtil.h"
#include "WhiteoutManager.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace LayerMount {

namespace fs = std::filesystem;

std::wstring DirWithSeparator(const std::wstring& dirPath) {
    return !dirPath.empty() && dirPath.back() == L'\\' ? dirPath : dirPath + L"\\";
}

std::wstring JoinDirPath(const std::wstring& dirPath,
                         const std::wstring& relativePath) {
    return DirWithSeparator(dirPath) + relativePath;
}

std::wstring NormalizePathPreserveCase(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    std::wstring result = path;
    std::replace(result.begin(), result.end(), L'/', L'\\');

    size_t start = 0;
    while (start < result.size() && result[start] == L'\\') {
        ++start;
    }
    if (start > 0) {
        result = result.substr(start);
    }

    while (!result.empty() && result.back() == L'\\') {
        result.pop_back();
    }

    return result;
}

bool IsInsideDirectory(std::wstring_view pathNorm, std::wstring_view dirNorm) {
    return pathNorm.size() > dirNorm.size() &&
           pathNorm.compare(0, dirNorm.size(), dirNorm) == 0 &&
           pathNorm[dirNorm.size()] == L'\\';
}

std::wstring BuildUpperPathPreserveCase(const std::wstring& upperRoot,
                                        const std::wstring& relativePath) {
    std::wstring preserved = NormalizePathPreserveCase(relativePath);
    if (preserved.empty()) return upperRoot;
    return JoinDirPath(upperRoot, preserved);
}

std::wstring WithStoredLeafName(const std::wstring& targetPath,
                                const std::wstring& entryPath) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW(entryPath.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return targetPath;
    }
    ::FindClose(find);
    return targetPath.substr(0, targetPath.find_last_of(L'\\') + 1) + fd.cFileName;
}

bool IsEnumerableDirectory(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

NTSTATUS IsDirectoryLink(const std::wstring& path, DWORD attributes, bool* isLink) {
    *isLink = false;
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return STATUS_INVALID_PARAMETER;
    }
    constexpr DWORD kDirectoryReparsePoint =
        FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    if ((attributes & kDirectoryReparsePoint) != kDirectoryReparsePoint) {
        return STATUS_SUCCESS;
    }
    HANDLE entry = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (entry == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }
    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    const BOOL read = ::GetFileInformationByHandleEx(
        entry, FileAttributeTagInfo, &tagInfo, sizeof(tagInfo));
    const DWORD readErr = read ? 0 : ::GetLastError();
    ::CloseHandle(entry);
    if (!read) {
        return NtStatusFromWin32(readErr);
    }
    *isLink = IsReparseTagNameSurrogate(tagInfo.ReparseTag);
    return STATUS_SUCCESS;
}

NTSTATUS EntryKindOf(const std::wstring& path, DWORD attributes, RenameEntryKind* kind) {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        *kind = RenameEntryKind::File;
        return STATUS_SUCCESS;
    }
    bool isLink = false;
    const NTSTATUS status = IsDirectoryLink(path, attributes, &isLink);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *kind = isLink ? RenameEntryKind::Link : RenameEntryKind::Directory;
    return STATUS_SUCCESS;
}

namespace {

NTSTATUS MoveUpperEntryOnDisk(const std::wstring& from,
                              const std::wstring& to,
                              ReplaceExisting replace,
                              CopyAcrossVolumes copy) {
    const DWORD flags =
        (replace == ReplaceExisting::Yes ? MOVEFILE_REPLACE_EXISTING : 0) |
        (copy == CopyAcrossVolumes::Yes ? MOVEFILE_COPY_ALLOWED : 0);
    if (::MoveFileExW(from.c_str(), to.c_str(), flags)) {
        return STATUS_SUCCESS;
    }
    const DWORD moveErr = ::GetLastError();
    if (moveErr != ERROR_ACCESS_DENIED) {
        return NtStatusFromWin32(moveErr);
    }

    // When a copy-up carried an inherited deny-write ACE from the lower
    // parent to the upper parent, MoveFileExW fails the destination DACL
    // check although the engine owns the upper entry. With SE_RESTORE_NAME,
    // a handle opened with backup semantics skips that check through
    // FileRenameInfo. FILE_FLAG_OPEN_REPARSE_POINT opens a junction or a
    // symbolic link itself. Without it, the handle is the link's target, and
    // the rename moves the target.
    HANDLE src = ::CreateFileW(from.c_str(),
        GENERIC_READ | DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (src == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }
    const size_t pathBytes = to.size() * sizeof(wchar_t);
    std::vector<BYTE> buf(sizeof(FILE_RENAME_INFO) + pathBytes);
    auto* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
    ri->ReplaceIfExists = replace == ReplaceExisting::Yes ? TRUE : FALSE;
    ri->RootDirectory   = nullptr;
    ri->FileNameLength  = static_cast<DWORD>(pathBytes);
    std::memcpy(ri->FileName, to.data(), pathBytes);
    const BOOL renamed = ::SetFileInformationByHandle(
        src, FileRenameInfo, ri, static_cast<DWORD>(buf.size()));
    const DWORD renameErr = renamed ? 0 : ::GetLastError();
    ::CloseHandle(src);
    if (!renamed) {
        return NtStatusFromWin32(renameErr);
    }
    return STATUS_SUCCESS;
}

NTSTATUS RemoveUpperEntryOnDisk(const std::wstring& path) {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD probeErr = ::GetLastError();
        if (probeErr == ERROR_FILE_NOT_FOUND || probeErr == ERROR_PATH_NOT_FOUND) {
            return STATUS_SUCCESS;
        }
        return NtStatusFromWin32(probeErr);
    }

    if (IsEnumerableDirectory(attrs)) {
        std::error_code ec;
        fs::remove_all(path, ec);
        return ec ? NtStatusFromWin32(static_cast<DWORD>(ec.value())) : STATUS_SUCCESS;
    }

    const bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const BOOL removed = isDirectory ? ::RemoveDirectoryW(path.c_str())
                                     : ::DeleteFileW(path.c_str());
    if (!removed) {
        const DWORD removeErr = ::GetLastError();
        if (removeErr != ERROR_FILE_NOT_FOUND && removeErr != ERROR_PATH_NOT_FOUND) {
            return NtStatusFromWin32(removeErr);
        }
    }
    return STATUS_SUCCESS;
}

}

NTSTATUS MoveUpperEntry(const std::wstring& from,
                        const std::wstring& to,
                        ReplaceExisting replace,
                        CopyAcrossVolumes copy,
                        const LayerConfig& config) {
    const NTSTATUS status = MoveUpperEntryOnDisk(from, to, replace, copy);
    if (NT_SUCCESS(status)) {
        MetadataStore::MoveSidecarRecords(from, to, config);
    }
    return status;
}

NTSTATUS RemoveUpperEntry(const std::wstring& path, const LayerConfig& config) {
    const std::vector<std::wstring> entries =
        MetadataStore::ListSidecarKeyedEntries(path, config);
    const NTSTATUS status = RemoveUpperEntryOnDisk(path);
    MetadataStore::RemoveSidecarRecordsOfGoneEntries(entries, config);
    return status;
}

NTSTATUS CreateDirectoryOrUseExisting(const std::wstring& path) {
    if (::CreateDirectoryW(path.c_str(), nullptr)) {
        return STATUS_SUCCESS;
    }
    const DWORD createErr = ::GetLastError();
    if (createErr != ERROR_ALREADY_EXISTS) {
        return NtStatusFromWin32(createErr);
    }
    const DWORD existingAttrs = ::GetFileAttributesW(path.c_str());
    if (existingAttrs == INVALID_FILE_ATTRIBUTES) {
        return NtStatusFromWin32(::GetLastError());
    }
    if ((existingAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return STATUS_OBJECT_NAME_COLLISION;
    }
    return STATUS_SUCCESS;
}

std::wstring JoinLayerScanPath(const std::wstring& layerPath,
                               const std::wstring& dirRelativePath) {
    return JoinDirPath(layerPath,
                       dirRelativePath.empty() ? L"*" : dirRelativePath + L"\\*");
}

namespace {

enum class WalkStop {
    None,
    File,
    Link,
    Unreadable,
};

// Where a walk of a path in one layer stopped. component is the path of the
// component that stopped it, relative to the layer root, and is empty when
// stop is WalkStop::None.
struct LayerWalk {
    WalkStop stop;
    std::wstring component;
};

// Walks dirRelativePath from the layer root to the first component that is
// not a directory, as HasNonDirectoryOrLinkSelfOrAncestorInLayer describes.
// A component that does not exist ends the walk with WalkStop::None.
LayerWalk WalkToFirstNonDirectory(const std::wstring& layerPath,
                                  const std::wstring& dirRelativePath) {
    fs::path walked;
    for (const fs::path& component : fs::path(dirRelativePath)) {
        walked /= component;
        const std::wstring componentPath = JoinDirPath(layerPath, walked.wstring());
        const DWORD attrs = GetFileAttributesW(componentPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                return LayerWalk{WalkStop::None, {}};
            }
            return LayerWalk{WalkStop::Unreadable, walked.wstring()};
        }
        RenameEntryKind kind = RenameEntryKind::Directory;
        if (!NT_SUCCESS(EntryKindOf(componentPath, attrs, &kind))) {
            return LayerWalk{WalkStop::Unreadable, walked.wstring()};
        }
        if (kind == RenameEntryKind::File) {
            return LayerWalk{WalkStop::File, walked.wstring()};
        }
        if (kind == RenameEntryKind::Link) {
            return LayerWalk{WalkStop::Link, walked.wstring()};
        }
    }
    return LayerWalk{WalkStop::None, {}};
}

// Whether the layer holds an entry at relativePath. A failure to read the
// entry's attributes, other than a missing path, counts as holding it, so an
// unreadable higher layer hides a lower link.
bool HoldsEntryInLayer(const std::wstring& layerPath, const std::wstring& relativePath) {
    if (GetFileAttributesW(JoinDirPath(layerPath, relativePath).c_str()) !=
        INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    const DWORD error = ::GetLastError();
    return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
}

}

bool HasNonDirectoryOrLinkSelfOrAncestorInLayer(const std::wstring& layerPath,
                                                const std::wstring& dirRelativePath) {
    return WalkToFirstNonDirectory(layerPath, dirRelativePath).stop != WalkStop::None;
}

bool HasLinkUnderHigherLayerEntry(const LayerConfig& config,
                                  size_t lowerIndex,
                                  const std::wstring& dirRelativePath) {
    const LayerWalk walk = WalkToFirstNonDirectory(config.lowerPaths[lowerIndex], dirRelativePath);
    if (walk.stop != WalkStop::Link) {
        return false;
    }
    if (HoldsEntryInLayer(config.upperPath, walk.component)) {
        return true;
    }
    for (size_t higher = 0; higher < lowerIndex; ++higher) {
        if (HoldsEntryInLayer(config.lowerPaths[higher], walk.component)) {
            return true;
        }
    }
    return false;
}

LowerVisibility LowersBelow(const LayerDirectory& dir) {
    if (dir.whiteoutMgr.HasOpaqueSelfOrAncestorInLayer(dir.dirNorm, dir.layerPath)) {
        return LowerVisibility::HiddenByOpaqueMarker;
    }
    if (HasNonDirectoryOrLinkSelfOrAncestorInLayer(dir.layerPath, dir.dirNorm)) {
        return LowerVisibility::HiddenByNonDirectoryOrLink;
    }
    return LowerVisibility::Visible;
}

}
