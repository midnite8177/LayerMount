#include "LayerPath.h"
#include "MetadataStore.h"
#include "NtStatusUtil.h"
#include "ScopedHandle.h"
#include "WhiteoutManager.h"
#include "EntryCopy.h"

#include <winioctl.h>
#include <algorithm>
#include <cstdint>
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

namespace {

constexpr std::wstring_view kExtended = L"\\\\?\\";
constexpr std::wstring_view kExtendedUnc = L"\\\\?\\UNC\\";
constexpr std::wstring_view kDevice = L"\\\\.\\";
constexpr std::wstring_view kUnc = L"\\\\";

std::optional<std::wstring> FullPathName(const std::wstring& path) {
    const DWORD size = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring full(size, L'\0');
    const DWORD written = ::GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
    if (written == 0 || written >= size) {
        return std::nullopt;
    }
    full.resize(written);
    return full;
}

// Whether path is a full drive or UNC path with backslash separators and no
// empty, "." or ".." component, so that it needs only the prefix.
bool IsFullPathAsWritten(std::wstring_view path) {
    size_t start = 0;
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') {
        start = 3;
    } else if (path.rfind(kUnc, 0) == 0 && path.size() > kUnc.size() &&
               path[kUnc.size()] != L'\\') {
        start = kUnc.size();
    } else {
        return false;
    }
    if (path.find(L'/') != std::wstring_view::npos) {
        return false;
    }
    for (size_t pos = start;;) {
        const size_t end = std::min(path.find(L'\\', pos), path.size());
        const std::wstring_view component = path.substr(pos, end - pos);
        if (component.empty() || component == L"." || component == L"..") {
            return false;
        }
        if (end == path.size()) {
            return true;
        }
        pos = end + 1;
    }
}

}

std::wstring WithoutExtendedPrefix(const std::wstring& path) {
    if (path.rfind(kExtendedUnc, 0) == 0) {
        return std::wstring(kUnc) + path.substr(kExtendedUnc.size());
    }
    if (path.rfind(kExtended, 0) == 0 && path.size() >= kExtended.size() + 2 &&
        path[kExtended.size() + 1] == L':') {
        return path.substr(kExtended.size());
    }
    return path;
}

std::wstring WithExtendedPrefix(const std::wstring& path) {
    if (path.rfind(kExtended, 0) == 0 || path.rfind(kDevice, 0) == 0) {
        return path;
    }
    std::wstring full = path;
    if (!IsFullPathAsWritten(path)) {
        std::optional<std::wstring> resolved = FullPathName(path);
        if (!resolved.has_value()) {
            return path;
        }
        full = std::move(*resolved);
    }
    if (full.rfind(kUnc, 0) == 0) {
        return std::wstring(kExtendedUnc) + full.substr(kUnc.size());
    }
    return std::wstring(kExtended) + full;
}

std::wstring ExtendedStreamPath(const std::wstring& path, std::wstring_view streamSuffix) {
    std::wstring streamPath = WithExtendedPrefix(path);
    streamPath += streamSuffix;
    return streamPath;
}

std::wstring ComparablePath(const std::wstring& path) {
    std::wstring plain = path;
    std::replace(plain.begin(), plain.end(), L'/', L'\\');
    plain = WithoutExtendedPrefix(plain);
    std::optional<std::wstring> full = FullPathName(plain);
    if (full.has_value()) {
        plain = std::move(*full);
    }
    while (!plain.empty() && plain.back() == L'\\') {
        plain.pop_back();
    }
    CharLowerBuffW(plain.data(), static_cast<DWORD>(plain.size()));
    return plain;
}

std::wstring FinalPathNameOf(HANDLE handle) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD size = ::GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (size == 0) {
        return {};
    }
    std::wstring path(size, L'\0');
    const DWORD length = ::GetFinalPathNameByHandleW(handle, path.data(), size, flags);
    if (length == 0 || length >= size) {
        return {};
    }
    path.resize(length);
    return path;
}

