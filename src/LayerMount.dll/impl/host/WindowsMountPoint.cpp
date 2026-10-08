#include "../WindowsNtStatus.h"
#include "../NtStatusUtil.h"
#include "WindowsMountPoint.h"

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

}

bool IsDriveLetterMountPoint(std::wstring_view mp) {
    if (mp.size() == 2 && iswalpha(mp[0]) && mp[1] == L':') return true;
    if (mp.size() == 3 && iswalpha(mp[0]) && mp[1] == L':' && mp[2] == L'\\') return true;
    return false;
}

NTSTATUS ValidateAndPrepareDirectoryMountPoint(const HostPath& mp,
                                                MountPointPrep* outPrep) {
    if (outPrep == nullptr) return STATUS_INVALID_PARAMETER;
    *outPrep = MountPointPrep{};

    if (mp.Text().empty()) return STATUS_INVALID_PARAMETER;

    const std::wstring path = mp.ForWin32();

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

    // The host adapter creates the leaf in its mount call, because some
    // filesystem hosts refuse to mount on a directory that already exists.
    return STATUS_SUCCESS;
}

void CaptureMountPointIdentity(const HostPath& mp, MountPointPrep* prep) {
    if (prep == nullptr || mp.Text().empty()) return;

    prep->volumeSerial = 0;
    std::memset(&prep->fileId, 0, sizeof(prep->fileId));

    const std::wstring path = mp.ForWin32();
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

DWORD RemoveOwnedMountPointDirectoryIfSafe(const HostPath& mp,
                                           const MountPointPrep& prep)
{
    if (!prep.directoryCreatedByUs || mp.Text().empty()) {
        return ERROR_SUCCESS;
    }

    const std::wstring path = mp.ForWin32();
    HANDLE h = ::CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return ::GetLastError();
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
        return okId ? bhfiErr : idErr;
    }

    if (idInfo.VolumeSerialNumber != prep.volumeSerial ||
        std::memcmp(&idInfo.FileId, &prep.fileId, sizeof(FILE_ID_128)) != 0) {
        return ERROR_SUCCESS;
    }
    if (!(bhfi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (bhfi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return ERROR_SUCCESS;
    }

    std::error_code ec;
    auto it = std::filesystem::directory_iterator(path, ec);
    if (ec) {
        return Win32FromErrorCode(ec);
    }
    if (it != std::filesystem::directory_iterator()) {
        return ERROR_SUCCESS;
    }

    if (!std::filesystem::remove(path, ec)) {
        return ec ? Win32FromErrorCode(ec) : ERROR_GEN_FAILURE;
    }
    return ERROR_SUCCESS;
}

}
