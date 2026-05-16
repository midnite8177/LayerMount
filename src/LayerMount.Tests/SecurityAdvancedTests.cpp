// Owner / SACL / inheritance ACL coverage. Addresses audit gap:
//   MetadataPreservationTests checks DACL preservation and SetSecurity via
//   the mount. Missing: owner propagation beyond DACL, SACL (audit) ACEs,
//   and inherited ACE behavior on directories and their children.
//
// Risk this covers:
//   - An owner-restricted lower file that copy-ups with a different owner
//     changes who has take-ownership / admin rights on the upper copy.
//   - SACL audit rules drop on copy-up, defeating compliance controls.
//   - Inherited deny-ACEs on a lower directory do not carry to its upper
//     shadow, so a renamed or copy-upped child silently gains access that
//     the original access control intended to block.

#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"

#include <aclapi.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

namespace {

// Capture owner SID as a string ("S-1-5-..."). Returns empty on failure.
std::wstring GetOwnerSidString(const std::wstring& path) {
    DWORD size = 0;
    ::GetFileSecurityW(path.c_str(), OWNER_SECURITY_INFORMATION,
                        nullptr, 0, &size);
    if (size == 0) return {};
    std::vector<BYTE> buf(size);
    auto sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(buf.data());
    if (!::GetFileSecurityW(path.c_str(), OWNER_SECURITY_INFORMATION,
                              sd, size, &size)) return {};
    PSID owner = nullptr;
    BOOL defaulted = FALSE;
    if (!::GetSecurityDescriptorOwner(sd, &owner, &defaulted)) return {};
    if (!owner) return {};
    LPWSTR sidStr = nullptr;
    if (!::ConvertSidToStringSidW(owner, &sidStr)) return {};
    std::wstring result = sidStr;
    ::LocalFree(sidStr);
    return result;
}

// Count ACEs in the DACL that EqualSid to the target.
size_t CountDaclAcesForSid(const std::wstring& path, PSID target) {
    DWORD size = 0;
    ::GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                        nullptr, 0, &size);
    if (size == 0) return 0;
    std::vector<BYTE> buf(size);
    auto sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(buf.data());
    if (!::GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                              sd, size, &size)) return 0;
    BOOL daclPresent = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    if (!::GetSecurityDescriptorDacl(sd, &daclPresent, &dacl, &defaulted) ||
        !daclPresent || !dacl) return 0;
    size_t count = 0;
    for (WORD i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER* hdr = nullptr;
        if (!::GetAce(dacl, i, reinterpret_cast<LPVOID*>(&hdr))) continue;
        PSID aceSid = nullptr;
        if (hdr->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            aceSid = &reinterpret_cast<ACCESS_ALLOWED_ACE*>(hdr)->SidStart;
        } else if (hdr->AceType == ACCESS_DENIED_ACE_TYPE) {
            aceSid = &reinterpret_cast<ACCESS_DENIED_ACE*>(hdr)->SidStart;
        } else {
            continue;
        }
        if (::EqualSid(aceSid, target)) ++count;
    }
    return count;
}

bool HasInheritedAceForSid(const std::wstring& path, PSID target) {
    DWORD size = 0;
    ::GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                        nullptr, 0, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    auto sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(buf.data());
    if (!::GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION,
                              sd, size, &size)) return false;
    BOOL daclPresent = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    if (!::GetSecurityDescriptorDacl(sd, &daclPresent, &dacl, &defaulted) ||
        !daclPresent || !dacl) return false;
    for (WORD i = 0; i < dacl->AceCount; ++i) {
        ACE_HEADER* hdr = nullptr;
        if (!::GetAce(dacl, i, reinterpret_cast<LPVOID*>(&hdr))) continue;
        if ((hdr->AceFlags & INHERITED_ACE) == 0) continue;
        PSID aceSid = nullptr;
        if (hdr->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            aceSid = &reinterpret_cast<ACCESS_ALLOWED_ACE*>(hdr)->SidStart;
        } else if (hdr->AceType == ACCESS_DENIED_ACE_TYPE) {
            aceSid = &reinterpret_cast<ACCESS_DENIED_ACE*>(hdr)->SidStart;
        } else {
            continue;
        }
        if (::EqualSid(aceSid, target)) return true;
    }
    return false;
}

