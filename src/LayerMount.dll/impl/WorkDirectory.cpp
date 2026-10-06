#include "WorkDirectory.h"
#include "ElevationUtil.h"
#include "EntryCopy.h"
#include "LayerPath.h"
#include "MetadataStore.h"
#include "NtStatusUtil.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace LayerMount {

namespace {

constexpr const wchar_t* kStagingAreaName = L"work";
constexpr const wchar_t* kWorkDirectoryLockName = L".layermount.lock";
constexpr const wchar_t* kUpperLockName = L".layermount.upper.lock";

// A directory in the two forms the layout rule compares: written is the path
// as ComparablePath gives it, and resolved is the final path in that form, or
// empty when the directory cannot be opened or its final path read.
struct LayoutPath {
    std::wstring written;
    std::wstring resolved;
};

LayoutPath WrittenPath(const std::wstring& path) {
    return {ComparablePath(path), {}};
}

LayoutPath WrittenAndResolvedPath(const std::wstring& path) {
    const std::wstring finalPath = FinalPathOfDirectory(path);
    return {ComparablePath(path),
            finalPath.empty() ? std::wstring() : ComparablePath(finalPath)};
}

using ReadLayoutPath = LayoutPath (*)(const std::wstring& path);

// The resolved paths when both directories have one, and the written paths
// otherwise.
std::pair<std::wstring, std::wstring> PathsToCompare(const LayoutPath& a, const LayoutPath& b) {
    if (!a.resolved.empty() && !b.resolved.empty()) {
        return {a.resolved, b.resolved};
    }
    return {a.written, b.written};
}

bool AreSameOrNested(const std::wstring& a, const std::wstring& b) {
    return a == b || IsInsideDirectory(a, b) || IsInsideDirectory(b, a);
}

bool OverlapsUpper(const LayoutPath& work, const LayoutPath& upper) {
    const auto [workPath, upperPath] = PathsToCompare(work, upper);
    return workPath != JoinDirPath(upperPath, kSidecarDirName) &&
           AreSameOrNested(workPath, upperPath);
}

bool OverlapsLower(const LayoutPath& work, const LayoutPath& lower) {
    const auto [workPath, lowerPath] = PathsToCompare(work, lower);
    return AreSameOrNested(workPath, lowerPath);
}

HRESULT CheckLayoutWith(const LayerConfig& config, ReadLayoutPath read, std::wstring& error) {
    const LayoutPath work = read(config.workDirPath);
    if (OverlapsUpper(work, read(config.upperPath))) {
        error = L"The work directory and the upper layer must not be the same directory or "
                L"contain one another: " + config.workDirPath;
        return E_INVALIDARG;
    }
    for (const std::wstring& lower : config.lowerPaths) {
        if (OverlapsLower(work, read(lower))) {
            error = L"The work directory and a lower layer must not be the same directory or "
                    L"contain one another: " + lower;
            return E_INVALIDARG;
        }
    }
    return S_OK;
}

// Leaves out . and .. from *names. On a failure, *names holds the names
// read before it.
DWORD ListDirectory(const std::wstring& path, std::vector<std::wstring>* names) {
    const ScopedHandle directory = OpenReparseEntry(path, FILE_LIST_DIRECTORY | SYNCHRONIZE);
    if (!directory.IsValid()) {
        return ::GetLastError();
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

struct DeleteFailure {
    std::wstring path;
    DWORD error = ERROR_SUCCESS;
};

// Deletes the entry at path and everything below it. A link or any other
// reparse point is a leaf, so the delete never leaves the tree. Appends the
// path of each deleted entry to *deleted. Stops at the first entry it
// cannot delete.
bool DeleteTree(const std::wstring& path, std::vector<std::wstring>* deleted,
                DeleteFailure* failure) {
    const ScopedHandle entry = OpenReparseEntry(
        path, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE);
    if (!entry.IsValid()) {
        const DWORD error = ::GetLastError();
        if (IsGone(error)) {
            return true;
        }
        *failure = {path, error};
        return false;
    }
    FILE_BASIC_INFO basic{};
    if (!::GetFileInformationByHandleEx(entry.Get(), FileBasicInfo, &basic, sizeof(basic))) {
        *failure = {path, ::GetLastError()};
        return false;
    }
    const DWORD attributes = basic.FileAttributes;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        std::vector<std::wstring> names;
        const DWORD listError = ListDirectory(path, &names);
        if (listError != ERROR_SUCCESS) {
            *failure = {path, listError};
            return false;
        }
        for (const std::wstring& name : names) {
            if (!DeleteTree(JoinDirPath(path, name), deleted, failure)) {
                return false;
            }
        }
    }
    // A delete refuses a read-only entry.
    if (!ClearReadOnly(entry.Get(), attributes) || !MarkForDelete(entry.Get())) {
        *failure = {path, ::GetLastError()};
        return false;
    }
    deleted->push_back(path);
    return true;
}

// Opens the lock file lockName in lockDirectory. directory and role name the
// directory that the lock holds, for the message in error.
HRESULT TakeLock(const std::wstring& lockDirectory, const wchar_t* lockName,
                 const std::wstring& directory, const wchar_t* role, ScopedHandle* lock,
                 std::wstring& error) {
    const std::wstring lockPath = JoinDirPath(lockDirectory, lockName);
    lock->Reset(::CreateFileW(lockPath.c_str(), GENERIC_READ | DELETE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    if (lock->IsValid()) {
        return S_OK;
    }
    const DWORD lockError = ::GetLastError();
    if (lockError == ERROR_SHARING_VIOLATION) {
        error = std::wstring(L"The ") + role + L" is in use by another overlay: " + directory;
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    error = std::wstring(L"Failed to lock the ") + role + L": " + directory;
    return HRESULT_FROM_WIN32(lockError);
}

// The lock goes in <upperPath>\.overlay, a name the merged view never shows.
// A transient overlay's work directory is that directory too, so the two
// lock files have different names.
HRESULT LockUpper(const std::wstring& upperPath, ScopedHandle* lock, std::wstring& error) {
    const std::wstring lockDirectory = JoinDirPath(upperPath, kSidecarDirName);
    ::CreateDirectoryW(lockDirectory.c_str(), nullptr);
    return TakeLock(lockDirectory, kUpperLockName, upperPath, L"upper layer", lock, error);
}

HRESULT ResetStagingArea(const LayerConfig& config, std::wstring& error) {
    const std::wstring staging = StagingAreaPath(config.workDirPath);
    std::vector<std::wstring> deleted;
    DeleteFailure failure;
    const bool cleaned = DeleteTree(staging, &deleted, &failure);
    // A delete that stops at an entry it cannot delete has deleted the
    // entries before it, so their records go before that failure returns.
    MetadataStore::RemoveSidecarRecordsOfGoneEntries(deleted, config);
    if (!cleaned) {
        error = L"Failed to delete a leftover entry from the work directory: " + failure.path;
        return HRESULT_FROM_WIN32(failure.error);
    }
    if (!::CreateDirectoryW(staging.c_str(), nullptr)) {
        const DWORD createError = ::GetLastError();
        error = L"Failed to create the staging area of the work directory: " + staging;
        return HRESULT_FROM_WIN32(createError);
    }
    return S_OK;
}

}

std::wstring StagingAreaPath(const std::wstring& workDirPath) {
    return JoinDirPath(workDirPath, kStagingAreaName);
}

WorkDirectory::WorkDirectory(ScopedHandle workDirectoryLock, ScopedHandle upperLock)
    : workDirectoryLock_(std::move(workDirectoryLock)), upperLock_(std::move(upperLock)) {}

HRESULT WorkDirectory::CheckLayout(const LayerConfig& config, std::wstring& error) {
    return CheckLayoutWith(config, WrittenPath, error);
}

HRESULT WorkDirectory::Open(const LayerConfig& config,
                            std::unique_ptr<WorkDirectory>* workDirectory,
                            std::wstring& error) {
    workDirectory->reset();
    const HRESULT layout = CheckLayoutWith(config, WrittenAndResolvedPath, error);
    if (FAILED(layout)) {
        return layout;
    }
    EnableFileSystemPrivileges();
    ScopedHandle workDirectoryLock;
    const HRESULT workDirectoryLocked =
        TakeLock(config.workDirPath, kWorkDirectoryLockName, config.workDirPath,
                 L"work directory", &workDirectoryLock, error);
    if (FAILED(workDirectoryLocked)) {
        return workDirectoryLocked;
    }
    ScopedHandle upperLock;
    const HRESULT upperLocked = LockUpper(config.upperPath, &upperLock, error);
    if (FAILED(upperLocked)) {
        return upperLocked;
    }
    const HRESULT reset = ResetStagingArea(config, error);
    if (FAILED(reset)) {
        return reset;
    }
    workDirectory->reset(new WorkDirectory(std::move(workDirectoryLock), std::move(upperLock)));
    return S_OK;
}

}
