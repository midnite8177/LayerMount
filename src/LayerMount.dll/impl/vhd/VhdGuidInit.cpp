// VhdGuidInit.cpp — defines the Virtual Disk API GUID constants (e.g.,
// VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT) so they are linked into
// LayerMount.VHD.lib. Without this, consumers of the static lib must each
// #define INITGUID before including <virtdisk.h> (the CLI's main.cpp does;
// the integration-tests DLL didn't, producing LNK2001).
//
// Include order matters:
//   1. <windows.h> pulls in <winioctl.h> with INITGUID NOT set, so
//      winioctl.h's many DEFINE_GUID declarations remain `extern const`.
//   2. <initguid.h> sets INITGUID for subsequent DEFINE_GUID expansions.
//   3. <virtdisk.h>'s GUIDs are now defined in this TU.
// Reversing step 1 and 2 would double-define the winioctl.h GUIDs.

#include <windows.h>
#include <initguid.h>
#include <virtdisk.h>

// Reference a GUID so the linker cannot strip this TU's definitions.
extern "C" const GUID* VhdGuidInit_KeepAlive() {
    return &VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;
}
