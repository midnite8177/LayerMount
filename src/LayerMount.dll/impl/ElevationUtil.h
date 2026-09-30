// ElevationUtil.h -- Internal checks and settings on the process token.
// VSS snapshot operations and VHD layer operations both need an
// elevated, administrator process. Copy-up and the create paths need
// privileges enabled on the token to write through restrictive ACLs.
//
// These functions are internal only. Code outside LayerMount.dll does
// not include this header.

#pragma once

#include <windows.h>

namespace LayerMount {

// Reports whether the current process runs elevated.
// Returns ERROR_SUCCESS if elevated, ERROR_PRIVILEGE_NOT_HELD (1314)
// if not, or the Win32 error from the token query if it fails.
DWORD CheckElevation() noexcept;

// Enables SE_CREATE_SYMBOLIC_LINK_NAME, SE_RESTORE_NAME, SE_BACKUP_NAME
// and SE_SECURITY_NAME on the process token, once per process. A privilege
// that the token does not carry stays off, and the call does not report it.
//
//   - SE_CREATE_SYMBOLIC_LINK_NAME: FSCTL_SET_REPARSE_POINT needs it for
//     IO_REPARSE_TAG_SYMLINK. An elevated token carries it disabled.
//   - SE_RESTORE_NAME / SE_BACKUP_NAME: a FILE_FLAG_BACKUP_SEMANTICS open
//     then skips the DACL check, so the engine can write into a directory
//     whose DACL denies write.
//   - SE_SECURITY_NAME: reading or writing SACL_SECURITY_INFORMATION needs
//     it. Without it, a SACL does not survive copy-up.
void EnableFileSystemPrivileges();

// Whether SE_SECURITY_NAME is enabled on the process token. When false, a
// SACL read or write fails with ERROR_PRIVILEGE_NOT_HELD, so callers skip it.
// The first call tries to enable it; later calls are a cheap load.
bool IsSecurityPrivilegeHeld();

}