std::wstring FinalPathOfDirectory(const std::wstring& path) {
    const ScopedHandle directory(::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!directory.IsValid()) {
        return {};
    }
    return FinalPathNameOf(directory.Get());
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
    HANDLE find = ::FindFirstFileW(WithExtendedPrefix(entryPath).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return targetPath;
    }
    ::FindClose(find);
    return targetPath.substr(0, targetPath.find_last_of(L'\\') + 1) + fd.cFileName;
}

ScopedHandle OpenReparseEntry(const std::wstring& path, DWORD access) {
    return ScopedHandle(::CreateFileW(WithExtendedPrefix(path).c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
}

namespace {

// Reads the tag of the entry at path, not of its target.
NTSTATUS ReadReparseTag(const std::wstring& path, DWORD* tag) {
    const ScopedHandle entry = OpenReparseEntry(path, FILE_READ_ATTRIBUTES);
    if (!entry.IsValid()) {
        return NtStatusFromWin32(::GetLastError());
    }
    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    if (!::GetFileInformationByHandleEx(
            entry.Get(), FileAttributeTagInfo, &tagInfo, sizeof(tagInfo))) {
        return NtStatusFromWin32(::GetLastError());
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

// The user-mode SDK headers declare no REPARSE_DATA_BUFFER, so this file
// declares the layout of a symbolic link's reparse data.
#pragma pack(push, 1)
struct SymbolicLinkReparseHeader {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    ULONG Flags;
};
#pragma pack(pop)

constexpr ULONG kSymlinkFlagRelative = 0x1;

}

ReparseLink ReadReparseLink(const std::wstring& path, std::wstring* relativeTarget) {
    relativeTarget->clear();
    const ScopedHandle entry = OpenReparseEntry(path, FILE_READ_ATTRIBUTES);
    if (!entry.IsValid()) {
        return ReparseLink::OtherLink;
    }
    std::vector<BYTE> data(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD returned = 0;
    const BOOL read = ::DeviceIoControl(entry.Get(), FSCTL_GET_REPARSE_POINT, nullptr, 0,
                                        data.data(), static_cast<DWORD>(data.size()),
                                        &returned, nullptr);
    if (!read || returned < sizeof(ULONG)) {
        return ReparseLink::OtherLink;
    }
    ULONG tag = 0;
    std::memcpy(&tag, data.data(), sizeof(tag));
    if (!IsLinkReparseTag(tag)) {
        return ReparseLink::None;
    }
    if (tag != IO_REPARSE_TAG_SYMLINK || returned < sizeof(SymbolicLinkReparseHeader)) {
        return ReparseLink::OtherLink;
    }
    SymbolicLinkReparseHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    const size_t nameStart = sizeof(header) + header.SubstituteNameOffset;
    if ((header.Flags & kSymlinkFlagRelative) == 0 ||
        nameStart + header.SubstituteNameLength > returned) {
        return ReparseLink::OtherLink;
    }
    std::wstring target(header.SubstituteNameLength / sizeof(wchar_t), L'\0');
    std::memcpy(target.data(), data.data() + nameStart, target.size() * sizeof(wchar_t));
    if (target.find(L':') != std::wstring::npos) {
        return ReparseLink::OtherLink;
    }
    *relativeTarget = std::move(target);
    return ReparseLink::RelativeSymlink;
}

namespace {

NTSTATUS MoveUpperEntryOnDisk(const std::wstring& from,
                              const std::wstring& to,
                              ReplaceExisting replace) {
    const std::wstring extendedFrom = WithExtendedPrefix(from);
    const std::wstring extendedTo = WithExtendedPrefix(to);
    const DWORD flags = replace == ReplaceExisting::Yes ? MOVEFILE_REPLACE_EXISTING : 0;
    if (::MoveFileExW(extendedFrom.c_str(), extendedTo.c_str(), flags)) {
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
    HANDLE src = ::CreateFileW(extendedFrom.c_str(),
        GENERIC_READ | DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (src == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }
    const size_t pathBytes = extendedTo.size() * sizeof(wchar_t);
    std::vector<BYTE> buf(sizeof(FILE_RENAME_INFO) + pathBytes);
    auto* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
    ri->ReplaceIfExists = replace == ReplaceExisting::Yes ? TRUE : FALSE;
    ri->RootDirectory   = nullptr;
    ri->FileNameLength  = static_cast<DWORD>(pathBytes);
    std::memcpy(ri->FileName, extendedTo.data(), pathBytes);
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
    const std::wstring extendedPath = WithExtendedPrefix(path);
    if (kind == EntryKind::Directory) {
        std::error_code ec;
        fs::remove_all(extendedPath, ec);
        return ec ? NtStatusFromWin32(static_cast<DWORD>(ec.value())) : STATUS_SUCCESS;
    }

    const BOOL removed = kind == EntryKind::Link ? ::RemoveDirectoryW(extendedPath.c_str())
                                                 : ::DeleteFileW(extendedPath.c_str());
    if (!removed) {
        const DWORD removeErr = ::GetLastError();
        if (removeErr != ERROR_FILE_NOT_FOUND && removeErr != ERROR_PATH_NOT_FOUND) {
            return NtStatusFromWin32(removeErr);
        }
    }
    return STATUS_SUCCESS;
}

}

UpperEntryMove MoveUpperEntry(const std::wstring& from,
                              const std::wstring& to,
                              ReplaceExisting replace,
                              const LayerConfig& config) {
    const NTSTATUS status = MoveUpperEntryOnDisk(from, to, replace);
    if (!NT_SUCCESS(status)) {
        return {status, STATUS_SUCCESS};
    }
    std::vector<MetadataStore::MovedSidecarRecord> moved;
    const NTSTATUS recordStatus = MetadataStore::MoveSidecarRecords(from, to, config, &moved);
    if (NT_SUCCESS(recordStatus)) {
        return {STATUS_SUCCESS, STATUS_SUCCESS};
    }
    const NTSTATUS entryBack = MoveUpperEntryOnDisk(to, from, ReplaceExisting::No);
    const NTSTATUS recordsBack = MetadataStore::MoveSidecarRecordsBack(moved, config);
    if (NT_SUCCESS(entryBack)) {
        return {recordStatus, recordsBack};
    }
    if (!NT_SUCCESS(recordsBack)) {
        return {STATUS_SUCCESS, recordsBack};
    }
    // The move that leaves stuck records walks the whole tree again, and it
    // deletes the record at the new key of an entry with none at its old
    // key. So the records that moved go back first, and then move again.
    return {STATUS_SUCCESS,
            MetadataStore::MoveSidecarRecordsLeavingStuckOnes(from, to, config)};
}

UpperEntryMove MoveUpperEntryLeavingStuckRecords(const std::wstring& from,
                                                 const std::wstring& to,
                                                 ReplaceExisting replace,
                                                 const LayerConfig& config) {
    const NTSTATUS status = MoveUpperEntryOnDisk(from, to, replace);
    if (!NT_SUCCESS(status)) {
        return {status, STATUS_SUCCESS};
    }
    return {STATUS_SUCCESS, MetadataStore::MoveSidecarRecordsLeavingStuckOnes(from, to, config)};
}

UpperEntryMove MoveUpperEntryAside(const std::wstring& path,
                                   const std::function<std::wstring()>& newAsidePath,
                                   const LayerConfig& config,
                                   std::wstring* asidePath) {
    asidePath->clear();
    if (::GetFileAttributesW(WithExtendedPrefix(path).c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD probeErr = ::GetLastError();
        if (probeErr == ERROR_FILE_NOT_FOUND || probeErr == ERROR_PATH_NOT_FOUND) {
            return {STATUS_SUCCESS, STATUS_SUCCESS};
        }
        return {NtStatusFromWin32(probeErr), STATUS_SUCCESS};
    }
    std::wstring target = newAsidePath();
    const UpperEntryMove move = MoveUpperEntry(path, target, ReplaceExisting::No, config);
    if (NT_SUCCESS(move.status)) {
        *asidePath = std::move(target);
    }
    return move;
}

NTSTATUS ProbeUpperEntry(const std::wstring& path, bool* exists, EntryKind* kind) {
    *exists = false;
    const DWORD attrs = ::GetFileAttributesW(WithExtendedPrefix(path).c_str());
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

namespace {

constexpr DWORD kDeleteEntryAccess =
    DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
constexpr DWORD kListDirectoryAccess = FILE_LIST_DIRECTORY | SYNCHRONIZE;
constexpr DWORD kTreeDeleteAccess = kDeleteEntryAccess | kListDirectoryAccess;

// Replaces the DACL of the entry at path with a protected DACL that grants
// kTreeDeleteAccess to the user of the thread's token. The owner of an
// entry gets WRITE_DAC with no privilege, unless an OWNER RIGHTS ACE takes
// that right away. So the write works only on an entry whose owner is in
// the thread's token. It goes on the entry alone and does not pass to its
// children. Returns ERROR_SUCCESS, or the Win32 error of the step that
// failed.
DWORD GrantTreeDeleteAccess(const std::wstring& path) {
    const ScopedHandle entry = OpenReparseEntry(path, WRITE_DAC);
    if (!entry.IsValid()) {
        return ::GetLastError();
    }
    const HANDLE token = ::GetCurrentThreadEffectiveToken();
    DWORD userBytes = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &userBytes);
    std::vector<std::uint64_t> user((userBytes + sizeof(std::uint64_t) - 1) /
                                    sizeof(std::uint64_t));
    if (!::GetTokenInformation(token, TokenUser, user.data(), userBytes, &userBytes)) {
        return ::GetLastError();
    }
    const PSID userSid = reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
    const DWORD aclBytes = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) +
                           ::GetLengthSid(userSid);
    std::vector<std::uint64_t> acl((aclBytes + sizeof(std::uint64_t) - 1) /
                                   sizeof(std::uint64_t));
    const PACL dacl = reinterpret_cast<PACL>(acl.data());
    SECURITY_DESCRIPTOR descriptor{};
    if (!::InitializeAcl(dacl, aclBytes, ACL_REVISION) ||
        !::AddAccessAllowedAce(dacl, ACL_REVISION, kTreeDeleteAccess, userSid) ||
        !::InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !::SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE) ||
        !::SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED) ||
        !::SetKernelObjectSecurity(entry.Get(), DACL_SECURITY_INFORMATION, &descriptor)) {
        return ::GetLastError();
    }
    return ERROR_SUCCESS;
}

// Opens the entry at path, not the target of a link, with access, into
// *entry. When the entry's DACL refuses the open, writes the DACL of
// GrantTreeDeleteAccess and opens the entry again, so a thread without the
// backup and restore privileges can delete a tree whose copied DACLs
// refuse delete or listing. Returns ERROR_SUCCESS or the Win32 error of
// the open. When the grant fails, that error is the ERROR_ACCESS_DENIED of
// the first open.
DWORD OpenForTreeDelete(const std::wstring& path, DWORD access, ScopedHandle* entry) {
    *entry = OpenReparseEntry(path, access);
    if (entry->IsValid()) {
        return ERROR_SUCCESS;
    }
    const DWORD openError = ::GetLastError();
    if (openError != ERROR_ACCESS_DENIED || GrantTreeDeleteAccess(path) != ERROR_SUCCESS) {
        return openError;
    }
    *entry = OpenReparseEntry(path, access);
    return entry->IsValid() ? ERROR_SUCCESS : ::GetLastError();
}

// Lists the directory at path for DeleteTree and leaves out . and .. from
// *names. When the directory's DACL refuses the listing, the open rewrites
// that DACL as OpenForTreeDelete does. On a failure, *names holds the names
// read before it.
DWORD ListDirectoryForTreeDelete(const std::wstring& path, std::vector<std::wstring>* names) {
    ScopedHandle directory;
    const DWORD openError = OpenForTreeDelete(path, kListDirectoryAccess, &directory);
    if (openError != ERROR_SUCCESS) {
        return openError;
    }
    std::vector<std::uint64_t> buffer(64 * 1024 / sizeof(std::uint64_t));
    const DWORD bufferBytes = static_cast<DWORD>(buffer.size() * sizeof(std::uint64_t));
    while (::GetFileInformationByHandleEx(directory.Get(), FileFullDirectoryInfo,
                                          buffer.data(), bufferBytes)) {
        const BYTE* record = reinterpret_cast<const BYTE*>(buffer.data());
        for (;;) {
            const auto* info = reinterpret_cast<const FILE_FULL_DIR_INFO*>(record);
            const std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                names->push_back(name);
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            record += info->NextEntryOffset;
        }
    }
    const DWORD error = ::GetLastError();
    return error == ERROR_NO_MORE_FILES ? ERROR_SUCCESS : error;
}

// The POSIX delete takes the name away at once, so the parent is empty for
// its own delete even when another process still holds the entry open. A
// file system without it gets the plain delete.
bool MarkForDelete(HANDLE entry) {
    FILE_DISPOSITION_INFO_EX posix{};
    posix.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    if (::SetFileInformationByHandle(entry, FileDispositionInfoEx, &posix, sizeof(posix))) {
        return true;
    }
    FILE_DISPOSITION_INFO plain{};
    plain.DeleteFile = TRUE;
    return ::SetFileInformationByHandle(entry, FileDispositionInfo, &plain, sizeof(plain)) !=
           FALSE;
}

// Whether DeleteTree enters the entry: a directory that is not a reparse
// point, or a directory reparse point that is not a link, such as a cloud
// placeholder. Empty when the reparse tag read fails, with that error in
// GetLastError.
std::optional<bool> EntersEntry(HANDLE entry, DWORD attributes) {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return true;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!::GetFileInformationByHandleEx(entry, FileAttributeTagInfo, &tag, sizeof(tag))) {
        return std::nullopt;
    }
    return !IsReparseTagNameSurrogate(tag.ReparseTag);
}

// Deletes the entry at path and everything below it. A link is a leaf, so
// the delete never leaves the tree. An entry whose DACL refuses the delete
// or the listing gets the DACL that GrantTreeDeleteAccess writes. Appends
// the path of each deleted entry to *deleted. Stops at the first entry it
// cannot delete and returns that entry.
std::optional<StagedEntryDeleteFailure> DeleteTree(const std::wstring& path,
                                                   std::vector<std::wstring>* deleted) {
    ScopedHandle entry;
    const DWORD openError = OpenForTreeDelete(path, kDeleteEntryAccess, &entry);
    if (openError != ERROR_SUCCESS) {
        if (IsGone(openError)) {
            return std::nullopt;
        }
        return StagedEntryDeleteFailure{path, openError};
    }
    FILE_BASIC_INFO basic{};
    if (!::GetFileInformationByHandleEx(entry.Get(), FileBasicInfo, &basic, sizeof(basic))) {
        return StagedEntryDeleteFailure{path, ::GetLastError()};
    }
    const DWORD attributes = basic.FileAttributes;
    const std::optional<bool> enters = EntersEntry(entry.Get(), attributes);
    if (!enters.has_value()) {
        return StagedEntryDeleteFailure{path, ::GetLastError()};
    }
    if (*enters) {
        std::vector<std::wstring> names;
        const DWORD listError = ListDirectoryForTreeDelete(path, &names);
        if (listError != ERROR_SUCCESS) {
            return StagedEntryDeleteFailure{path, listError};
        }
        for (const std::wstring& name : names) {
            std::optional<StagedEntryDeleteFailure> failure =
                DeleteTree(JoinDirPath(path, name), deleted);
            if (failure.has_value()) {
                return failure;
            }
        }
    }
    // A delete refuses a read-only entry.
    if (!ClearReadOnly(entry.Get(), attributes) || !MarkForDelete(entry.Get())) {
        return StagedEntryDeleteFailure{path, ::GetLastError()};
    }
    deleted->push_back(path);
    return std::nullopt;
}

}

std::optional<StagedEntryDeleteFailure> RemoveStagedEntry(const std::wstring& path,
                                                          const LayerConfig& config) {
    std::vector<std::wstring> deleted;
    std::optional<StagedEntryDeleteFailure> failure = DeleteTree(path, &deleted);
    // A delete that stops at an entry it cannot delete has deleted the
    // entries before it, so their records go too.
    MetadataStore::RemoveSidecarRecordsOfGoneEntries(deleted, config);
    return failure;
}

NTSTATUS CreateDirectoryOrUseExisting(const std::wstring& path) {
    const std::wstring extendedPath = WithExtendedPrefix(path);
    if (::CreateDirectoryW(extendedPath.c_str(), nullptr)) {
        return STATUS_SUCCESS;
    }
    const DWORD createErr = ::GetLastError();
    if (createErr != ERROR_ALREADY_EXISTS) {
        return NtStatusFromWin32(createErr);
    }
    const DWORD existingAttrs = ::GetFileAttributesW(extendedPath.c_str());
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

LinkStop LinkStopOf(WalkStop stop) {
    switch (stop) {
    case WalkStop::Link:
        return LinkStop::Link;
    case WalkStop::Unreadable:
        return LinkStop::Unreadable;
    case WalkStop::None:
    case WalkStop::File:
        break;
    }
    return LinkStop::None;
}

bool EndsAtLinkOrUnreadable(WalkStop stop) {
    return LinkStopOf(stop) != LinkStop::None;
}

LinkOnPath FindLinkOnPath(const std::wstring& layerPath, const std::wstring& dirRelativePath) {
    const LayerWalk walk = WalkToFirstNonDirectory(layerPath, dirRelativePath);
    switch (LinkStopOf(walk.stop)) {
    case LinkStop::Link:
        return walk.component == dirRelativePath ? LinkOnPath::Self : LinkOnPath::Ancestor;
    case LinkStop::Unreadable:
        return LinkOnPath::Unreadable;
    case LinkStop::None:
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

namespace {

std::vector<LayerWalk> WalkLowers(const LayerConfig& config, const std::wstring& dirNorm) {
    std::vector<LayerWalk> walks;
    walks.reserve(config.lowerPaths.size());
    for (const std::wstring& lowerPath : config.lowerPaths) {
        walks.push_back(WalkToFirstNonDirectory(lowerPath, dirNorm));
    }
    return walks;
}

}

LinkInView FindLinkInView(const LayerConfig& config,
                          const WhiteoutManager& whiteoutMgr,
                          const std::wstring& dirNorm) {
    const LinkInView none{LinkStop::None, LayerSource::None, {}};
    const LayerWalk upperWalk = WalkToFirstNonDirectory(config.upperPath, dirNorm);
    if (EndsAtLinkOrUnreadable(upperWalk.stop)) {
        return LinkInView{LinkStopOf(upperWalk.stop), LayerSource::Upper, upperWalk.component};
    }
    if (upperWalk.stop == WalkStop::File) {
        return none;
    }
    const std::vector<LayerWalk> lowerWalks = WalkLowers(config, dirNorm);
    const bool anyLowerLink = std::any_of(lowerWalks.begin(), lowerWalks.end(),
        [](const LayerWalk& walk) { return EndsAtLinkOrUnreadable(walk.stop); });
    if (!anyLowerLink) {
        return none;
    }
    const LayerAncestry upper =
        LayerAncestryOf(config, LayerDirectory{whiteoutMgr, config.upperPath, dirNorm}, -1);
    if (upper.whitedOut || upper.opaque) {
        return none;
    }
    for (size_t i = 0; i < config.lowerPaths.size(); ++i) {
        const LayerAncestry lower = LayerAncestryOf(
            config, LayerDirectory{whiteoutMgr, config.lowerPaths[i], dirNorm}, static_cast<int>(i));
        if (lower.whitedOut || lower.linkUnderHigherEntry) {
            return none;
        }
        const LayerWalk& walk = lowerWalks[i];
        if (EndsAtLinkOrUnreadable(walk.stop)) {
            return LinkInView{LinkStopOf(walk.stop), LayerSource::Lower, walk.component};
        }
        if (lower.opaque || walk.stop == WalkStop::File) {
            return none;
        }
    }
    return none;
}

LinkInView FindLinkAbove(const LayerConfig& config,
                         const WhiteoutManager& whiteoutMgr,
                         const std::wstring& normalizedPath) {
    return FindLinkInView(config, whiteoutMgr, fs::path(normalizedPath).parent_path().wstring());
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
        status = MoveUpperEntry(stagedPath, upperPath, ReplaceExisting::No, config).status;
    }
    RemoveStagedEntry(containerPath, config);
    return status;
}

}
