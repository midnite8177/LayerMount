#pragma once

// ACL helpers shared by the native test projects. Each consumer adds this
// folder to its include path; the C# project one level up globs only *.cs,
// so the header is not part of it.

#include <windows.h>
#include <aclapi.h>

#include <CppUnitTest.h>

#include <string>

#pragma comment(lib, "advapi32.lib")

namespace LayerMountTestShared {

// Allocates the Everyone SID and frees it on destruction. sid stays null
// when the allocation fails.
struct EveryoneSid {
    PSID sid = nullptr;
    EveryoneSid() {
        SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
        ::AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID,
                                   0, 0, 0, 0, 0, 0, 0, &sid);
    }
    ~EveryoneSid() { if (sid) ::FreeSid(sid); }
};

// Merges a deny ACE for Everyone, carrying accessMask and the given
// inheritance flags, into the directory's existing DACL. The DACL stays
// unprotected, so the ACE propagates to existing children. Each step
// asserts on failure.
inline void AddInheritableDenyAce(const std::wstring& path,
                                  DWORD accessMask,
                                  DWORD inheritance) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

    EveryoneSid everyone;
    Assert::IsNotNull(everyone.sid, L"the Everyone SID allocates");

    PACL currentDacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    Assert::AreEqual<DWORD>(ERROR_SUCCESS,
        ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, &currentDacl, nullptr, &sd),
        L"GetNamedSecurityInfoW reads the directory's DACL");

    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = accessMask;
    ea.grfAccessMode        = DENY_ACCESS;
    ea.grfInheritance       = inheritance;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(everyone.sid);

    PACL newDacl = nullptr;
    Assert::AreEqual<DWORD>(ERROR_SUCCESS,
        ::SetEntriesInAclW(1, &ea, currentDacl, &newDacl),
        L"the deny ACE merges into the existing DACL");
    ::LocalFree(sd);

    DWORD setResult = ::SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, newDacl, nullptr);
    ::LocalFree(newDacl);
    Assert::AreEqual<DWORD>(ERROR_SUCCESS, setResult,
        L"the directory's DACL takes the deny ACE");
}

}