// RAII wrapper to free an "Everyone" SID.
struct EveryoneSid {
    PSID sid = nullptr;
    EveryoneSid() {
        SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
        ::AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID,
                                     0, 0, 0, 0, 0, 0, 0, &sid);
    }
    ~EveryoneSid() { if (sid) ::FreeSid(sid); }
};

} // namespace

// ============================================================================
// AdvancedSecurityTests — owner + inheritance + SACL invariants.
// ============================================================================

TEST_CLASS(AdvancedSecurityTests) {
public:
    // ------------------------------------------------------------------------
    // Owner propagation: copy-up preserves the owner SID from lower.
    //
    // CopySecurityDescriptor currently includes OWNER_SECURITY_INFORMATION,
    // so this test should pass — it serves as regression coverage so a
    // future refactor can't silently drop owner copying.
    // ------------------------------------------------------------------------
    TEST_METHOD(CopyUp_PreservesOwnerSid) {
        LayerMountTests::TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"owned.txt", "o");

        const std::wstring lowerPath = env.Lower(0) + L"\\owned.txt";
        const std::wstring expected = GetOwnerSidString(lowerPath);
        Assert::IsFalse(expected.empty(), L"Precondition: lower has an owner SID");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"owned.txt")));

        const std::wstring got = GetOwnerSidString(env.Upper() + L"\\owned.txt");
        Assert::AreEqual(expected, got,
            L"Upper owner SID must match lower owner SID");
    }

    // ------------------------------------------------------------------------
    // Inherited DACL ACEs on a child file: a file created under a directory
    // with an inheritable ACE should carry that ACE (flagged INHERITED_ACE)
    // into the upper copy.
    //
    // Tests the scenario: lower/secured/ has an inheritable Everyone-DENY
    // ACE on it; lower/secured/child.txt inherits the deny. On copy-up of
    // the child, the upper file must still carry the inherited deny —
    // otherwise a previously-blocked subject gains unintended access.
    // ------------------------------------------------------------------------
    TEST_METHOD(CopyUp_ChildWithInheritedAce_PreservesInheritedAce) {
        // CopyUp's preservation of inherited ACEs relies on
        // FILE_FLAG_BACKUP_SEMANTICS opens (in CopyAlternateStream and
        // MetadataADS::WriteAdsOnly) bypassing the inherited DENY-WRITE
        // ACE when writing ADS / metadata to the upper file. That bypass
        // requires SE_BACKUP_NAME / SE_RESTORE_NAME, which are admin-only
        // privileges enabled by EnsureCopyUpPrivileges; on a standard
        // (non-elevated) token the privileges are not held and the post-
        // SetFileSecurityW writes return ACCESS_DENIED. The test is a
        // genuine assertion that copy-up under a restrictive parent DACL
        // preserves access controls — it just needs elevation to exercise
        // the engine's actual code path. Match the pattern used by other
        // elevation-gated tests in this file (see CopyUp_SaclAuditAce_*).
        UNIT_SKIP_IF_NOT_ADMIN();

        LayerMountTests::TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"secured");
        env.WriteFile(env.Lower(0), L"secured\\child.txt", "payload");

        EveryoneSid everyone;
        Assert::IsNotNull(everyone.sid,
            L"Precondition: allocate Everyone SID");

        // Place an inheritable deny-write ACE on the parent, let the child
        // inherit it automatically. SetNamedSecurityInfo will push the
        // inheritable ACE down to existing children when
        // PROTECTED_DACL_SECURITY_INFORMATION is not set.
        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = FILE_GENERIC_WRITE;
        ea.grfAccessMode = DENY_ACCESS;
        ea.grfInheritance =
            OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone.sid);

        PACL newDacl = nullptr;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            ::SetEntriesInAclW(1, &ea, nullptr, &newDacl));

        const std::wstring lowerDir = env.Lower(0) + L"\\secured";
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            ::SetNamedSecurityInfoW(const_cast<LPWSTR>(lowerDir.c_str()),
                                       SE_FILE_OBJECT,
                                       DACL_SECURITY_INFORMATION |
                                           UNPROTECTED_DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, newDacl, nullptr));
        ::LocalFree(newDacl);

        const std::wstring lowerChild = env.Lower(0) + L"\\secured\\child.txt";
        // Verify child actually inherited the deny (pre-copy-up baseline).
        if (!HasInheritedAceForSid(lowerChild, everyone.sid)) {
            Logger::WriteMessage(L"[SKIP] Lower child did not auto-inherit the "
                                 L"deny ACE — OS/filesystem doesn't support the "
                                 L"inheritance model required for this test");
            return;
        }

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"secured\\child.txt")));

        // Upper child must carry the inherited ACE OR an explicit equivalent.
        // CopySecurityDescriptor copies the DACL as-is including inherited
        // ACEs — but GetFileSecurityW returns inherited ACEs with the
        // INHERITED_ACE flag only when the dest itself also inherits. Since
        // the upper parent dir was newly-created (via CopyUp) it may or may
        // not carry the inheritable ACE. Either way, the effective deny
        // ACE for Everyone must be present on the child.
        const std::wstring upperChild = env.Upper() + L"\\secured\\child.txt";
        const size_t count = CountDaclAcesForSid(upperChild, everyone.sid);
        Assert::IsTrue(count > 0,
            L"Upper child must carry a deny-ACE for Everyone (inherited from "
            L"lower parent's ACL). Missing = access-control bypass on copy-up.");
    }

    // ------------------------------------------------------------------------
    // Directory rename from lower: inherited ACEs must carry through.
    //
    // When a lower directory is renamed through the mount, its children are
    // recursively copied to the new upper location. Inherited ACEs on those
    // children (from the old parent) must be preserved OR regenerated from
    // the new upper parent's inheritable ACEs — losing them silently weakens
    // access control on the renamed subtree.
    // ------------------------------------------------------------------------
    TEST_METHOD(DirectoryRenameFromLower_ChildRetainsInheritedAce) {
        LayerMountTests::TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"src-secured");
        env.WriteFile(env.Lower(0), L"src-secured\\kid.txt", "kid");

        EveryoneSid everyone;
        Assert::IsNotNull(everyone.sid);

        // Use an inheritable ALLOW ACE for Everyone granting a specific rare
        // bit (FILE_READ_ATTRIBUTES). A DENY ACE would block the test's own
        // copy-up create from succeeding — we want to test ACE propagation,
        // not access control on the test harness itself.
        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = FILE_READ_ATTRIBUTES;
        ea.grfAccessMode = GRANT_ACCESS;
        ea.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone.sid);

        PACL dacl = nullptr;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            ::SetEntriesInAclW(1, &ea, nullptr, &dacl));
        const std::wstring dirPath = env.Lower(0) + L"\\src-secured";
        ::SetNamedSecurityInfoW(const_cast<LPWSTR>(dirPath.c_str()),
                                  SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION |
                                      UNPROTECTED_DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, dacl, nullptr);
        ::LocalFree(dacl);

        const std::wstring lowerKid = env.Lower(0) + L"\\src-secured\\kid.txt";
        if (!HasInheritedAceForSid(lowerKid, everyone.sid)) {
            Logger::WriteMessage(L"[SKIP] Inherited ACE did not propagate on layer");
            return;
        }

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // Simulate a rename-from-lower that the mount path would trigger.
        Assert::IsTrue(NT_SUCCESS(cu.HandleDirectoryRename(
            L"src-secured", L"dst-secured", /*sourceIsInLower=*/true)));

        const std::wstring newKid = env.Upper() + L"\\dst-secured\\kid.txt";
        const size_t countOnChild = CountDaclAcesForSid(newKid, everyone.sid);
        Assert::IsTrue(countOnChild > 0,
            L"After directory rename from lower, child files must retain "
            L"(or re-inherit via auto-inheritance) the parent's inheritable "
            L"ACE. Missing = access-control state silently weakened by rename.");
    }

    // ------------------------------------------------------------------------
    // SACL / audit ACE propagation.
    //
    // CopySecurityDescriptor now requests SACL_SECURITY_INFORMATION when
    // SE_SECURITY_NAME is held. Audit ACEs must survive copy-up so that
    // compliance controls (file-access auditing) are preserved on first
    // modification.
    //
    // Setting a SACL requires SE_SECURITY_NAME privilege. If the test
    // environment doesn't grant it (standard user), we skip — the
    // LayerMount implementation itself also no-ops SACL in that case.
    // ------------------------------------------------------------------------
    TEST_METHOD(CopyUp_SaclAuditAce_PreservedOnCopyUp) {
        // Enable SE_SECURITY_NAME for this thread.
        HANDLE tok = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(),
                                  TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
            Logger::WriteMessage(L"[SKIP] Could not open process token");
            return;
        }
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        if (!::LookupPrivilegeValueW(nullptr, SE_SECURITY_NAME,
                                        &tp.Privileges[0].Luid)) {
            ::CloseHandle(tok);
            Logger::WriteMessage(L"[SKIP] LookupPrivilegeValue(SE_SECURITY_NAME) failed");
            return;
        }
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ::AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        const DWORD adjustErr = ::GetLastError();
        ::CloseHandle(tok);
        if (adjustErr == ERROR_NOT_ALL_ASSIGNED) {
            Logger::WriteMessage(L"[SKIP] SE_SECURITY_NAME not held by this user");
            return;
        }

        LayerMountTests::TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"audited.txt", "a");

        EveryoneSid everyone;
        Assert::IsNotNull(everyone.sid);

        // Build a SACL with a SYSTEM_AUDIT ACE for Everyone: any access
        // attempt gets logged.
        const DWORD sidLen = ::GetLengthSid(everyone.sid);
        const DWORD aclLen = sizeof(ACL) +
                             sizeof(SYSTEM_AUDIT_ACE) - sizeof(DWORD) + sidLen;
        std::vector<BYTE> aclBuf(aclLen, 0);
        PACL sacl = reinterpret_cast<PACL>(aclBuf.data());
        Assert::IsTrue(::InitializeAcl(sacl, aclLen, ACL_REVISION) != FALSE);
        Assert::IsTrue(::AddAuditAccessAceEx(sacl, ACL_REVISION, 0,
            FILE_ALL_ACCESS, everyone.sid, TRUE, TRUE) != FALSE);

        const std::wstring lowerPath = env.Lower(0) + L"\\audited.txt";
        DWORD rc = ::SetNamedSecurityInfoW(
            const_cast<LPWSTR>(lowerPath.c_str()), SE_FILE_OBJECT,
            SACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, sacl);
        if (rc != ERROR_SUCCESS) {
            wchar_t msg[128];
            swprintf_s(msg, L"[SKIP] SetNamedSecurityInfo(SACL) failed (%lu)", rc);
            Logger::WriteMessage(msg);
            return;
        }

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"audited.txt")));

        // Query SACL on upper file. Today this returns empty / no SACL
        // because CopySecurityDescriptor doesn't request SACL.
        const std::wstring upperPath = env.Upper() + L"\\audited.txt";
        DWORD size = 0;
        ::GetFileSecurityW(upperPath.c_str(), SACL_SECURITY_INFORMATION,
                            nullptr, 0, &size);
        bool upperHasSacl = false;
        if (size > 0) {
            std::vector<BYTE> buf(size);
            auto sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(buf.data());
            if (::GetFileSecurityW(upperPath.c_str(),
                                      SACL_SECURITY_INFORMATION,
                                      sd, size, &size)) {
                BOOL present = FALSE, defaulted = FALSE;
                PACL gotSacl = nullptr;
                if (::GetSecurityDescriptorSacl(sd, &present, &gotSacl, &defaulted)) {
                    upperHasSacl = (present && gotSacl && gotSacl->AceCount > 0);
                }
            }
        }

        Assert::IsTrue(upperHasSacl,
            L"SACL (audit ACE) must be preserved on copy-up. When this "
            L"regresses, compliance-relevant audit rules silently drop on "
            L"first modification.");
    }
};

} // namespace LayerMountTests
