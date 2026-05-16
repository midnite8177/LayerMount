// WindowsMountPoint.cpp -- Internal implementation of the Windows
// mount-point helpers exposed via LayerMountPoint* in LayerMount.h.
//
// Lives inside the DLL so every host adapter shares one canonical
// directory-ownership implementation rather than duplicating it.

#define WIN32_NO_STATUS
#include "WindowsMountPoint.h"
#undef WIN32_NO_STATUS

#pragma warning(push)
#pragma warning(disable: 4005)   // STATUS_* macro redefinition between winnt.h and ntstatus.h
#include <ntstatus.h>
#pragma warning(pop)

#include <cwctype>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace LayerMount::impl::host {

namespace {

bool PathIsReparsePoint(const std::wstring& path) {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;
    }

    HANDLE h = ::CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    const bool isReparse =
        ::GetFileInformationByHandleEx(h, FileAttributeTagInfo,
            &tagInfo, sizeof(tagInfo)) != FALSE &&
        (tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    ::CloseHandle(h);
    return isReparse;
}

} // namespace

bool IsDriveLetterMountPoint(std::wstring_view mp) {
    if (mp.size() == 2 && iswalpha(mp[0]) && mp[1] == L':') return true;
    if (mp.size() == 3 && iswalpha(mp[0]) && mp[1] == L':' && mp[2] == L'\\') return true;
    return false;
}

NTSTATUS ValidateAndPrepareDirectoryMountPoint(std::wstring_view mp,
                                                MountPointPrep* outPrep) {
    if (outPrep == nullptr) return STATUS_INVALID_PARAMETER;
    *outPrep = MountPointPrep{};

    if (mp.empty()) return STATUS_INVALID_PARAMETER;

    const std::wstring path(mp);

    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (PathIsReparsePoint(path)) {
            return STATUS_IO_REPARSE_DATA_INVALID;
        }
        return STATUS_OBJECT_NAME_COLLISION;
    }

    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    // PrepareDirectory is validation-only: it does NOT create the leaf and
    // does NOT claim ownership. The engine's mount-point contract assumes
    // the host adapter creates the leaf itself as part of its mount call
    // (host adapters that fail on a pre-existing directory with
    // STATUS_OBJECT_NAME_COLLISION are pre-empted otherwise). Any eager
    // CreateDirectoryW here would therefore make such mounts impossible.
    //
    // Ownership (`directoryCreatedByUs`) is claimed later by
    // CaptureMountPointIdentity, which only runs after the host's mount
    // has succeeded -- meaning a fresh leaf was either created by the host
    // or already owned by the caller. That is the real commit point for
    // the reservation, and it closes the TOCTOU the previous
    // `directoryCreatedByUs = true` overclaim opened up: a successful
    // CaptureIdentity proves the mount raced no one, because the host
    // mount call would have failed if another process had created the leaf
    // in between.
    return STATUS_SUCCESS;
}

void CaptureMountPointIdentity(std::wstring_view mp, MountPointPrep* prep) {
    if (prep == nullptr || mp.empty()) return;

    prep->volumeSerial = 0;
    std::memset(&prep->fileId, 0, sizeof(prep->fileId));

    // Deliberately single-purpose: capture identity only, never claim
    // ownership. Ownership (`directoryCreatedByUs`) is a separate decision
    // the host adapter makes after it verifies its mount call succeeded --
    // mixing the two would break callers that use CaptureIdentity for
    // diagnostic or identity-only comparisons without intending to take
    // ownership.

    const std::wstring path(mp);
    HANDLE h = ::CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    FILE_ID_INFO idInfo{};
    if (::GetFileInformationByHandleEx(h, FileIdInfo, &idInfo, sizeof(idInfo))) {
        prep->volumeSerial = idInfo.VolumeSerialNumber;
        std::memcpy(&prep->fileId, &idInfo.FileId, sizeof(FILE_ID_128));
    }
    ::CloseHandle(h);
}

MountPointReleaseResult RemoveOwnedMountPointDirectoryIfSafe(
    std::wstring_view mp, const MountPointPrep& prep, DWORD& outWin32Error)
{
    outWin32Error = ERROR_SUCCESS;

    if (!prep.directoryCreatedByUs || mp.empty()) {
        return MountPointReleaseResult::NotOwned;
    }

    const std::wstring path(mp);
    HANDLE h = ::CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        outWin32Error = ::GetLastError();
        return MountPointReleaseResult::Failed;
    }

    FILE_ID_INFO idInfo{};
    BY_HANDLE_FILE_INFORMATION bhfi{};
    const BOOL okId   = ::GetFileInformationByHandleEx(h, FileIdInfo,
                                                        &idInfo, sizeof(idInfo));
    const DWORD idErr = okId ? ERROR_SUCCESS : ::GetLastError();
    const BOOL okBhfi = ::GetFileInformationByHandle(h, &bhfi);
    const DWORD bhfiErr = okBhfi ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(h);
    if (!okId || !okBhfi) {
        outWin32Error = okId ? bhfiErr : idErr;
        return MountPointReleaseResult::Failed;
    }

    if (idInfo.VolumeSerialNumber != prep.volumeSerial ||
        std::memcmp(&idInfo.FileId, &prep.fileId, sizeof(FILE_ID_128)) != 0) {
        return MountPointReleaseResult::StillInUse;
    }
    if (!(bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (bhfi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return MountPointReleaseResult::StillInUse;
    }

    std::error_code ec;
    auto it = std::filesystem::directory_iterator(path, ec);
    if (ec) {
        outWin32Error = static_cast<DWORD>(ec.value());
        return MountPointReleaseResult::Failed;
    }
    if (it != std::filesystem::directory_iterator()) {
        // Directory is non-empty -- another process left something in
        // it. Leave it alone; surface as StillInUse so the caller can
        // decide whether to escalate.
        return MountPointReleaseResult::StillInUse;
    }

    if (!std::filesystem::remove(path, ec)) {
        outWin32Error = static_cast<DWORD>(ec.value());
        return MountPointReleaseResult::Failed;
    }
    return MountPointReleaseResult::Removed;
}

} // namespace LayerMount::impl::host
