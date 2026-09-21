// WindowsNtStatus.h -- <windows.h> plus the full NTSTATUS constant set, in the
// one include order that lets them coexist.
//
// WIN32_NO_STATUS keeps <windows.h> from defining the basic STATUS_*
// macros, so <ntstatus.h> supplies the full set. winnt.h defines a few
// newer codes outside that guard (STATUS_ASSERTION_FAILURE,
// STATUS_ENCLAVE_VIOLATION, STATUS_INTERRUPTED, STATUS_THREAD_NOT_RUNNING,
// STATUS_ALREADY_REGISTERED, STATUS_SXS_EARLY_DEACTIVATION,
// STATUS_SXS_INVALID_DEACTIVATION), and they collide with ntstatus.h, so
// C4005 stays suppressed around that one include.
//
// Include this header before any header that includes <windows.h> on its
// own; once <windows.h> is in, its guard makes WIN32_NO_STATUS a no-op.

#pragma once

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#pragma warning(push)
#pragma warning(disable: 4005)
#include <ntstatus.h>
#pragma warning(pop)
