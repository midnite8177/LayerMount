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

} // namespace LayerMount
