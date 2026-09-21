// NtdllExport.h: loads one ntdll.dll export by name.
// The engine does not link ntdll.lib, so a caller resolves each export
// at run time. A missing export yields nullptr.
// Internal. Consumers of LayerMount.dll do not see it.

#pragma once

#include <windows.h>

namespace LayerMount {

template <typename Fn>
Fn LoadNtdllExport(const char* name) {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return nullptr;
#pragma warning(push)
#pragma warning(disable: 4191) // unsafe function pointer cast
    return reinterpret_cast<Fn>(::GetProcAddress(ntdll, name));
#pragma warning(pop)
}

} // namespace LayerMount
