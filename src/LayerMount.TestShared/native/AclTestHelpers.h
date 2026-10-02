#pragma once

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
// unprotected, so an ACE with inheritance flags propagates to existing
// children. Each step asserts on failure.
inline void AddDenyAce(const std::wstring& path,
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

// Denies accessMask to Everyone on one file or directory, without
// inheritance, and restores the original DACL on destruction. Declare it
// after the test's layer environment so the DACL comes back before the
// environment removes its tree.
class AccessDenied {
public:
    AccessDenied(const std::wstring& path, DWORD accessMask) : path_(path) {
        using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            ::GetNamedSecurityInfoW(path_.c_str(), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, &originalDacl_, nullptr,
                                    &originalSd_),
            L"GetNamedSecurityInfoW reads the original DACL");
        AddDenyAce(path_, accessMask, NO_INHERITANCE);
    }

    ~AccessDenied() {
        ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, originalDacl_, nullptr);
        ::LocalFree(originalSd_);
    }

    AccessDenied(const AccessDenied&) = delete;
    AccessDenied& operator=(const AccessDenied&) = delete;

private:
    std::wstring path_;
    PACL originalDacl_ = nullptr;
    PSECURITY_DESCRIPTOR originalSd_ = nullptr;
};

// Denies FILE_LIST_DIRECTORY to Everyone on one directory, as AccessDenied
// does.
class DirectoryListingDenied : public AccessDenied {
public:
    explicit DirectoryListingDenied(const std::wstring& path)
        : AccessDenied(path, FILE_LIST_DIRECTORY) {}
};

// Makes the calling thread impersonate a copy of the process token with
// SE_BACKUP_NAME disabled, and reverts on destruction. FindFirstFileW opens
// with backup intent, so a deny ACE does not stop a scan while SE_BACKUP_NAME
// is enabled.
class BackupPrivilegeDisabledOnThread {
public:
    BackupPrivilegeDisabledOnThread() {
        using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
        Assert::IsTrue(::ImpersonateSelf(SecurityImpersonation) != FALSE,
            L"ImpersonateSelf gives the thread a copy of the process token");
        if (!DisableBackupPrivilegeOnThread()) {
            ::RevertToSelf();
            Assert::Fail(L"SE_BACKUP_NAME is disabled on the thread's token");
        }
    }

    ~BackupPrivilegeDisabledOnThread() {
        ::RevertToSelf();
    }

    BackupPrivilegeDisabledOnThread(const BackupPrivilegeDisabledOnThread&) = delete;
    BackupPrivilegeDisabledOnThread& operator=(const BackupPrivilegeDisabledOnThread&) = delete;

private:
    static bool DisableBackupPrivilegeOnThread() {
        HANDLE token = nullptr;
        if (!::OpenThreadToken(::GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                               TRUE, &token)) {
            return false;
        }
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Attributes = 0;
        bool disabled = false;
        if (::LookupPrivilegeValueW(nullptr, SE_BACKUP_NAME, &privileges.Privileges[0].Luid)) {
            disabled = ::AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges),
                                               nullptr, nullptr) != FALSE;
        }
        ::CloseHandle(token);
        return disabled;
    }
};

inline DWORD FindFirstFileError(const std::wstring& searchPath) {
    WIN32_FIND_DATAW findData;
    HANDLE hFind = ::FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return ::GetLastError();
    ::FindClose(hFind);
    return ERROR_SUCCESS;
}

inline void AssertListingDenied(const std::wstring& dirPath) {
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreEqual<DWORD>(
        ERROR_ACCESS_DENIED, FindFirstFileError(dirPath + L"\\*"),
        L"The deny ACE must make the directory scan fail");
}


inline bool TryEnablePrivilege(LPCWSTR privilege) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool enabled = false;
    if (::LookupPrivilegeValueW(nullptr, privilege, &tp.Privileges[0].Luid)) {
        // AdjustTokenPrivileges returns TRUE with ERROR_NOT_ALL_ASSIGNED
        // when the token does not carry the privilege.
        enabled = ::AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) != FALSE
               && ::GetLastError() == ERROR_SUCCESS;
    }
    ::CloseHandle(token);
    return enabled;
}

}
