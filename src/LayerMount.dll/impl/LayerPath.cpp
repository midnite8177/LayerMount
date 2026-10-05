#include "LayerPath.h"
#include "MetadataStore.h"
#include "NtStatusUtil.h"
#include "WhiteoutManager.h"
#include "EntryCopy.h"

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

std::wstring CaseFoldedName(const std::wstring& name) {
    std::wstring folded = name;
    CharLowerBuffW(folded.data(), static_cast<DWORD>(folded.size()));
    return folded;
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

namespace {

// Reads the tag of the entry at path, not of its target. Opens a file or a
// directory.
NTSTATUS ReadReparseTag(const std::wstring& path, DWORD* tag) {
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
    *tag = tagInfo.ReparseTag;
    return STATUS_SUCCESS;
}

// The user-mode SDK headers define no names for the WSL FIFO, character
// device and block device tags.
constexpr DWORD kReparseTagLxFifo = 0x80000024;
constexpr DWORD kReparseTagLxChr = 0x80000025;
constexpr DWORD kReparseTagLxBlk = 0x80000026;

// A reparse point is a link when its tag is a name surrogate, such as the
// tag of a symbolic link or of a junction.
bool IsLinkReparseTag(DWORD tag) {
    return IsReparseTagNameSurrogate(tag);
}

// The reparse data of these tags is the whole entry, so a data copy loses
// it. Overlayfs also copies up a WSL special file as a special file.
bool IsSelfContainedFileReparseTag(DWORD tag) {
    return tag == IO_REPARSE_TAG_AF_UNIX || tag == kReparseTagLxFifo ||
           tag == kReparseTagLxChr || tag == kReparseTagLxBlk ||
           tag == IO_REPARSE_TAG_APPEXECLINK;
}

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
    DWORD tag = 0;
    const NTSTATUS status = ReadReparseTag(path, &tag);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *isLink = IsLinkReparseTag(tag);
    return STATUS_SUCCESS;
}

NTSTATUS EntryKindOf(const std::wstring& path, DWORD attributes, EntryKind* kind) {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        *kind = EntryKind::File;
        return STATUS_SUCCESS;
    }
    bool isLink = false;
    const NTSTATUS status = IsDirectoryLink(path, attributes, &isLink);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *kind = isLink ? EntryKind::Link : EntryKind::Directory;
    return STATUS_SUCCESS;
}

NTSTATUS ClonesReparsePoint(const std::wstring& path, DWORD attributes, bool* clones) {
    *clones = false;
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return STATUS_SUCCESS;
    }
    DWORD tag = 0;
    const NTSTATUS status = ReadReparseTag(path, &tag);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    *clones = IsLinkReparseTag(tag) || (!isDirectory && IsSelfContainedFileReparseTag(tag));
    return STATUS_SUCCESS;
}

