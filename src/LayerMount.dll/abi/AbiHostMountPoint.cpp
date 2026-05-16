// AbiHostMountPoint.cpp -- C ABI shims for the Windows mount-point
// host helpers. The implementation lives in impl/host/; this file only
// translates between LM_MOUNT_POINT_PREP (POD, ABI-stable) and the
// internal MountPointPrep, and wraps the impl in the standard
// AbiGuard exception ladder.

#include "../public/LayerMount.h"
#include "AbiGuard.h"
#include "../impl/host/WindowsMountPoint.h"

#include <cstring>
#include <string_view>

namespace {

// LM_MOUNT_POINT_PREP <-> MountPointPrep field shuffle. Internal struct
// is std::-typed (bool); public struct is BOOL/UINT64/UINT8[16] for ABI
// stability. Shapes are aligned but not memcpy-compatible because of bool
// padding -- copy field-by-field.
inline void ToInternal(const LM_MOUNT_POINT_PREP& src,
                       LayerMount::impl::host::MountPointPrep& dst) {
    dst.directoryCreatedByUs = src.directoryCreatedByUs != FALSE;
    dst.volumeSerial         = static_cast<ULONGLONG>(src.volumeSerial);
    static_assert(sizeof(dst.fileId) == sizeof(src.fileId),
                  "FILE_ID_128 size mismatch with LM_MOUNT_POINT_PREP::fileId");
    std::memcpy(&dst.fileId, src.fileId, sizeof(dst.fileId));
}

inline void ToPublic(const LayerMount::impl::host::MountPointPrep& src,
                     LM_MOUNT_POINT_PREP& dst) {
    dst.directoryCreatedByUs = src.directoryCreatedByUs ? TRUE : FALSE;
    dst.volumeSerial         = static_cast<UINT64>(src.volumeSerial);
    static_assert(sizeof(dst.fileId) == sizeof(src.fileId),
                  "FILE_ID_128 size mismatch with LM_MOUNT_POINT_PREP::fileId");
    std::memcpy(dst.fileId, &src.fileId, sizeof(dst.fileId));
    std::memset(dst.reserved, 0, sizeof(dst.reserved));
}

inline std::wstring_view View(PCWSTR p) {
    return p == nullptr ? std::wstring_view{} : std::wstring_view{p};
}

} // namespace

extern "C" {

LM_API HRESULT LM_CALL LayerMountPointIsDriveLetter(PCWSTR mountPoint,
                                                         BOOL*  outIsDriveLetter)
{
    if (outIsDriveLetter == nullptr) return E_POINTER;

    using namespace ::LayerMount::abi;
    LM_ABI_BEGIN();

    *outIsDriveLetter =
        ::LayerMount::impl::host::IsDriveLetterMountPoint(View(mountPoint))
            ? TRUE : FALSE;
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountPointPrepareDirectory(
    PCWSTR                mountPoint,
    LM_MOUNT_POINT_PREP* outPrep)
{
    if (outPrep == nullptr) return E_POINTER;

    using namespace ::LayerMount::abi;
    LM_ABI_BEGIN();

    ::LayerMount::impl::host::MountPointPrep prep{};
    const NTSTATUS status =
        ::LayerMount::impl::host::ValidateAndPrepareDirectoryMountPoint(
            View(mountPoint), &prep);

    ToPublic(prep, *outPrep);

    if (NT_SUCCESS(status)) return S_OK;
    return HRESULT_FROM_NT(status);

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountPointCaptureIdentity(
    PCWSTR                mountPoint,
    LM_MOUNT_POINT_PREP* prep)
{
    if (prep == nullptr) return E_POINTER;

    using namespace ::LayerMount::abi;
    LM_ABI_BEGIN();

    ::LayerMount::impl::host::MountPointPrep internal{};
    ToInternal(*prep, internal);

    ::LayerMount::impl::host::CaptureMountPointIdentity(View(mountPoint),
                                                        &internal);

    ToPublic(internal, *prep);
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountPointReleaseIfSafe(
    PCWSTR                      mountPoint,
    const LM_MOUNT_POINT_PREP* prep)
{
    if (prep == nullptr) return E_POINTER;

    using namespace ::LayerMount::abi;
    LM_ABI_BEGIN();

    ::LayerMount::impl::host::MountPointPrep internal{};
    ToInternal(*prep, internal);

    // Distinguish real cleanup failures (surface as HRESULT_FROM_WIN32
    // so managed hosts can log / react) from the common no-op paths
    // (NotOwned, StillInUse). Previously every exit path returned
    // S_OK, so a stale / unremovable mount-point directory looked like
    // a successful release.
    DWORD win32Err = ERROR_SUCCESS;
    auto r = ::LayerMount::impl::host::RemoveOwnedMountPointDirectoryIfSafe(
        View(mountPoint), internal, win32Err);
    if (r == ::LayerMount::impl::host::MountPointReleaseResult::Failed) {
        return HRESULT_FROM_WIN32(
            win32Err != ERROR_SUCCESS ? win32Err : ERROR_GEN_FAILURE);
    }

    return S_OK;

    LM_ABI_END();
}

} // extern "C"
