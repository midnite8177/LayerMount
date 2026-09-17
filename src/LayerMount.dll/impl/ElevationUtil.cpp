#include "ElevationUtil.h"

namespace LayerMount {

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

}
