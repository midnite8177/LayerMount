#pragma once

#include <windows.h>
#include <aclapi.h>

#include <CppUnitTest.h>

#include <memory>
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

struct WellKnownSid {
    explicit WellKnownSid(WELL_KNOWN_SID_TYPE type) {
        DWORD size = sizeof(buffer);
        Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(
            ::CreateWellKnownSid(type, nullptr, buffer, &size) != FALSE,
            L"The test must build the well-known SID");
    }
    PSID Get() const { return const_cast<BYTE*>(buffer); }
    BYTE buffer[SECURITY_MAX_SID_SIZE];
};

// One ACE for DaclWithOneMoreAce. The caller keeps sid alive for the call.
struct AceSpec {
    ACCESS_MODE mode;
    DWORD accessMask;
    DWORD inheritance;
    PSID sid;
};

using LocalAcl = std::unique_ptr<ACL, decltype(&::LocalFree)>;

// Returns the DACL of the entry at path with ace merged in. Asserts when the
// read or the merge fails.
inline LocalAcl DaclWithOneMoreAce(const std::wstring& path, const AceSpec& ace) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

    PACL current = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    Assert::AreEqual<DWORD>(ERROR_SUCCESS,
        ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, &current, nullptr, &sd),
        (L"The test must read the DACL of " + path).c_str());
    const std::unique_ptr<void, decltype(&::LocalFree)> sdOwner(sd, &::LocalFree);

    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = ace.accessMask;
    ea.grfAccessMode        = ace.mode;
    ea.grfInheritance       = ace.inheritance;
    ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName    = reinterpret_cast<LPWSTR>(ace.sid);

    PACL merged = nullptr;
    const DWORD mergeStatus = ::SetEntriesInAclW(1, &ea, current, &merged);
    LocalAcl owner(merged, &::LocalFree);
    Assert::AreEqual<DWORD>(ERROR_SUCCESS, mergeStatus,
        (L"The ACE must merge into the DACL of " + path).c_str());
    return owner;
}

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

    const LocalAcl newDacl =
        DaclWithOneMoreAce(path, AceSpec{DENY_ACCESS, accessMask, inheritance, everyone.sid});
    Assert::AreEqual<DWORD>(ERROR_SUCCESS,
        ::SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, newDacl.get(), nullptr),
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

// Creates a new file at path with backup intent and returns the error, or
// ERROR_SUCCESS after it deletes the file it made.
inline DWORD NewFileError(const std::wstring& path) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return ::GetLastError();
    ::CloseHandle(file);
    ::DeleteFileW(path.c_str());
    return ERROR_SUCCESS;
}

// Creates a new directory at path and returns the error, or ERROR_SUCCESS
// after it removes the directory it made.
inline DWORD NewDirectoryError(const std::wstring& path) {
    if (!::CreateDirectoryW(path.c_str(), nullptr)) return ::GetLastError();
    ::RemoveDirectoryW(path.c_str());
    return ERROR_SUCCESS;
}

// Calls visit(header, mask, sid) for each allow and deny ACE in the DACL of
// the entry at path, in DACL order, and skips the other ACE types. A
// reparse point gives its own DACL, not the DACL of its target. Asserts
// when the DACL or one of its ACEs cannot be read.
template <typename Visit>
void ForEachAllowOrDenyAce(const std::wstring& path, const Visit& visit) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    const HANDLE entry = ::CreateFileW(path.c_str(), READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Assert::IsTrue(entry != INVALID_HANDLE_VALUE, (L"The test must open " + path).c_str());
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD readError = ::GetSecurityInfo(entry, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                              nullptr, nullptr, &dacl, nullptr, &sd);
    ::CloseHandle(entry);
    Assert::AreEqual<DWORD>(ERROR_SUCCESS, readError,
        (L"The test must read the DACL of " + path).c_str());
    const std::unique_ptr<void, decltype(&::LocalFree)> sdOwner(sd, &::LocalFree);
    for (WORD i = 0; dacl != nullptr && i < dacl->AceCount; ++i) {
        ACE_HEADER* header = nullptr;
        Assert::IsTrue(::GetAce(dacl, i, reinterpret_cast<LPVOID*>(&header)) != FALSE,
            (L"The test must read each ACE in the DACL of " + path).c_str());
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE &&
            header->AceType != ACCESS_DENIED_ACE_TYPE) {
            continue;
        }
        auto* ace = reinterpret_cast<ACCESS_ALLOWED_ACE*>(header);
        visit(*header, ace->Mask, static_cast<PSID>(&ace->SidStart));
    }
}

// The number of allow and deny ACEs for target in the DACL of the entry at
// path.
inline size_t CountDaclAcesForSid(const std::wstring& path, PSID target) {
    size_t count = 0;
    ForEachAllowOrDenyAce(path, [&](const ACE_HEADER&, ACCESS_MASK, PSID sid) {
        if (::EqualSid(sid, target)) {
            ++count;
        }
    });
    return count;
}

// Merges an allow ACE for sid with FILE_READ_ATTRIBUTES, inherited by
// files and directories, into the DACL of the entry at path. The DACL stays
// unprotected. Asserts when the write fails.
inline void GrantInheritableReadAttributes(const std::wstring& path, PSID sid) {
    const LocalAcl merged = DaclWithOneMoreAce(
        path, AceSpec{GRANT_ACCESS, FILE_READ_ATTRIBUTES,
                      OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, sid});
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreEqual<DWORD>(ERROR_SUCCESS,
        ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, merged.get(), nullptr),
        L"The DACL of the entry takes the grant");
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
