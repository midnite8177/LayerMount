#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"

#include "AclTestHelpers.h"

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>

#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::AccessDenied;
using LayerMountTestShared::AddDenyAce;
using LayerMountTestShared::EveryoneSid;
using LayerMountTestShared::ForEachAllowOrDenyAce;

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

size_t CountDaclAcesForSid(const std::wstring& path, PSID target) {
    size_t count = 0;
    ForEachAllowOrDenyAce(path, [&](const ACE_HEADER&, ACCESS_MASK, PSID sid) {
        if (::EqualSid(sid, target)) ++count;
    });
    return count;
}

bool HasInheritedAceForSid(const std::wstring& path, PSID target) {
    bool found = false;
    ForEachAllowOrDenyAce(path, [&](const ACE_HEADER& header, ACCESS_MASK, PSID sid) {
        if ((header.AceFlags & INHERITED_ACE) != 0 && ::EqualSid(sid, target)) found = true;
    });
    return found;
}

std::vector<BYTE> AceFlagsForSid(const std::wstring& path, PSID target) {
    std::vector<BYTE> flags;
    ForEachAllowOrDenyAce(path, [&](const ACE_HEADER& header, ACCESS_MASK, PSID sid) {
        if (::EqualSid(sid, target)) flags.push_back(header.AceFlags);
    });
    return flags;
}

struct WellKnownSid {
    explicit WellKnownSid(WELL_KNOWN_SID_TYPE type) {
        DWORD size = sizeof(buffer);
        Assert::IsTrue(::CreateWellKnownSid(type, nullptr, buffer, &size) != FALSE,
            L"The test must build the well-known SID");
    }
    PSID Get() const { return const_cast<BYTE*>(buffer); }
    BYTE buffer[SECURITY_MAX_SID_SIZE];
};

void GrantInheritableReadAttributes(const std::wstring& path, PSID sid) {
    PACL current = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    Assert::AreEqual<DWORD>(ERROR_SUCCESS,
        ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, &current, nullptr, &sd),
        L"The test must read the directory's DACL");
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = FILE_READ_ATTRIBUTES;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    PACL merged = nullptr;
    const DWORD mergeStatus = ::SetEntriesInAclW(1, &ea, current, &merged);
    ::LocalFree(sd);
    Assert::AreEqual<DWORD>(ERROR_SUCCESS, mergeStatus, L"The grant must merge into the DACL");
    const DWORD setStatus = ::SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, merged, nullptr);
    ::LocalFree(merged);
    Assert::AreEqual<DWORD>(ERROR_SUCCESS, setStatus, L"The directory's DACL must take the grant");
}

// upper\p holds an inherited ACE for parentSid, and the work directory an
// explicit inheritable ACE for workSid.
struct RenameInheritanceSetup {
    explicit RenameInheritanceSetup(const TempLayerEnvironment& env)
        : parentSid(WinBatchSid), workSid(WinDialupSid) {
        GrantInheritableReadAttributes(env.Upper(), parentSid.Get());
        env.CreateDir(env.Upper(), L"p");
        GrantInheritableReadAttributes(env.Work(), workSid.Get());
    }
    WellKnownSid parentSid;
    WellKnownSid workSid;
};

