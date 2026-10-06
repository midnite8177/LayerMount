#include "WorkDirectory.h"
#include "ElevationUtil.h"
#include "LayerPath.h"

#include <optional>
#include <utility>

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
    const std::optional<StagedEntryDeleteFailure> failure = RemoveStagedEntry(staging, config);
    if (failure.has_value()) {
        error = L"Failed to delete a leftover entry from the work directory: " + failure->path;
        return HRESULT_FROM_WIN32(failure->error);
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
