#include "SecurityPolicy.h"
#include "NtStatusUtil.h"

#include <sddl.h>

#include <cstring>
#include <string>

namespace LayerMount {

NTSTATUS SecurityPolicy::SyntheticSecurity(SECURITY_INFORMATION effective,
                                           bool isProbe,
                                           PSECURITY_DESCRIPTOR sd,
                                           SIZE_T sdBytes,
                                           SIZE_T* requiredBytes) {
    std::wstring worldSddl;
    if (effective & OWNER_SECURITY_INFORMATION) {
        worldSddl += L"O:WD";
    }
    if (effective & GROUP_SECURITY_INFORMATION) {
        worldSddl += L"G:WD";
    }
    if (effective & DACL_SECURITY_INFORMATION) {
        worldSddl += L"D:(A;;FR;;;WD)";
    }
    if (worldSddl.empty()) {
        if (requiredBytes != nullptr) {
            *requiredBytes = 0;
        }
        return STATUS_SUCCESS;
    }

    PSECURITY_DESCRIPTOR worldSd = nullptr;
    ULONG worldSize = 0;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            worldSddl.c_str(), SDDL_REVISION_1, &worldSd, &worldSize)) {
        return NtStatusFromWin32(::GetLastError());
    }
    if (requiredBytes != nullptr) {
        *requiredBytes = worldSize;
    }
    if (isProbe) {
        ::LocalFree(worldSd);
        return STATUS_SUCCESS;
    }
    if (sdBytes < worldSize) {
        ::LocalFree(worldSd);
        return STATUS_BUFFER_OVERFLOW;
    }
    std::memcpy(sd, worldSd, worldSize);
    ::LocalFree(worldSd);
    return STATUS_SUCCESS;
}

}
