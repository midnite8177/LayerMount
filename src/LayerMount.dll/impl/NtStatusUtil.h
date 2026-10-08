// NtStatusUtil.h -- Internal Win32 error and NTSTATUS mapping helpers.
// The engine returns NTSTATUS from many paths; the C ABI shim translates
// NTSTATUS -> HRESULT before the boundary.
//
// Intentionally internal -- consumers of LayerMount.dll do not see it.

#pragma once

#include "WindowsNtStatus.h"

#include <system_error>

namespace LayerMount {

// Translate a Win32 error code to the equivalent NTSTATUS. Covers the
// set the engine relies on for its callback return values. Unknown
// codes fall back to STATUS_UNSUCCESSFUL.
NTSTATUS NtStatusFromWin32(DWORD win32Error) noexcept;

// The NTSTATUS for err, or for fallback when err is 0, so a failed call
// that leaves the last error at 0 never maps to success.
NTSTATUS StatusFromWin32Error(DWORD err, DWORD fallback) noexcept;

// StatusFromWin32Error for the last error of the call that just failed.
NTSTATUS StatusOfFailedCall(DWORD fallback) noexcept;

// Whether a Win32 error says that no entry is at the path: the entry is
// gone, or a directory on the path is.
inline bool IsGone(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// The Win32 error in ec. MSVC's std::filesystem reports Win32 errors in
// system_category, so an error from another category is ERROR_GEN_FAILURE.
inline DWORD Win32FromErrorCode(const std::error_code& ec) noexcept {
    return ec.category() == std::system_category() ? static_cast<DWORD>(ec.value())
                                                   : ERROR_GEN_FAILURE;
}

// The HRESULT the C ABI returns for an engine NTSTATUS: S_OK for success,
// HRESULT_FROM_NT for everything else. A failed fill stores its message
// under this value, so LayerMountGetLastErrorMessage finds it with the
// HRESULT the caller received.
inline HRESULT HresultFromNtStatus(NTSTATUS status) noexcept {
    if (status == STATUS_SUCCESS) return S_OK;
    return HRESULT_FROM_NT(status);
}

}
