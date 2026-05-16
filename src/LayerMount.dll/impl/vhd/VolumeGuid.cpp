#include "VolumeGuid.h"
#include "VHDLayerManager.h"
#include <virtdisk.h>
#include <winioctl.h>
#include <vector>

namespace LayerMount::VHD {

// ===========================================================================
// 4.6 — GetVolumeGuidForPhysicalDisk
// ===========================================================================
// Enumerates all volumes and matches by physical disk number.
// Translated from C# VhdServices.FindVHDVolumePath.

DWORD GetVolumeGuidForPhysicalDisk(const std::wstring& physicalDiskPath,
                                    std::wstring& outVolumeGuid) {
    outVolumeGuid.clear();

    // Parse disk number from \\.\PhysicalDriveN
    DWORD targetDiskNumber = static_cast<DWORD>(-1);
    auto pos = physicalDiskPath.find(L"PhysicalDrive");
    if (pos != std::wstring::npos) {
        auto numStr = physicalDiskPath.substr(pos + 13);
        targetDiskNumber = static_cast<DWORD>(std::wcstoul(numStr.c_str(), nullptr, 10));
    }
    if (targetDiskNumber == static_cast<DWORD>(-1)) {
        return ERROR_INVALID_PARAMETER;
    }

    // Enumerate all volumes
    WCHAR volumeName[MAX_PATH]{};
    HANDLE findHandle = ::FindFirstVolumeW(volumeName, MAX_PATH);
    if (findHandle == INVALID_HANDLE_VALUE) {
        return ::GetLastError();
    }

    DWORD result           = ERROR_NOT_FOUND;
    // Track the first concrete per-volume CreateFileW failure so the caller
    // gets a useful diagnostic when enumeration finishes with no match.
    // Previously these were silently skipped and every no-match outcome
    // surfaced as ERROR_NOT_FOUND even when the real cause was, e.g.,
    // ACCESS_DENIED from running without SeManageVolumePrivilege.
    DWORD firstOpenFailure = ERROR_SUCCESS;
    // Track a genuine FindNextVolumeW failure separately from
    // ERROR_NO_MORE_FILES. The enumeration handle can be invalidated by a
    // volume arrival/removal event mid-walk; collapsing that into
    // NOT_FOUND hid transient storage-stack errors from callers.
    DWORD findEnumError    = ERROR_SUCCESS;

    while (true) {
        std::wstring volPath(volumeName);

        // Strip trailing backslash for CreateFileW
        std::wstring volPathNoSlash = StripTrailingBackslash(volPath);

        HANDLE hVolume = ::CreateFileW(
            volPathNoSlash.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (hVolume == INVALID_HANDLE_VALUE) {
            DWORD err = ::GetLastError();
            if (firstOpenFailure == ERROR_SUCCESS && err != ERROR_SUCCESS) {
                firstOpenFailure = err;
            }
        } else {
            STORAGE_DEVICE_NUMBER sdn{};
            DWORD bytesReturned = 0;
            BOOL ok = ::DeviceIoControl(
                hVolume,
                IOCTL_STORAGE_GET_DEVICE_NUMBER,
                nullptr, 0,
                &sdn, sizeof(sdn),
                &bytesReturned, nullptr);

            ::CloseHandle(hVolume);

            if (ok && sdn.DeviceNumber == targetDiskNumber) {
                // Found it — return with trailing backslash
                outVolumeGuid = EnsureTrailingBackslash(volPath);
                result = ERROR_SUCCESS;
                break;
            }
        }

        if (!::FindNextVolumeW(findHandle, volumeName, MAX_PATH)) {
            DWORD findErr = ::GetLastError();
            // ERROR_NO_MORE_FILES is the normal end-of-enumeration signal;
            // any other error must not be hidden behind NOT_FOUND.
            if (findErr != ERROR_NO_MORE_FILES && findErr != ERROR_SUCCESS) {
                findEnumError = findErr;
            }
            break;
        }
    }

    ::FindVolumeClose(findHandle);

    if (result == ERROR_SUCCESS) {
        return ERROR_SUCCESS;
    }
    // Preference order on no-match: enumeration error (hard failure of
    // the walk) > first per-volume open failure (likely the real cause)
    // > NOT_FOUND (no volume with a matching physical disk number).
    if (findEnumError != ERROR_SUCCESS)    return findEnumError;
    if (firstOpenFailure != ERROR_SUCCESS) return firstOpenFailure;
    return ERROR_NOT_FOUND;
}

// ===========================================================================
// 4.6 — GetMountPointForGuid
// ===========================================================================

DWORD GetMountPointForGuid(const std::wstring& volumeGuid,
                            std::wstring& outMountPoint) {
    outMountPoint.clear();

    std::wstring guidWithSlash = EnsureTrailingBackslash(volumeGuid);

    // First call to get required buffer size
    DWORD charCount = 0;
    ::GetVolumePathNamesForVolumeNameW(guidWithSlash.c_str(),
                                       nullptr, 0, &charCount);

    if (charCount == 0) {
        return ::GetLastError();
    }

    // Second call to get the actual path names
    std::vector<wchar_t> buffer(charCount);
    if (!::GetVolumePathNamesForVolumeNameW(guidWithSlash.c_str(),
                                             buffer.data(),
                                             charCount, &charCount)) {
        return ::GetLastError();
    }

    // Parse multi-string: null-separated, double-null terminated.
    // Return the first non-empty entry.
    const wchar_t* p = buffer.data();
    while (*p) {
        outMountPoint = p;
        if (!outMountPoint.empty()) return ERROR_SUCCESS;
        p += wcslen(p) + 1;
    }

    return ERROR_NOT_FOUND;
}

// ===========================================================================
// 4.6 — GetVolumeGuidForVHD
// ===========================================================================

DWORD GetVolumeGuidForVHD(HANDLE vhdHandle, std::wstring& outVolumeGuid) {
    outVolumeGuid.clear();

    // Get the physical path from the VHD handle
    WCHAR physPath[MAX_PATH]{};
    ULONG physPathSize = sizeof(physPath);
    DWORD result = ::GetVirtualDiskPhysicalPath(vhdHandle, &physPathSize, physPath);
    if (result != ERROR_SUCCESS) return result;

    return GetVolumeGuidForPhysicalDisk(physPath, outVolumeGuid);
}

} // namespace LayerMount::VHD