void AssertInheritsOnlyFromTheNewParent(const std::wstring& path,
                                        const RenameInheritanceSetup& setup) {
    const std::vector<BYTE> parentFlags = AceFlagsForSid(path, setup.parentSid.Get());
    Assert::IsTrue(std::any_of(parentFlags.begin(), parentFlags.end(),
                               [](BYTE f) { return (f & INHERITED_ACE) != 0; }),
        (path + L" inherits the ACE of its new upper parent").c_str());
    Assert::IsTrue(AceFlagsForSid(path, setup.workSid.Get()).empty(),
        (path + L" carries no ACE of the work directory").c_str());
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
        DirectoryRename dirRename(config, resolver, wm, cache, cu,
                                  ::LayerMount::abi::CapabilityGate(kDefaultHostCapabilities));

        Assert::IsTrue(NT_SUCCESS(dirRename.RenameLowerDirectory(
            CallerPath(L"src-secured"), CallerPath(L"dst-secured"),
            RenameEntryKind::Directory, ReplaceExisting::No)));

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

    TEST_METHOD(DirectoryRenameFromLower_TreeInheritsFromTheNewUpperParentOnly) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"tree\\sub\\x.txt", "x");
        const RenameInheritanceSetup setup(env);
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"p\\moved"),
            RenameEntryKind::Directory, ReplaceExisting::No),
            L"The rename of the lower directory succeeds");

        const std::wstring moved = env.Upper() + L"\\p\\moved";
        AssertInheritsOnlyFromTheNewParent(moved, setup);
        AssertInheritsOnlyFromTheNewParent(moved + L"\\sub", setup);
        AssertInheritsOnlyFromTheNewParent(moved + L"\\sub\\x.txt", setup);
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The rename leaves nothing in the work directory");
    }

    TEST_METHOD(DirectoryRenameFromLower_JunctionInheritsFromTheNewUpperParentOnly) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Lower(0) + L"\\link",
                                  env.Root() + L"\\target")) {
            return;
        }
        const RenameInheritanceSetup setup(env);
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"link"), CallerPath(L"p\\moved"),
            RenameEntryKind::Link, ReplaceExisting::No),
            L"The rename of the lower junction succeeds");

        AssertInheritsOnlyFromTheNewParent(env.Upper() + L"\\p\\moved", setup);
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The rename leaves nothing in the work directory");
    }

    TEST_METHOD(CopyUpDirectory_LowerJunctionInheritsFromItsUpperParentOnly) {
        TempLayerEnvironment env(1);
        if (!LowerJunctionCreatedOrSkipped(env, L"p")) {
            return;
        }
        const RenameInheritanceSetup setup(env);
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpDirectory(L"p\\link"),
            L"The copy-up of the lower junction succeeds");

        AssertInheritsOnlyFromTheNewParent(env.Upper() + L"\\p\\link", setup);
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The copy-up leaves nothing in the work directory");
    }

    TEST_METHOD(CaseOnlyRename_LowerJunctionInheritsFromItsUpperParentOnly) {
        TempLayerEnvironment env(1);
        if (!LowerJunctionCreatedOrSkipped(env, L"p")) {
            return;
        }
        const RenameInheritanceSetup setup(env);
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.RenameDirectoryCase(
            CallerPath(L"p\\link"), CallerPath(L"p\\LINK"), RenameEntryKind::Link),
            L"The case-only rename of the lower junction succeeds");

        AssertInheritsOnlyFromTheNewParent(env.Upper() + L"\\p\\LINK", setup);
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The rename leaves nothing in the work directory");
    }

    TEST_METHOD(CopyUpDirectory_LowerJunctionUnderAParentThatDeniesAdding_Succeeds) {
        TempLayerEnvironment env(1);
        if (!LowerJunctionCreatedOrSkipped(env, L"p")) {
            return;
        }
        env.CreateDir(env.Upper(), L"p");
        const AccessDenied denied(env.Upper() + L"\\p", FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpDirectory(L"p\\link"),
            L"The copy-up of the lower junction succeeds");

        Assert::IsTrue(HasAttribute(env.Upper() + L"\\p\\link", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper entry is a link");
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The copy-up leaves nothing in the work directory");
    }

    TEST_METHOD(DirectoryRenameFromLower_IntoAParentThatDeniesAdding_Succeeds) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"tree\\sub\\x.txt", "x");
        env.CreateDir(env.Upper(), L"p");
        const AccessDenied denied(env.Upper() + L"\\p", FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"p\\moved"),
            RenameEntryKind::Directory, ReplaceExisting::No),
            L"The rename of the lower tree succeeds");

        Assert::AreEqual(std::string("x"), env.ReadFile(env.Upper(), L"p\\moved\\sub\\x.txt"),
            L"The tree arrives at the new name");
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The rename leaves nothing in the work directory");
    }
};

}
