#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"

#include "AclTestHelpers.h"

#include <aclapi.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::AddDenyAce;
using LayerMountTestShared::EveryoneSid;

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

constexpr const wchar_t* kUpperRootHoldsEveryoneAce =
    L"[SKIP] The upper root already holds an ACE for Everyone. The upper Foo would "
    L"inherit it whether or not copy-up carried the lower ACL.";

// Grants and does not deny, because a deny ACE blocks the test's own copy-up create.
DWORD GrantEveryoneInheritableReadAttributes(const std::wstring& path, PSID everyone) {
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = FILE_READ_ATTRIBUTES;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone);

    PACL dacl = nullptr;
    const DWORD aclStatus = ::SetEntriesInAclW(1, &ea, nullptr, &dacl);
    if (aclStatus != ERROR_SUCCESS) {
        return aclStatus;
    }
    const DWORD setStatus = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),
                                                    SE_FILE_OBJECT,
                                                    DACL_SECURITY_INFORMATION |
                                                        UNPROTECTED_DACL_SECURITY_INFORMATION,
                                                    nullptr, nullptr, dacl, nullptr);
    ::LocalFree(dacl);
    return setStatus;
}

}

TEST_CLASS(AdvancedSecurityTests) {
public:
    // Owner propagation: copy-up preserves the owner SID from lower.
    //
    // CopySecurityDescriptor includes OWNER_SECURITY_INFORMATION.
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

    // Inherited DACL ACEs on a child file: a file created under a directory
    // with an inheritable ACE should carry that ACE (flagged INHERITED_ACE)
    // into the upper copy.
    //
    // Tests the scenario: lower/secured/ has an inheritable Everyone-DENY
    // ACE on it; lower/secured/child.txt inherits the deny. On copy-up of
    // the child, the upper file must still carry the inherited deny —
    // otherwise a previously-blocked subject gains unintended access.
    TEST_METHOD(CopyUp_ChildWithInheritedAce_PreservesInheritedAce) {
        // CopyUp's preservation of inherited ACEs relies on
        // FILE_FLAG_BACKUP_SEMANTICS opens (in CopyAlternateStream and
        // MetadataStore::WriteAdsOnly) bypassing the inherited DENY-WRITE
        // ACE when writing ADS / metadata to the upper file. That bypass
        // requires SE_BACKUP_NAME / SE_RESTORE_NAME, which are admin-only
        // privileges enabled by EnableFileSystemPrivileges; on a standard
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

        // The child inherits the deny-write ACE from the parent.
        const std::wstring lowerDir = env.Lower(0) + L"\\secured";
        AddDenyAce(lowerDir, FILE_GENERIC_WRITE,
                   OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);

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

    // Directory rename from lower: inherited ACEs must carry through.
    //
    // When a lower directory is renamed through the mount, its children are
    // recursively copied to the new upper location. Inherited ACEs on those
    // children (from the old parent) must be preserved OR regenerated from
    // the new upper parent's inheritable ACEs — losing them silently weakens
    // access control on the renamed subtree.
    TEST_METHOD(DirectoryRenameFromLower_ChildRetainsInheritedAce) {
        LayerMountTests::TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"src-secured");
        env.WriteFile(env.Lower(0), L"src-secured\\kid.txt", "kid");

        EveryoneSid everyone;
        Assert::IsNotNull(everyone.sid);

        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            GrantEveryoneInheritableReadAttributes(env.Lower(0) + L"\\src-secured", everyone.sid));

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

        Assert::IsTrue(NT_SUCCESS(cu.RenameLowerDirectory(
            L"src-secured", L"dst-secured", ReplaceExisting::No)));

        const std::wstring newKid = env.Upper() + L"\\dst-secured\\kid.txt";
        const size_t countOnChild = CountDaclAcesForSid(newKid, everyone.sid);
        Assert::IsTrue(countOnChild > 0,
            L"After directory rename from lower, child files must retain "
            L"(or re-inherit via auto-inheritance) the parent's inheritable "
            L"ACE. Missing = access-control state silently weakened by rename.");
    }

    TEST_METHOD(Create_UnderLowerDirectory_UpperDirectoryCarriesTheLowersAce) {
        LayerMountTests::TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo\\a.txt", "x");

        EveryoneSid everyone;
        Assert::IsNotNull(everyone.sid);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            GrantEveryoneInheritableReadAttributes(env.Lower(0) + L"\\Foo", everyone.sid));
        if (CountDaclAcesForSid(env.Upper(), everyone.sid) != 0) {
            Logger::WriteMessage(kUpperRootHoldsEveryoneAce);
            return;
        }

        ::LayerMount::LayerMount mount(env.MakeConfig());
        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"foo\\b.txt", kNoCreateOptions),
            L"A create under the lower directory must succeed");

        Assert::IsTrue(CountDaclAcesForSid(env.Upper() + L"\\Foo", everyone.sid) > 0,
            L"The upper Foo must carry the lower Foo's ACE for Everyone");
    }

    TEST_METHOD(Rename_LowerFileIntoLowerDirectory_UpperDirectoryCarriesTheLowersAce) {
        LayerMountTests::TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo\\a.txt", "x");
        env.WriteFile(env.Lower(0), L"b.txt", "y");

        EveryoneSid everyone;
        Assert::IsNotNull(everyone.sid);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            GrantEveryoneInheritableReadAttributes(env.Lower(0) + L"\\Foo", everyone.sid));
        if (CountDaclAcesForSid(env.Upper(), everyone.sid) != 0) {
            Logger::WriteMessage(kUpperRootHoldsEveryoneAce);
            return;
        }

        ::LayerMount::LayerMount mount(env.MakeConfig());
        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"b.txt", L"foo\\b.txt", kFailIfExists, kNoCallerPid),
            L"The rename of the lower file into the lower directory must succeed");

        Assert::IsTrue(CountDaclAcesForSid(env.Upper() + L"\\Foo", everyone.sid) > 0,
            L"The upper Foo must carry the lower Foo's ACE for Everyone");
    }

    TEST_METHOD(Rename_LowerDirectoryIntoLowerDirectory_UpperDirectoryCarriesTheLowersAce) {
        LayerMountTests::TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo\\a.txt", "x");
        env.WriteFile(env.Lower(0), L"x\\c.txt", "y");

        EveryoneSid everyone;
        Assert::IsNotNull(everyone.sid);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            GrantEveryoneInheritableReadAttributes(env.Lower(0) + L"\\Foo", everyone.sid));
        if (CountDaclAcesForSid(env.Upper(), everyone.sid) != 0) {
            Logger::WriteMessage(kUpperRootHoldsEveryoneAce);
            return;
        }

        ::LayerMount::LayerMount mount(env.MakeConfig());
        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"x", L"foo\\x", kFailIfExists, kNoCallerPid),
            L"The rename of the lower directory into the lower directory must succeed");

        Assert::IsTrue(CountDaclAcesForSid(env.Upper() + L"\\Foo", everyone.sid) > 0,
            L"The upper Foo must carry the lower Foo's ACE for Everyone");
    }

    // CopySecurityDescriptor requests SACL_SECURITY_INFORMATION when the
    // process holds SE_SECURITY_NAME, so audit ACEs survive copy-up.
    TEST_METHOD(CopyUp_SaclAuditAce_PreservedOnCopyUp) {
        if (!LayerMountTestShared::TryEnablePrivilege(SE_SECURITY_NAME)) {
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

}
