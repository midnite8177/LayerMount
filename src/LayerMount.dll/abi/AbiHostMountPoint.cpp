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

inline ::LayerMount::HostPath HostPathOf(PCWSTR p) {
    return ::LayerMount::HostPath(p == nullptr ? L"" : p);
}

}

extern "C" {

LM_API HRESULT LM_CALL LayerMountPointIsDriveLetter(PCWSTR mountPoint,
                                                         BOOL*  outIsDriveLetter)
{
    LM_ABI_ENTRY();
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
    LM_ABI_ENTRY();
    if (outPrep == nullptr) return E_POINTER;

    using namespace ::LayerMount::abi;
    LM_ABI_BEGIN();

    ::LayerMount::impl::host::MountPointPrep prep{};
    const NTSTATUS status =
        ::LayerMount::impl::host::ValidateAndPrepareDirectoryMountPoint(
            HostPathOf(mountPoint), &prep);

    ToPublic(prep, *outPrep);

    if (NT_SUCCESS(status)) return S_OK;
    return HRESULT_FROM_NT(status);

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountPointCaptureIdentity(
    PCWSTR                mountPoint,
    LM_MOUNT_POINT_PREP* prep)
{
    LM_ABI_ENTRY();
    if (prep == nullptr) return E_POINTER;

    using namespace ::LayerMount::abi;
    LM_ABI_BEGIN();

    ::LayerMount::impl::host::MountPointPrep internal{};
    ToInternal(*prep, internal);

    ::LayerMount::impl::host::CaptureMountPointIdentity(HostPathOf(mountPoint),
                                                        &internal);

    ToPublic(internal, *prep);
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountPointReleaseIfSafe(
    PCWSTR                      mountPoint,
    const LM_MOUNT_POINT_PREP* prep)
{
    LM_ABI_ENTRY();
    if (prep == nullptr) return E_POINTER;

    using namespace ::LayerMount::abi;
    LM_ABI_BEGIN();

    ::LayerMount::impl::host::MountPointPrep internal{};
    ToInternal(*prep, internal);

    const DWORD win32Err =
        ::LayerMount::impl::host::RemoveOwnedMountPointDirectoryIfSafe(
            HostPathOf(mountPoint), internal);
    return HRESULT_FROM_WIN32(win32Err);

    LM_ABI_END();
}

}
