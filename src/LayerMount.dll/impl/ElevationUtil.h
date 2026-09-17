// ElevationUtil.h -- Internal check of process elevation.
// VSS snapshot operations and VHD layer operations both need an
// elevated, administrator process. This function is the one check
// that both managers use.
//
// This function is internal only. Code outside LayerMount.dll does
// not include this header.

#pragma once

#include <windows.h>

namespace LayerMount {

// Reports whether the current process runs elevated.
// Returns ERROR_SUCCESS if elevated, ERROR_PRIVILEGE_NOT_HELD (1314)
// if not, or the Win32 error from the token query if it fails.
DWORD CheckElevation() noexcept;

}
