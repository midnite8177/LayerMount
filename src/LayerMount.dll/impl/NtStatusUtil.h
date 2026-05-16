// NtStatusUtil.h -- Internal Win32 -> NTSTATUS mapping helper.
// The engine returns NTSTATUS from many paths; the C ABI shim translates
// NTSTATUS -> HRESULT before the boundary.
//
// Intentionally internal -- consumers of LayerMount.dll do not see it.

#pragma once

// See LayerMount.h for the WIN32_NO_STATUS / ntstatus.h / C4005 dance.
// This helper is included from translation units that don't include
// LayerMount.h directly, so we repeat the same include pattern here.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#pragma warning(push)
#pragma warning(disable: 4005)
#include <ntstatus.h>
#pragma warning(pop)

namespace LayerMount {

// Translate a Win32 error code to the equivalent NTSTATUS. Covers the
// set the engine relies on for its callback return values. Unknown
// codes fall back to STATUS_UNSUCCESSFUL.
NTSTATUS NtStatusFromWin32(DWORD win32Error) noexcept;

} // namespace LayerMount