namespace {

NTSTATUS MoveUpperEntryOnDisk(const std::wstring& from,
                              const std::wstring& to,
                              ReplaceExisting replace) {
    const DWORD flags = replace == ReplaceExisting::Yes ? MOVEFILE_REPLACE_EXISTING : 0;
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

NTSTATUS RemoveUpperEntryOnDisk(const std::wstring& path, EntryKind kind) {
    if (kind == EntryKind::Directory) {
        std::error_code ec;
        fs::remove_all(path, ec);
        return ec ? NtStatusFromWin32(static_cast<DWORD>(ec.value())) : STATUS_SUCCESS;
    }

    const BOOL removed = kind == EntryKind::Link ? ::RemoveDirectoryW(path.c_str())
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
                        const LayerConfig& config) {
    const NTSTATUS status = MoveUpperEntryOnDisk(from, to, replace);
    if (NT_SUCCESS(status)) {
        MetadataStore::MoveSidecarRecords(from, to, config);
    }
    return status;
}

NTSTATUS ProbeUpperEntry(const std::wstring& path, bool* exists, EntryKind* kind) {
    *exists = false;
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD probeErr = ::GetLastError();
        if (probeErr == ERROR_FILE_NOT_FOUND || probeErr == ERROR_PATH_NOT_FOUND) {
            return STATUS_SUCCESS;
        }
        return NtStatusFromWin32(probeErr);
    }
    const NTSTATUS kindStatus = EntryKindOf(path, attrs, kind);
    if (kindStatus == STATUS_OBJECT_NAME_NOT_FOUND || kindStatus == STATUS_OBJECT_PATH_NOT_FOUND) {
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(kindStatus)) {
        return kindStatus;
    }
    *exists = true;
    return STATUS_SUCCESS;
}

NTSTATUS RemoveUpperEntryOfKind(const std::wstring& path,
                                EntryKind kind,
                                const LayerConfig& config) {
    const std::vector<std::wstring> entries =
        MetadataStore::ListSidecarKeyedEntries(path, config);
    const NTSTATUS status = RemoveUpperEntryOnDisk(path, kind);
    MetadataStore::RemoveSidecarRecordsOfGoneEntries(entries, config);
    return status;
}

NTSTATUS RemoveUpperEntry(const std::wstring& path, const LayerConfig& config) {
    bool exists = false;
    EntryKind kind = EntryKind::File;
    const NTSTATUS probe = ProbeUpperEntry(path, &exists, &kind);
    if (!NT_SUCCESS(probe) || !exists) {
        return probe;
    }
    return RemoveUpperEntryOfKind(path, kind, config);
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

// Where a walk of a path in one layer stopped. component is the path of the
// component that stopped it, relative to the layer root, and is empty when
// stop is WalkStop::None.
struct LayerWalk {
    WalkStop stop;
    std::wstring component;
};

// Where a walk stops at a component of this kind. A directory or a missing
// component gives WalkStop::None.
WalkStop WalkStopAt(ComponentKind kind) {
    switch (kind) {
    case ComponentKind::File:
        return WalkStop::File;
    case ComponentKind::Link:
        return WalkStop::Link;
    case ComponentKind::Unreadable:
        return WalkStop::Unreadable;
    case ComponentKind::Missing:
    case ComponentKind::Directory:
        break;
    }
    return WalkStop::None;
}

// Walks dirRelativePath from the layer root to the first component that is
// not a directory, as HasNonDirectoryOrLinkSelfOrAncestorInLayer describes.
// A component that does not exist ends the walk with WalkStop::None.
LayerWalk WalkToFirstNonDirectory(const std::wstring& layerPath,
                                  const std::wstring& dirRelativePath) {
    fs::path walked;
    for (const fs::path& component : fs::path(dirRelativePath)) {
        walked /= component;
        switch (ComponentKindInLayer(layerPath, walked.wstring())) {
        case ComponentKind::Missing:
            return LayerWalk{WalkStop::None, {}};
        case ComponentKind::Unreadable:
            return LayerWalk{WalkStop::Unreadable, walked.wstring()};
        case ComponentKind::File:
            return LayerWalk{WalkStop::File, walked.wstring()};
        case ComponentKind::Link:
            return LayerWalk{WalkStop::Link, walked.wstring()};
        case ComponentKind::Directory:
            break;
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

ComponentKind ComponentKindInLayer(const std::wstring& layerPath,
                                   const std::wstring& relativePath) {
    const std::wstring componentPath = JoinDirPath(layerPath, relativePath);
    const DWORD attrs = GetFileAttributesW(componentPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? ComponentKind::Missing
            : ComponentKind::Unreadable;
    }
    EntryKind kind = EntryKind::Directory;
    if (!NT_SUCCESS(EntryKindOf(componentPath, attrs, &kind))) {
        return ComponentKind::Unreadable;
    }
    switch (kind) {
    case EntryKind::File:
        return ComponentKind::File;
    case EntryKind::Link:
        return ComponentKind::Link;
    case EntryKind::Directory:
        break;
    }
    return ComponentKind::Directory;
}

bool HasNonDirectoryOrLinkSelfOrAncestorInLayer(const std::wstring& layerPath,
                                                const std::wstring& dirRelativePath) {
    return WalkToFirstNonDirectory(layerPath, dirRelativePath).stop != WalkStop::None;
}

LinkOnPath FindLinkOnPath(const std::wstring& layerPath, const std::wstring& dirRelativePath) {
    const LayerWalk walk = WalkToFirstNonDirectory(layerPath, dirRelativePath);
    switch (walk.stop) {
    case WalkStop::Link:
        return walk.component == dirRelativePath ? LinkOnPath::Self : LinkOnPath::Ancestor;
    case WalkStop::Unreadable:
        return LinkOnPath::Unreadable;
    case WalkStop::None:
    case WalkStop::File:
        break;
    }
    return LinkOnPath::None;
}

bool HasLinkUnderHigherLayerEntry(const LayerConfig& config,
                                  size_t lowerIndex,
                                  const std::wstring& dirRelativePath) {
    const LayerWalk walk = WalkToFirstNonDirectory(config.lowerPaths[lowerIndex], dirRelativePath);
    return walk.stop == WalkStop::Link &&
           HigherLayerHoldsEntry(config, lowerIndex, walk.component);
}

bool HigherLayerHoldsEntry(const LayerConfig& config,
                           size_t lowerIndex,
                           const std::wstring& relativePath) {
    if (HoldsEntryInLayer(config.upperPath, relativePath)) {
        return true;
    }
    for (size_t higher = 0; higher < lowerIndex; ++higher) {
        if (HoldsEntryInLayer(config.lowerPaths[higher], relativePath)) {
            return true;
        }
    }
    return false;
}

LayerAncestry StepLayerAncestry(const LayerConfig& config,
                                const LayerDirectory& dir,
                                int lowerIndex,
                                const LayerAncestry& parent) {
    if (parent.absent) {
        return parent;
    }
    LayerAncestry ancestry = parent;
    if (parent.firstNonDirectory != WalkStop::None) {
        return ancestry;
    }
    ancestry.whitedOut =
        parent.whitedOut ||
        dir.whiteoutMgr.HasWhiteoutUnderWalkedDirectoryInLayer(dir.dirNorm, dir.layerPath);
    const ComponentKind kind = ComponentKindInLayer(dir.layerPath, dir.dirNorm);
    ancestry.opaque =
        parent.opaque ||
        (kind == ComponentKind::Directory &&
         dir.whiteoutMgr.IsOpaqueWalkedDirectoryInLayer(dir.dirNorm, dir.layerPath));
    ancestry.absent = kind == ComponentKind::Missing;
    ancestry.firstNonDirectory = WalkStopAt(kind);
    ancestry.linkUnderHigherEntry =
        kind == ComponentKind::Link && lowerIndex >= 0 &&
        HigherLayerHoldsEntry(config, static_cast<size_t>(lowerIndex), dir.dirNorm);
    return ancestry;
}

LayerAncestry LayerAncestryOf(const LayerConfig& config,
                              const LayerDirectory& dir,
                              int lowerIndex) {
    LayerAncestry ancestry{false, dir.whiteoutMgr.IsOpaqueInLayer(std::wstring(), dir.layerPath),
                           WalkStop::None, false, false};
    fs::path walked;
    for (const fs::path& component : fs::path(dir.dirNorm)) {
        walked /= component;
        const std::wstring walkedPath = walked.wstring();
        ancestry = StepLayerAncestry(
            config, LayerDirectory{dir.whiteoutMgr, dir.layerPath, walkedPath}, lowerIndex, ancestry);
    }
    return ancestry;
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

NTSTATUS BuildInContainerAndMove(const std::wstring& containerPath,
                                 const std::wstring& upperPath,
                                 const LayerConfig& config,
                                 const std::function<NTSTATUS(const std::wstring&)>& build) {
    const std::wstring stagedPath = containerPath + L"\\entry";
    NTSTATUS status = ::CreateDirectoryW(containerPath.c_str(), nullptr)
        ? WriteSecurityToInheritAs(containerPath, fs::path(upperPath).parent_path().wstring())
        : StatusOfFailedCall(ERROR_WRITE_FAULT);
    if (NT_SUCCESS(status)) {
        status = build(stagedPath);
    }
    if (NT_SUCCESS(status)) {
        status = MoveUpperEntry(stagedPath, upperPath, ReplaceExisting::No, config);
    }
    RemoveUpperEntry(containerPath, config);
    return status;
}

}
