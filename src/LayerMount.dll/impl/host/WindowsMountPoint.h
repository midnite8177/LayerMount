// Internal header for the mount point helpers that LayerMountPoint* in
// public/LayerMount.h exposes. Not installed.

#pragma once

#include <windows.h>

#include "../HostPath.h"

#include <string_view>

namespace LayerMount::impl::host {

struct MountPointPrep {
    bool        directoryCreatedByUs = false;
    ULONGLONG   volumeSerial         = 0;
    FILE_ID_128 fileId{};
};

bool IsDriveLetterMountPoint(std::wstring_view mp);

NTSTATUS ValidateAndPrepareDirectoryMountPoint(const HostPath& mp,
                                                MountPointPrep* outPrep);

void CaptureMountPointIdentity(const HostPath& mp, MountPointPrep* prep);

// Returns ERROR_SUCCESS when it removes the directory or leaves it in
// place, and the Win32 error when it cannot open, read or remove it.
DWORD RemoveOwnedMountPointDirectoryIfSafe(const HostPath& mp,
                                           const MountPointPrep& prep);

}
