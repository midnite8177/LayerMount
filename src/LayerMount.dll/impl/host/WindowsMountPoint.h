// WindowsMountPoint.h -- Internal header for the Windows mount-point
// helpers exposed publicly via LayerMountPoint* in LayerMount.h.
//
// The implementation has zero host-adapter dependency: it is pure Win32
// directory plumbing that every Windows host adapter would otherwise
// re-implement. Living inside the DLL (alongside other host helpers like
// LayerMountHResultToNtStatus) keeps the surface in one place.
//
// NOT installed. Consumers of LayerMount.dll see only LayerMountPoint*
// in public/LayerMount.h.

#pragma once

#include <windows.h>

#include <string_view>

namespace LayerMount::impl::host {

// Mount-point preparation state. PrepareDirectory captures whether it
// created the directory; CaptureIdentity fills volume-serial + file-id
// so ReleaseIfSafe can verify the directory we created is still the one
// we'd be removing (guards against the user replacing the path while we
// were mounted).
//
// Mirrors the layout of public LM_MOUNT_POINT_PREP byte-for-byte so the
// ABI shim can copy fields directly without per-field translation.
struct MountPointPrep {
    bool        directoryCreatedByUs = false;
    ULONGLONG   volumeSerial         = 0;
    FILE_ID_128 fileId{};
};

bool IsDriveLetterMountPoint(std::wstring_view mp);

NTSTATUS ValidateAndPrepareDirectoryMountPoint(std::wstring_view mp,
                                                MountPointPrep* outPrep);

void CaptureMountPointIdentity(std::wstring_view mp, MountPointPrep* prep);

// Outcome of RemoveOwnedMountPointDirectoryIfSafe. Used by the ABI
// shim to report cleanup failures to managed callers instead of
// silently treating all exit paths as success.
enum class MountPointReleaseResult {
    Removed,     // directory was ours and was successfully removed
    NotOwned,    // we never created the directory -- nothing to do
    StillInUse,  // identity mismatch or directory not empty -- left alone
    Failed,      // remove attempt failed (outWin32Error carries the code)
};

MountPointReleaseResult RemoveOwnedMountPointDirectoryIfSafe(
    std::wstring_view mp, const MountPointPrep& prep, DWORD& outWin32Error);

} // namespace LayerMount::impl::host
