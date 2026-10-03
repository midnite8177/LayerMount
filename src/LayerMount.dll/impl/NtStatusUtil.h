// NtStatusUtil.h -- Internal Win32 -> NTSTATUS mapping helper.
// The engine returns NTSTATUS from many paths; the C ABI shim translates
// NTSTATUS -> HRESULT before the boundary.
//
// Intentionally internal -- consumers of LayerMount.dll do not see it.

#pragma once

#include "WindowsNtStatus.h"

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

// The HRESULT the C ABI returns for an engine NTSTATUS: S_OK for success,
// HRESULT_FROM_NT for everything else. A failed fill stores its message
// under this value, so LayerMountGetLastErrorMessage finds it with the
// HRESULT the caller received.
inline HRESULT HresultFromNtStatus(NTSTATUS status) noexcept {
    if (status == STATUS_SUCCESS) return S_OK;
    return HRESULT_FROM_NT(status);
}

}
