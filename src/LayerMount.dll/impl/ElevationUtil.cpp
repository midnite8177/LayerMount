#include "ElevationUtil.h"

#include <mutex>

namespace LayerMount {

namespace {

// AdjustTokenPrivileges returns TRUE for ERROR_NOT_ALL_ASSIGNED, when the
// token does not carry the privilege, so the result is in GetLastError.
bool EnablePrivilege(LPCWSTR privName) noexcept {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                            &token)) {
        return false;
    }
    LUID luid{};
    if (!::LookupPrivilegeValueW(nullptr, privName, &luid)) {
        ::CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL ok = ::AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp),
                                              nullptr, nullptr);
    const DWORD err = ::GetLastError();
    ::CloseHandle(token);
    return ok && err == ERROR_SUCCESS;
}

}

DWORD CheckElevation() noexcept
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return ::GetLastError();
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    BOOL ok = ::GetTokenInformation(token, TokenElevation,
                                    &elevation, sizeof(elevation), &size);
    DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(token);

    if (err != ERROR_SUCCESS) return err;

    return elevation.TokenIsElevated ? ERROR_SUCCESS : ERROR_PRIVILEGE_NOT_HELD;
}

void EnableFileSystemPrivileges() {
    static std::once_flag once;
    std::call_once(once, []() {
        EnablePrivilege(SE_CREATE_SYMBOLIC_LINK_NAME);
        EnablePrivilege(SE_RESTORE_NAME);
        EnablePrivilege(SE_BACKUP_NAME);
        (void)IsSecurityPrivilegeHeld();
    });
}

bool IsSecurityPrivilegeHeld() {
    static std::once_flag once;
    static bool held = false;
    std::call_once(once, []() { held = EnablePrivilege(SE_SECURITY_NAME); });
    return held;
}

}
