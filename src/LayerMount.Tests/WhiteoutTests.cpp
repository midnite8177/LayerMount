#include "pch.h"
#include "TestFixture.h"

#include "WhiteoutManager.h"
#include "PathResolver.h"
#include "Cache.h"
#include "MetadataStore.h"
#include "AclTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::AccessDenied;
using LayerMountTestShared::BackupPrivilegeDisabledOnThread;
using LayerMountTestShared::DirectoryListingDenied;
using LayerMountTestShared::AssertListingDenied;

namespace LayerMountTests {

namespace {

struct ResolverUnderTest {
    explicit ResolverUnderTest(const TempLayerEnvironment& env)
        : ResolverUnderTest(env.MakeConfig()) {}

    explicit ResolverUnderTest(const ::LayerMount::LayerConfig& layerConfig)
        : config(layerConfig)
        , wm(config, &cache)
        , resolver(config, wm, cache) {}

    ::LayerMount::LayerConfig config;
    Cache cache;
    WhiteoutManager wm;
    PathResolver resolver;
};

DWORD AttributesProbeError(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES
        ? ::GetLastError()
        : ERROR_SUCCESS;
}

void AssertEveryListedEntryResolves(const TempLayerEnvironment& env,
                                    const std::wstring& dir,
                                    const std::map<std::wstring, MergedEntry>& merged) {
    ResolverUnderTest r(env);
    for (const auto& [key, entry] : merged) {
        const std::wstring child = dir + L"\\" + entry.findData.cFileName;
        Assert::IsTrue(r.resolver.ResolvePath(child).Found(),
            (L"Every listed entry must resolve: " + child).c_str());
    }
}

void AssertLinkHidesLowerChildren(const TempLayerEnvironment& env,
                                  LayerSource linkSource) {
    ResolverUnderTest r(env);

    Assert::IsFalse(r.resolver.ResolvePath(L"link\\fromlower.txt").Found(),
        L"The link must hide the child of the directory below it");
    Assert::IsFalse(r.resolver.ResolveLowerPath(L"link\\fromlower.txt").Found(),
        L"The link must hide the child of the directory below it from the lower lookup");
    Assert::IsFalse(r.resolver.ResolvePath(L"link\\sub\\fromlower.txt").Found(),
        L"The link must hide the grandchild of the directory below it");
    const ResolvedPath inTarget = r.resolver.ResolvePath(L"link\\fromtarget.txt");
    Assert::IsTrue(inTarget.Found(), L"The link target's own child must resolve through the link");
    Assert::IsTrue(inTarget.source == linkSource,
        L"The link target's own child must resolve in the layer that holds the link");
}

// Makes the upper link "link" to the directory target under the environment
// root. target and target\sub each hold an opaque marker file. Logs a skip
// and returns false when the link cannot be created.
bool UpperLinkToMarkedTargetCreatedOrSkipped(const TempLayerEnvironment& env,
                                             LinkCreator createLink) {
    env.WriteFile(env.Root(), OpaqueMarkerPath(L"target"), "");
    env.WriteFile(env.Root(), OpaqueMarkerPath(L"target\\sub"), "");
    return LinkCreatedOrSkipped(createLink, env.Upper() + L"\\link", env.Root() + L"\\target");
}

void AssertTargetMarkersKept(const TempLayerEnvironment& env) {
    Assert::IsTrue(env.FileExists(env.Root(), OpaqueMarkerPath(L"target")),
        L"The opaque marker file in the link target must stay");
    Assert::IsTrue(env.FileExists(env.Root(), OpaqueMarkerPath(L"target\\sub")),
        L"The opaque marker file in the subdirectory of the link target must stay");
}

void AssertLinkListsOnlyTargetEntries(const TempLayerEnvironment& env) {
    ::LayerMount::LayerMount mount(env.MakeConfig());

    const MergedDirectory atLink = mount.MergeDirectoryEntries(L"link");
    const MergedDirectory underLink = mount.MergeDirectoryEntries(L"link\\sub");

    AssertStatus(STATUS_SUCCESS, atLink.status, L"The merge at the link must succeed");
    Assert::AreEqual<size_t>(2, atLink.entries.size(),
        L"The listing of the link must hold only the link target's entries");
    Assert::IsTrue(atLink.entries.count(L"fromtarget.txt") == 1 && atLink.entries.count(L"sub") == 1,
        L"The listing of the link must hold the link target's entries");
    AssertEveryListedEntryResolves(env, L"link", atLink.entries);
    AssertStatus(STATUS_SUCCESS, underLink.status, L"The merge under the link must succeed");
    Assert::AreEqual<size_t>(1, underLink.entries.size(),
        L"The listing under the link must hold only the link target's entry");
    Assert::IsTrue(underLink.entries.count(L"fromtarget.txt") == 1,
        L"The listing under the link must hold the link target's entry");
}

void AssertHiddenFromListingAndLookup(const std::map<std::wstring, MergedEntry>& merged,
                                      const PathResolver& resolver,
                                      const std::wstring& listedName,
                                      const std::wstring& lookupPath,
                                      const std::wstring& hiddenBy) {
    Assert::IsTrue(merged.count(listedName) == 0,
        (hiddenBy + L" must hide " + listedName + L" from the listing").c_str());
    Assert::IsFalse(resolver.ResolvePath(lookupPath).Found(),
        (hiddenBy + L" must hide " + lookupPath + L" from the lookup by name").c_str());
}

}

static_assert(RefusesTemporaryConfig<WhiteoutManager, Cache*>,
    "WhiteoutManager keeps a reference to its LayerConfig");

TEST_CLASS(WhiteoutTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(IsWhiteoutName_WithPrefix_ReturnsTrue) {
        Assert::IsTrue(WhiteoutManager::IsWhiteoutName(L".wh.foo.txt"));
    }

    TEST_METHOD(IsWhiteoutName_NoPrefix_ReturnsFalse) {
        Assert::IsFalse(WhiteoutManager::IsWhiteoutName(L"foo.txt"));
    }

    TEST_METHOD(IsWhiteoutName_UpperCasePrefix_ReturnsTrue) {
        Assert::IsTrue(WhiteoutManager::IsWhiteoutName(L".WH.foo.txt"));
        Assert::IsTrue(WhiteoutManager::IsWhiteoutName(L".Wh.foo.txt"));
    }

    TEST_METHOD(IsWhiteoutName_PartialPrefix_ReturnsFalse) {
        Assert::IsFalse(WhiteoutManager::IsWhiteoutName(L".w.foo"));
    }

    TEST_METHOD(WhitedOutNameOfEntry_OpaqueMarkerInAnyCase_HidesNothing) {
        for (const wchar_t* marker : {L".wh..wh..opq", L".WH..WH..OPQ", L".Wh..wH..Opq"}) {
            Assert::IsFalse(WhiteoutManager::WhitedOutNameOfEntry(marker).has_value(),
                (std::wstring(L"The opaque marker ") + marker + L" must hide no name").c_str());
        }
    }

    TEST_METHOD(GetWhiteoutFileName_RootFile_ProducesDotWhFile) {
        std::wstring result = WhiteoutManager::GetWhiteoutFileName(L"foo.txt");
        Assert::AreEqual(std::wstring(L".wh.foo.txt"), result);
    }

    TEST_METHOD(GetWhiteoutFileName_NestedFile_ProducesParentSlashDotWhName) {
        std::wstring result = WhiteoutManager::GetWhiteoutFileName(L"sub\\foo.txt");
        Assert::AreEqual(std::wstring(L"sub\\.wh.foo.txt"), result);
    }

    TEST_METHOD(GetWhiteoutFullPath_JoinsLayerAndWhName) {
        std::wstring result = WhiteoutManager::GetWhiteoutFullPath(
            L"C:\\upper", L"sub\\foo.txt");
        Assert::AreEqual(std::wstring(L"C:\\upper\\sub\\.wh.foo.txt"), result);
    }

    TEST_METHOD(CreateWhiteout_File_ProducesHiddenSystemMarkerInUpper) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        AssertStatus(STATUS_SUCCESS, wm.CreateWhiteout(L"foo.txt", WhiteoutType::File),
                     L"CreateWhiteout must succeed");

        std::wstring whPath = env.Upper() + L"\\" + WhiteoutMarkerPath(L"foo.txt");
        DWORD attrs = ::GetFileAttributesW(whPath.c_str());
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES, attrs, L"Whiteout file should exist");
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_HIDDEN) != 0, L"Expected HIDDEN attribute");
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_SYSTEM) != 0, L"Expected SYSTEM attribute");
    }

    TEST_METHOD(CreateWhiteout_NestedPath_CreatesParentDir) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        AssertStatus(STATUS_SUCCESS, wm.CreateWhiteout(L"sub\\foo.txt", WhiteoutType::File),
                     L"CreateWhiteout must succeed");

        std::wstring parentDir = env.Upper() + L"\\sub";
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(parentDir.c_str()),
            L"Parent directory should be auto-created");

        std::wstring whPath = env.Upper() + L"\\" + WhiteoutMarkerPath(L"sub\\foo.txt");
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(whPath.c_str()),
            L"Nested whiteout should exist");
    }

    TEST_METHOD(HasWhiteout_AfterCreate_ReturnsTrue) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.CreateWhiteout(L"foo.txt", WhiteoutType::File);
        Assert::IsTrue(wm.HasWhiteout(L"foo.txt", env.Upper()));
    }

    TEST_METHOD(HasWhiteout_NoMarker_ReturnsFalse) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsFalse(wm.HasWhiteout(L"missing.txt", env.Upper()));
    }

    TEST_METHOD(HasWhiteoutInAnyLayer_MarkerInLower_ReturnsTrue) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        std::wstring lowerWh = env.Lower(0) + L"\\" + WhiteoutMarkerPath(L"foo.txt");
        HANDLE h = ::CreateFileW(lowerWh.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h);

        Assert::IsTrue(wm.HasWhiteoutInAnyLayer(L"foo.txt"));
    }

    TEST_METHOD(RemoveWhiteout_ExistingMarker_DeletesAndSucceeds) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.CreateWhiteout(L"foo.txt", WhiteoutType::File);
        Assert::IsTrue(wm.HasWhiteout(L"foo.txt", env.Upper()));

        AssertStatus(STATUS_SUCCESS, wm.RemoveWhiteout(L"foo.txt"),
            L"RemoveWhiteout must delete the marker");
        Assert::IsFalse(wm.HasWhiteout(L"foo.txt", env.Upper()));
    }

    TEST_METHOD(RemoveWhiteout_NoMarker_Succeeds) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        AssertStatus(STATUS_SUCCESS, wm.RemoveWhiteout(L"nonexistent.txt"),
            L"RemoveWhiteout should be idempotent when marker doesn't exist");
    }

    TEST_METHOD(CreateWhiteout_InvalidatesCache) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        ResolvedPath fake;
        fake.absolutePath = env.Lower(0) + L"\\foo.txt";
        fake.source = LayerSource::Lower;
        fake.lowerIndex = 0;
        cache.Put(L"foo.txt", fake);
        Assert::IsTrue(cache.Get(L"foo.txt").has_value(), L"Cache should have entry before whiteout");

        wm.CreateWhiteout(L"foo.txt", WhiteoutType::File);

        Assert::IsFalse(cache.Get(L"foo.txt").has_value(),
            L"Cache entry should be invalidated after CreateWhiteout");
    }

};

TEST_CLASS(WhiteoutPathResolverTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(PathResolve_WithWhiteoutInUpper_HidesLowerFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"secret.txt", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath before = resolver.ResolvePath(L"secret.txt");
        Assert::IsTrue(before.Found());
        Assert::IsTrue(before.source == LayerSource::Lower);

        wm.CreateWhiteout(L"secret.txt", WhiteoutType::File);

        ResolvedPath after = resolver.ResolvePath(L"secret.txt");
        Assert::IsFalse(after.Found(), L"Whiteout should hide the lower file");
        Assert::IsTrue(after.isWhiteout, L"Result should be flagged as whiteout");
    }

    TEST_METHOD(PathResolve_AfterRemoveWhiteout_LowerFileReExposed) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"secret.txt", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        wm.CreateWhiteout(L"secret.txt", WhiteoutType::File);
        Assert::IsFalse(resolver.ResolvePath(L"secret.txt").Found());

        wm.RemoveWhiteout(L"secret.txt");
        ResolvedPath result = resolver.ResolvePath(L"secret.txt");
        Assert::IsTrue(result.Found(), L"Lower file should be re-exposed");
        Assert::IsTrue(result.source == LayerSource::Lower);
    }

    TEST_METHOD(PathResolve_FileInUpperOverLowerDirectory_HidesLowerChildren) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\inner.txt", "lower");
        env.WriteFile(env.Upper(), L"d", "upper");

        ResolverUnderTest r(env);

        const ResolvedPath child = r.resolver.ResolvePath(L"d\\inner.txt");
        Assert::IsFalse(child.Found(), L"The upper file must hide the children of the lower directory");
        Assert::IsFalse(child.isWhiteout, L"A path under an upper file is plain not-found, not a whiteout");
        Assert::IsFalse(r.resolver.ResolveLowerPath(L"d\\inner.txt").Found(),
            L"The upper file must hide the children of the lower directory from the lower lookup");
        const ResolvedPath lowerDir = r.resolver.ResolveLowerPath(L"d");
        Assert::IsTrue(lowerDir.Found() && (lowerDir.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            L"The lower lookup of the path itself must still find the lower directory");
    }

    TEST_METHOD(ResolveLowerPath_UnderOpaqueUpperAncestor_FindsNoLowerChildOrGrandchild) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"p\\d\\x.txt", "lower");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"p"), "");

        ResolverUnderTest r(env);

        Assert::IsFalse(r.resolver.ResolveLowerPath(L"p\\d").Found(),
            L"The opaque upper p must hide the lower child p\\d from the lower lookup");
        Assert::IsFalse(r.resolver.ResolveLowerPath(L"p\\d\\x.txt").Found(),
            L"The opaque upper p must hide the lower grandchild p\\d\\x.txt from the lower lookup");
    }

    TEST_METHOD(ResolveLowerPath_UnderOpaqueUpperRoot_FindsNoLowerEntry) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"top.txt", "lower");
        env.WriteFile(env.Lower(0), L"d\\x.txt", "lower");
        env.WriteFile(env.Upper(), L".wh..wh..opq", "");

        ResolverUnderTest r(env);

        for (const wchar_t* hidden : {L"top.txt", L"d", L"d\\x.txt"}) {
            Assert::IsFalse(r.resolver.ResolveLowerPath(hidden).Found(),
                (std::wstring(L"The opaque upper root must hide ") + hidden +
                 L" from the lower lookup").c_str());
        }
    }

    TEST_METHOD(ResolveLowerPath_OpaqueMarkerOnThePathItself_StillFindsTheLowerDirectory) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"p\\d\\x.txt", "lower");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"p\\d"), "");

        ResolverUnderTest r(env);

        const ResolvedPath lowerDir = r.resolver.ResolveLowerPath(L"p\\d");
        Assert::IsTrue(lowerDir.Found() && (lowerDir.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            L"An opaque marker on p\\d itself must not hide the lower p\\d from the lower lookup");
        Assert::IsFalse(r.resolver.ResolveLowerPath(L"p\\d\\x.txt").Found(),
            L"The opaque upper p\\d must hide its lower child from the lower lookup");
    }

    TEST_METHOD(PathResolve_FileInLowerOverDeeperLowerDirectory_HidesDeeperChildren) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L"d", "lower0");
        env.WriteFile(env.Lower(1), L"d\\inner.txt", "lower1");

        ResolverUnderTest r(env);

        Assert::IsFalse(r.resolver.ResolvePath(L"d\\inner.txt").Found(),
            L"The file in the higher lower must hide the children of the deeper lower's directory");
        Assert::IsFalse(r.resolver.ResolveLowerPath(L"d\\inner.txt").Found(),
            L"The file in the higher lower must hide the children of the deeper lower's directory");
    }

    TEST_METHOD(PathResolve_DirectoryInUpperOverLowerFileOverDeeperDirectory_HidesDeeperChildren) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Upper(), L"d\\own.txt", "upper");
        env.WriteFile(env.Lower(0), L"d", "lower0");
        env.WriteFile(env.Lower(1), L"d\\inner.txt", "lower1");

        ResolverUnderTest r(env);

        Assert::IsTrue(r.resolver.ResolvePath(L"d\\own.txt").Found(),
            L"The upper directory's own child must resolve");
        Assert::IsFalse(r.resolver.ResolvePath(L"d\\inner.txt").Found(),
            L"The lower file must hide the children of the deeper lower's directory");
    }

    TEST_METHOD(PathResolve_JunctionInUpperOverLowerDirectory_HidesLowerChildren) {
        TempLayerEnvironment env(1);
        if (!BuildLinkOverDirectory(env, LayerSource::Upper, CreateDirectoryJunction)) {
            return;
        }

        AssertLinkHidesLowerChildren(env, LayerSource::Upper);
    }

    TEST_METHOD(PathResolve_DirectorySymlinkInUpperOverLowerDirectory_HidesLowerChildren) {
        TempLayerEnvironment env(1);
        if (!BuildLinkOverDirectory(env, LayerSource::Upper, CreateDirectorySymlink)) {
            return;
        }

        AssertLinkHidesLowerChildren(env, LayerSource::Upper);
    }

    TEST_METHOD(PathResolve_JunctionInLowerOverDeeperLowerDirectory_HidesDeeperChildren) {
        TempLayerEnvironment env(2);
        if (!BuildLinkOverDirectory(env, LayerSource::Lower, CreateDirectoryJunction)) {
            return;
        }

        AssertLinkHidesLowerChildren(env, LayerSource::Lower);
    }

    TEST_METHOD(PathResolve_DirectoryInUpperOverLowerJunction_HidesTargetAndDeeperLowerChildren) {
        TempLayerEnvironment env(2);
        if (!BuildLinkOverDirectory(env, LayerSource::Lower, CreateDirectoryJunction)) {
            return;
        }
        env.WriteFile(env.Upper(), L"link\\fromupper.txt", "upper");

        ResolverUnderTest r(env);

        Assert::IsTrue(r.resolver.ResolvePath(L"link\\fromupper.txt").Found(),
            L"The upper directory's own child must resolve");
        for (const wchar_t* hidden : {L"link\\fromtarget.txt", L"link\\sub\\fromtarget.txt",
                                      L"link\\fromlower.txt", L"link\\sub\\fromlower.txt"}) {
            Assert::IsFalse(r.resolver.ResolvePath(hidden).Found(),
                (std::wstring(L"A lower junction under an upper directory must hide ") + hidden).c_str());
            Assert::IsFalse(r.resolver.ResolveLowerPath(hidden).Found(),
                (std::wstring(L"A lower junction under an upper directory must hide ") + hidden +
                 L" from the lower lookup").c_str());
        }
    }

    TEST_METHOD(PathResolve_UpperJunctionWithUnreadableReparseTag_HidesLowerChild) {
        TempLayerEnvironment env(1);
        if (!BuildLinkOverDirectory(env, LayerSource::Upper, CreateDirectoryJunction)) {
            return;
        }
        const std::wstring junction = env.Upper() + L"\\link";
        LinkAccessDenied synchronizeDenied(junction, SYNCHRONIZE);
        ResolverUnderTest r(env);
        BackupPrivilegeDisabledOnThread noBackupPrivilege;
        DisableRestorePrivilegeOnThread();
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, AttributesProbeError(junction),
            L"The deny ACE must leave the junction's attributes readable");
        Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, LinkTagOpenError(junction),
            L"The deny ACE must make the junction's reparse tag unreadable");

        Assert::IsFalse(r.resolver.ResolvePath(L"link\\fromlower.txt").Found(),
            L"An upper component whose reparse tag the walk cannot read can be a link, "
            L"so it must hide the lower child");
    }

    TEST_METHOD(PathResolve_UpperAncestorUnreadable_HidesLowerChild) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"a");
        env.WriteFile(env.Lower(0), L"a\\b\\inner.txt", "lower");
        DirectoryListingDenied rootListingDenied(env.Upper());
        AccessDenied attributesDenied(env.Upper() + L"\\a", FILE_READ_ATTRIBUTES);
        ResolverUnderTest r(env);
        BackupPrivilegeDisabledOnThread noBackupPrivilege;
        Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, AttributesProbeError(env.Upper() + L"\\a"),
            L"The deny ACEs must make the upper ancestor's attributes unreadable");

        Assert::IsFalse(r.resolver.ResolvePath(L"a\\b\\inner.txt").Found(),
            L"An upper ancestor that the walk cannot read can be a file, so it must hide the lower child");
    }

};

TEST_CLASS(WhiteoutDirectoryEnumerationTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(MergeDirectoryEntries_LowerDirUnreadable_ReturnsTheScanFailure) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Upper(), L"sub\\up.txt", "upper");
        env.CreateDir(env.Lower(0), L"sub");
        env.WriteFile(env.Lower(1), L"sub\\below.txt", "lower1");
        DirectoryListingDenied denied(env.Lower(0) + L"\\sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        BackupPrivilegeDisabledOnThread noBackupPrivilege;
        AssertListingDenied(env.Lower(0) + L"\\sub");

        const MergedDirectory merged = mount.MergeDirectoryEntries(L"sub");

        AssertStatus(STATUS_ACCESS_DENIED, merged.status, L"A failed lower scan must report the scan's status");
        Assert::IsTrue(merged.entries.empty(),
            L"A failed lower scan must give no entries, not the upper's entries alone");
    }

    TEST_METHOD(MergeDirectoryEntries_UpperDirUnreadable_ReturnsTheScanFailure) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"sub\\up.txt", "upper");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"sub\\gone.txt"), "");
        env.WriteFile(env.Lower(0), L"sub\\gone.txt", "lower0");
        env.WriteFile(env.Lower(0), L"sub\\below.txt", "lower0");
        DirectoryListingDenied denied(env.Upper() + L"\\sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        BackupPrivilegeDisabledOnThread noBackupPrivilege;
        AssertListingDenied(env.Upper() + L"\\sub");

        const MergedDirectory merged = mount.MergeDirectoryEntries(L"sub");

        AssertStatus(STATUS_ACCESS_DENIED, merged.status, L"A failed upper scan must report the scan's status");
        Assert::IsTrue(merged.entries.empty(),
            L"An unreadable upper can hold whiteouts, so a failed upper scan must give no entries");
    }

    TEST_METHOD(MergeDirectoryEntries_FileInLowerAtListedDir_HidesDeeperLowersEntries) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L"sub", "lower0");
        env.WriteFile(env.Lower(1), L"sub\\below.txt", "lower1");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        const MergedDirectory merged = mount.MergeDirectoryEntries(L"sub");

        AssertStatus(STATUS_SUCCESS, merged.status, L"The merge must succeed");
        Assert::IsTrue(merged.entries.count(L"below.txt") == 0,
            L"A file in a lower where the listed directory should be must hide the deeper lower's entry");
    }

    TEST_METHOD(MergeDirectoryEntries_FileInUpperAtListedDirOrAncestor_ListsNoLowerEntries) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"d", "upper");
        env.WriteFile(env.Lower(0), L"d\\inner.txt", "lower");
        env.WriteFile(env.Lower(0), L"d\\sub\\deep.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        const MergedDirectory atDir = mount.MergeDirectoryEntries(L"d");
        const MergedDirectory underDir = mount.MergeDirectoryEntries(L"d\\sub");

        AssertStatus(STATUS_SUCCESS, atDir.status, L"The merge at the upper file must succeed");
        Assert::IsTrue(atDir.entries.empty(),
            L"An upper file at the listed directory must hide the lower directory's entries");
        AssertStatus(STATUS_SUCCESS, underDir.status, L"The merge under the upper file must succeed");
        Assert::IsTrue(underDir.entries.empty(),
            L"An upper file at an ancestor of the listed directory must hide the lower entries");
    }

    TEST_METHOD(MergeDirectoryEntries_UpperDirectoryWithWhiteoutAtItOrAncestor_ListsOnlyUpperEntries) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"d\\u.txt", "upper");
        env.WriteFile(env.Upper(), L"d\\sub\\v.txt", "upper");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"d"), "");
        env.WriteFile(env.Lower(0), L"d\\x.txt", "lower");
        env.WriteFile(env.Lower(0), L"d\\sub\\y.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        const MergedDirectory atDir = mount.MergeDirectoryEntries(L"d");
        const MergedDirectory underDir = mount.MergeDirectoryEntries(L"d\\sub");

        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\x.txt"),
            L"An open of the lower file under the whited-out upper directory must find nothing");
        AssertStatus(STATUS_SUCCESS, atDir.status, L"The merge at the upper directory must succeed");
        Assert::AreEqual(size_t{2}, atDir.entries.size(),
            L"The listing of the upper directory must hold only its upper entries");
        Assert::IsTrue(atDir.entries.count(L"u.txt") == 1 && atDir.entries.count(L"sub") == 1,
            L"The listing of the upper directory must hold u.txt and sub");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\sub\\y.txt"),
            L"An open of the lower file under the whited-out ancestor must find nothing");
        AssertStatus(STATUS_SUCCESS, underDir.status, L"The merge under the upper directory must succeed");
        Assert::AreEqual(size_t{1}, underDir.entries.size(),
            L"The listing under the whited-out ancestor must hold only the upper entry");
        Assert::IsTrue(underDir.entries.count(L"v.txt") == 1,
            L"The listing under the whited-out ancestor must hold v.txt");
    }

    TEST_METHOD(MergeDirectoryEntries_LowerWhiteoutAtListedDirOrAncestor_HidesDeeperLowersEntries) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L"d\\a.txt", "lower0");
        env.WriteFile(env.Lower(0), WhiteoutMarkerPath(L"d\\sub"), "");
        env.WriteFile(env.Lower(1), L"d\\sub\\x.txt", "lower1");
        env.WriteFile(env.Lower(1), L"d\\sub\\deep\\y.txt", "lower1");
        env.CreateDir(env.Upper(), L"d\\sub\\deep");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        const MergedDirectory atDir = mount.MergeDirectoryEntries(L"d\\sub");
        const MergedDirectory underDir = mount.MergeDirectoryEntries(L"d\\sub\\deep");

        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\sub\\x.txt"),
            L"An open of the deeper lower's file under the whiteout must find nothing");
        AssertStatus(STATUS_SUCCESS, atDir.status, L"The merge at the whited-out directory must succeed");
        Assert::AreEqual(size_t{1}, atDir.entries.size(),
            L"The listing of the whited-out directory must hold only the upper entry");
        Assert::IsTrue(atDir.entries.count(L"deep") == 1,
            L"The listing of the whited-out directory must hold the upper deep");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\sub\\deep\\y.txt"),
            L"An open of the deeper lower's file under the whited-out ancestor must find nothing");
        AssertStatus(STATUS_SUCCESS, underDir.status, L"The merge under the whiteout must succeed");
        Assert::IsTrue(underDir.entries.empty(),
            L"The listing under the whited-out ancestor must hold no lower entry");
    }

    TEST_METHOD(MergeDirectoryEntries_JunctionInUpperAtListedDirOrAncestor_ListsOnlyTargetEntries) {
        TempLayerEnvironment env(1);
        if (!BuildLinkOverDirectory(env, LayerSource::Upper, CreateDirectoryJunction)) {
            return;
        }

        AssertLinkListsOnlyTargetEntries(env);
    }

    TEST_METHOD(MergeDirectoryEntries_DirectorySymlinkInUpperAtListedDirOrAncestor_ListsOnlyTargetEntries) {
        TempLayerEnvironment env(1);
        if (!BuildLinkOverDirectory(env, LayerSource::Upper, CreateDirectorySymlink)) {
            return;
        }

        AssertLinkListsOnlyTargetEntries(env);
    }

    TEST_METHOD(MergeDirectoryEntries_JunctionInLowerAtListedDirOrAncestor_ListsNoDeeperEntries) {
        TempLayerEnvironment env(2);
        if (!BuildLinkOverDirectory(env, LayerSource::Lower, CreateDirectoryJunction)) {
            return;
        }

        AssertLinkListsOnlyTargetEntries(env);
    }

    TEST_METHOD(MergeDirectoryEntries_DirectoryInUpperOverLowerJunction_ListsOnlyUpperEntries) {
        TempLayerEnvironment env(2);
        if (!BuildLinkOverDirectory(env, LayerSource::Lower, CreateDirectoryJunction)) {
            return;
        }
        env.WriteFile(env.Upper(), L"link\\fromupper.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        const MergedDirectory atLink = mount.MergeDirectoryEntries(L"link");
        const MergedDirectory underLink = mount.MergeDirectoryEntries(L"link\\sub");

        AssertStatus(STATUS_SUCCESS, atLink.status, L"The merge at the upper directory must succeed");
        Assert::AreEqual<size_t>(1, atLink.entries.size(),
            L"A lower junction under an upper directory must add no entries to the listing");
        Assert::IsTrue(atLink.entries.count(L"fromupper.txt") == 1,
            L"The listing must hold the upper directory's entry");
        AssertStatus(STATUS_SUCCESS, underLink.status, L"The merge under the upper directory must succeed");
        Assert::IsTrue(underLink.entries.empty(),
            L"A lower junction at an ancestor that the upper holds must add no entries to the listing");
    }

    TEST_METHOD(MergeDirectoryEntries_LowerJunctionUnderUpperParentDirectory_ListsTargetEntries) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Upper(), L"p\\fromupper.txt", "upper");
        env.WriteFile(env.Root(), L"target\\fromtarget.txt", "target");
        env.WriteFile(env.Lower(1), L"p\\link\\fromlower.txt", "lower");
        env.CreateDir(env.Lower(0), L"p");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Lower(0) + L"\\p\\link",
                                  env.Root() + L"\\target")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        const MergedDirectory merged = mount.MergeDirectoryEntries(L"p\\link");

        AssertStatus(STATUS_SUCCESS, merged.status, L"The merge at the lower junction must succeed");
        Assert::AreEqual<size_t>(1, merged.entries.size(),
            L"The listing of the lower junction must hold only the junction target's entry");
        Assert::IsTrue(merged.entries.count(L"fromtarget.txt") == 1,
            L"A lower junction whose name no higher layer holds must list its target's entry");
        AssertEveryListedEntryResolves(env, L"p\\link", merged.entries);
    }

    TEST_METHOD(MergeDirectoryEntries_DirectoryInUpperOverLowerFileOverDeeperDirectory_ListsNoDeeperEntries) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Upper(), L"d\\own.txt", "upper");
        env.WriteFile(env.Lower(0), L"d", "lower0");
        env.WriteFile(env.Lower(1), L"d\\inner.txt", "lower1");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        const MergedDirectory merged = mount.MergeDirectoryEntries(L"d");

        AssertStatus(STATUS_SUCCESS, merged.status, L"The merge must succeed");
        Assert::IsTrue(merged.entries.count(L"own.txt") == 1, L"The merge must list the upper's entry");
        Assert::IsTrue(merged.entries.count(L"inner.txt") == 0,
            L"The lower file must hide the deeper lower directory's entries");
        AssertEveryListedEntryResolves(env, L"d", merged.entries);
    }

    TEST_METHOD(MergeDirectoryEntries_WhiteoutsInLower_HideTheirNamesInDeeperLower) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), WhiteoutMarkerPath(L"sub\\a.txt"), "");
        env.WriteFile(env.Lower(0), WhiteoutMarkerPath(L"sub\\b.txt"), "");
        env.WriteFile(env.Lower(1), L"sub\\a.txt", "lower1");
        env.WriteFile(env.Lower(1), L"sub\\b.txt", "lower1");
        env.WriteFile(env.Lower(1), L"sub\\c.txt", "lower1");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L"a.txt") == 0,
            L"The whiteout .wh.a.txt must hide a.txt in the deeper lower");
        Assert::IsTrue(merged.count(L"b.txt") == 0,
            L"The whiteout .wh.b.txt must hide b.txt in the deeper lower");
        Assert::IsTrue(merged.count(L"c.txt") == 1,
            L"The listing must show the deeper lower's entry that no whiteout hides");
        Assert::AreEqual(static_cast<size_t>(1), merged.size(),
            L"The listing must not show the whiteout markers");
    }

    TEST_METHOD(MergeDirectoryEntries_WhiteoutScannedAfterEntryInSameLower_HidesThatEntry) {
        TempLayerEnvironment env(1);
        // NTFS lists '!' (0x21) before '.' (0x2E), so the scan meets the
        // entry before its whiteout.
        env.WriteFile(env.Lower(0), L"sub\\!early.txt", "lower0");
        env.WriteFile(env.Lower(0), L"sub\\.wh.!early.txt", "");
        env.WriteFile(env.Lower(0), L"sub\\own.txt", "lower0");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L"!early.txt") == 0,
            L"A lower's whiteout must hide that lower's own entry");
        Assert::IsTrue(merged.count(L"own.txt") == 1,
            L"The listing must show the lower's entry that no whiteout hides");
    }

    TEST_METHOD(MergeDirectoryEntries_RootOfExtendedFormLowerPath_AppliesThatLowersWhiteouts) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), WhiteoutMarkerPath(L"a.txt"), "");
        env.WriteFile(env.Lower(0), L"own.txt", "lower0");
        env.WriteFile(env.Lower(1), L"a.txt", "lower1");
        env.WriteFile(env.Lower(1), L"deep.txt", "lower1");
        auto config = env.MakeConfig();
        config.lowerPaths[0] = HostPath(L"\\\\?\\" + env.Lower(0));
        ::LayerMount::LayerMount mount(config);

        auto merged = mount.MergeDirectoryEntries(L"").entries;

        Assert::IsTrue(merged.count(L"own.txt") == 1,
            L"The listing must show the entry at the root of the extended-form lower");
        Assert::IsTrue(merged.count(L"a.txt") == 0,
            L"The whiteout at the root of the extended-form lower must hide a.txt below it");
        Assert::IsTrue(merged.count(L"deep.txt") == 1,
            L"The listing must show the deeper lower's entry that no whiteout hides");
    }

    TEST_METHOD(MergeDirectoryEntries_ExtendedFormLowerPathWithTrailingSeparator_ListsRootAndSubdirectory) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"own.txt", "lower0");
        env.WriteFile(env.Lower(0), L"sub\\nested.txt", "lower0");
        auto config = env.MakeConfig();
        config.lowerPaths[0] = HostPath(ExtendedDirWithSeparator(env.Lower(0)));
        ::LayerMount::LayerMount mount(config);

        const MergedDirectory root = mount.MergeDirectoryEntries(L"");
        const MergedDirectory sub = mount.MergeDirectoryEntries(L"sub");

        AssertStatus(STATUS_SUCCESS, root.status,
            L"The root scan of a lower path that ends in a separator must succeed");
        Assert::IsTrue(root.entries.count(L"own.txt") == 1,
            L"The listing must show the entry at the root of the lower");
        AssertStatus(STATUS_SUCCESS, sub.status,
            L"The subdirectory scan of a lower path that ends in a separator must succeed");
        Assert::IsTrue(sub.entries.count(L"nested.txt") == 1,
            L"The listing must show the entry in the subdirectory of the lower");
    }

    TEST_METHOD(MergeDirectoryEntries_OpaqueMarkerAtRootOfExtendedFormLowerWithTrailingSeparator_HidesDeeperLowers) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L".wh..wh..opq", "");
        env.WriteFile(env.Lower(0), L"own.txt", "lower0");
        env.WriteFile(env.Lower(1), L"deep.txt", "lower1");
        auto config = env.MakeConfig();
        config.lowerPaths[0] = HostPath(ExtendedDirWithSeparator(env.Lower(0)));
        ::LayerMount::LayerMount mount(config);

        auto merged = mount.MergeDirectoryEntries(L"").entries;

        Assert::IsTrue(merged.count(L"own.txt") == 1,
            L"The listing must show the entry at the root of the opaque lower");
        Assert::IsTrue(merged.count(L"deep.txt") == 0,
            L"The opaque marker at the root of the lower must hide the deeper lower's entry");
    }
};

TEST_CLASS(OpaqueDirectoryTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(SetOpaque_WritesOpaqueADS) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        AssertStatus(STATUS_SUCCESS, wm.SetOpaque(L"sub"), L"SetOpaque must succeed");

        std::wstring dirPath = env.Upper() + L"\\sub";
        Assert::IsTrue(MetadataStore::HasOpaqueMetadata(dirPath, nullptr),
            L"SetOpaque should write :overlay.opaque ADS");
    }

    TEST_METHOD(SetOpaque_DirectoryUnderAnUpperFile_ReturnsTheMarkerWriteError) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"file", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND, wm.SetOpaque(L"file\\sub"),
            L"SetOpaque under a file must fail with the error of the marker write");
    }

    TEST_METHOD(SetOpaque_WritesDotWhOpqMarker) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.SetOpaque(L"sub");

        std::wstring marker = env.Upper() + L"\\" + OpaqueMarkerPath(L"sub");
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(marker.c_str()),
            L"SetOpaque should create .wh..wh..opq sentinel file");
    }

    TEST_METHOD(IsOpaque_ADSOnly_ReturnsTrue) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");

        std::wstring dirPath = env.Upper() + L"\\sub";
        Assert::IsTrue(MetadataStore::SetOpaqueMetadata(dirPath, nullptr));

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsTrue(wm.IsOpaque(L"sub"));
    }

    TEST_METHOD(IsOpaque_MarkerFileOnly_ReturnsTrue) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");

        std::wstring marker = env.Upper() + L"\\" + OpaqueMarkerPath(L"sub");
        HANDLE h = ::CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsTrue(wm.IsOpaque(L"sub"));
    }

    TEST_METHOD(IsOpaque_Neither_ReturnsFalse) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsFalse(wm.IsOpaque(L"sub"));
    }

    TEST_METHOD(IsOpaqueInLayer_ChecksSpecificLayerOnly) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        env.CreateDir(env.Lower(0), L"sub");

        std::wstring upperDir = env.Upper() + L"\\sub";
        MetadataStore::SetOpaqueMetadata(upperDir, nullptr);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsTrue(wm.IsOpaqueInLayer(L"sub", env.Upper()));
        Assert::IsFalse(wm.IsOpaqueInLayer(L"sub", env.Lower(0)));
    }

    TEST_METHOD(RemoveOpaque_ClearsBothMarkers) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.SetOpaque(L"sub");
        Assert::IsTrue(wm.IsOpaque(L"sub"));

        Assert::IsTrue(wm.RemoveOpaque(L"sub"));

        std::wstring dirPath = env.Upper() + L"\\sub";
        Assert::IsFalse(MetadataStore::HasOpaqueMetadata(dirPath, nullptr), L"the :overlay.opaque stream is gone");

        std::wstring marker = env.Upper() + L"\\" + OpaqueMarkerPath(L"sub");
        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(marker.c_str()),
            L"Sentinel file should be removed");
    }

    TEST_METHOD(HasOpaqueSelfOrAncestorInLayer_DirOpaqueInLayer_ReturnsTrue) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"d"), "");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsTrue(wm.HasOpaqueSelfOrAncestorInLayer(L"d", env.Lower(0)));
    }

    TEST_METHOD(HasOpaqueSelfOrAncestorInLayer_AncestorOpaqueInLayer_ReturnsTrue) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"a"), "");
        env.CreateDir(env.Lower(0), L"a\\b\\c");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsTrue(wm.HasOpaqueSelfOrAncestorInLayer(L"a\\b\\c", env.Lower(0)));
    }

    TEST_METHOD(HasOpaqueSelfOrAncestorInLayer_OpaqueOnlyInOtherLayer_ReturnsFalse) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"a"), "");
        env.CreateDir(env.Lower(0), L"a\\b");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsFalse(wm.HasOpaqueSelfOrAncestorInLayer(L"a\\b", env.Lower(0)));
        Assert::IsTrue(wm.HasOpaqueSelfOrAncestorInLayer(L"a\\b", env.Upper()));
    }

    TEST_METHOD(HasOpaqueSelfOrAncestorInLayer_NoMarker_ReturnsFalse) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"a\\b");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsFalse(wm.HasOpaqueSelfOrAncestorInLayer(L"a\\b", env.Lower(0)));
    }

    TEST_METHOD(IsOpaque_UpperLinkToMarkedTarget_ReturnsFalse) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            if (!UpperLinkToMarkedTargetCreatedOrSkipped(env, createLink)) {
                continue;
            }
            Assert::IsTrue(MetadataStore::SetOpaqueMetadata(env.Upper() + L"\\link", nullptr),
                L"Preconditions: the link must hold its own opaque stream");
            auto config = env.MakeConfig();
            Cache cache;
            WhiteoutManager wm(config, &cache);

            Assert::IsFalse(wm.IsOpaque(L"link"),
                L"A link must never be opaque");
            Assert::IsFalse(wm.IsOpaqueInLayer(L"link", env.Upper()),
                L"A link must never be opaque in its layer");
            Assert::IsFalse(wm.HasOpaqueSelfOrAncestorInLayer(L"link", env.Upper()),
                L"A link must not count as an opaque directory");
        }
    }

    TEST_METHOD(IsOpaqueInLayer_DirectoryUnderUpperLink_IgnoresTheMarkerInTheTarget) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            if (!UpperLinkToMarkedTargetCreatedOrSkipped(env, createLink)) {
                continue;
            }
            Assert::IsTrue(MetadataStore::SetOpaqueMetadata(env.Root() + L"\\target\\sub", nullptr),
                L"Preconditions: the subdirectory of the target must hold an opaque stream");
            auto config = env.MakeConfig();
            Cache cache;
            WhiteoutManager wm(config, &cache);

            Assert::IsFalse(wm.IsOpaqueInLayer(L"link\\sub", env.Upper()),
                L"A directory under a link must not be opaque");
            Assert::IsFalse(wm.HasOpaqueSelfOrAncestorInLayer(L"link\\sub", env.Upper()),
                L"A directory under a link must have no opaque self or ancestor");
        }
    }

    TEST_METHOD(SetOpaque_UpperLink_FailsWithNotADirectoryAndChangesNothingInTheTarget) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            env.CreateDir(env.Root(), L"target");
            const std::wstring target = env.Root() + L"\\target";
            const FILETIME stamped = MakeFileTime(2016, 4, 5);
            StampTimes(target, MakeFileTime(2016, 1, 2), MakeFileTime(2016, 3, 4), stamped);
            if (!LinkCreatedOrSkipped(createLink, env.Upper() + L"\\link", target)) {
                continue;
            }
            auto config = env.MakeConfig();
            Cache cache;
            WhiteoutManager wm(config, &cache);

            AssertStatus(STATUS_NOT_A_DIRECTORY, wm.SetOpaque(L"link"),
                L"SetOpaque on a link must fail");

            Assert::IsFalse(env.FileExists(env.Root(), OpaqueMarkerPath(L"target")),
                L"SetOpaque on a link must write no marker file into the target");
            Assert::IsFalse(MetadataStore::HasOpaqueMetadata(target, nullptr),
                L"SetOpaque on a link must write no opaque stream onto the target");
            Assert::IsFalse(MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\link", nullptr),
                L"SetOpaque on a link must write no opaque stream onto the link");
            FILETIME creation{}, access{}, write{};
            GetTimes(target, &creation, &access, &write);
            Assert::AreEqual(0L, ::CompareFileTime(&stamped, &write),
                L"SetOpaque on a link must leave the target's last-write time as it was");
        }
    }

    TEST_METHOD(SetOpaque_DirectoryUnderLink_FailsWithNotADirectoryAndWritesNothing) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                env.CreateDir(env.Root(), L"target\\sub");
                if (!LinkCreatedOrSkipped(createLink, LinkLayerPath(env, linkSource) + L"\\link",
                                          env.Root() + L"\\target")) {
                    continue;
                }
                auto config = env.MakeConfig();
                Cache cache;
                WhiteoutManager wm(config, &cache);
                const LayerSnapshot upperBefore(env.Upper());

                AssertStatus(STATUS_NOT_A_DIRECTORY, wm.SetOpaque(L"link"),
                    L"SetOpaque on a link must fail");
                AssertStatus(STATUS_NOT_A_DIRECTORY, wm.SetOpaque(L"link\\sub"),
                    L"SetOpaque on a directory under a link must fail");
                AssertStatus(STATUS_NOT_A_DIRECTORY, wm.SetOpaque(L"link\\new"),
                    L"SetOpaque on a missing directory under a link must fail");

                Assert::IsFalse(env.FileExists(env.Root(), OpaqueMarkerPath(L"target")),
                    L"SetOpaque on a link must write no marker file into the target");
                Assert::IsFalse(env.FileExists(env.Root(), OpaqueMarkerPath(L"target\\sub")),
                    L"SetOpaque under a link must write no marker file into the target");
                Assert::IsFalse(MetadataStore::HasOpaqueMetadata(env.Root() + L"\\target\\sub", nullptr),
                    L"SetOpaque under a link must write no opaque stream into the target");
                Assert::IsFalse(env.FileExists(env.Root(), L"target\\new"),
                    L"SetOpaque under a link must make no directory in the target");
                upperBefore.AssertUnchanged(L"SetOpaque under a link must write nothing in the upper");
            }
        }
    }

    TEST_METHOD(RemoveOpaque_UpperLinkToMarkedTarget_ClearsTheLinkStreamAndKeepsTheTargetMarkers) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            if (!UpperLinkToMarkedTargetCreatedOrSkipped(env, createLink)) {
                continue;
            }
            const std::wstring link = env.Upper() + L"\\link";
            Assert::IsTrue(MetadataStore::SetOpaqueMetadata(link, nullptr),
                L"Preconditions: the link must hold its own opaque stream");
            auto config = env.MakeConfig();
            Cache cache;
            WhiteoutManager wm(config, &cache);

            Assert::IsTrue(wm.RemoveOpaque(L"link"), L"RemoveOpaque on a link must succeed");
            Assert::IsTrue(wm.RemoveOpaque(L"link\\sub"),
                L"RemoveOpaque on a directory under a link must succeed");

            Assert::IsFalse(MetadataStore::HasOpaqueMetadata(link, nullptr),
                L"RemoveOpaque on a link must remove the link's own opaque stream");
            AssertTargetMarkersKept(env);
        }
    }

    TEST_METHOD(RemoveOpaque_UpperLinkWithTrailingSeparator_ClearsTheLinkStreamAndKeepsTheTargetMarkers) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            if (!UpperLinkToMarkedTargetCreatedOrSkipped(env, createLink)) {
                continue;
            }
            const std::wstring link = env.Upper() + L"\\link";
            Assert::IsTrue(MetadataStore::SetOpaqueMetadata(link, nullptr),
                L"Preconditions: the link must hold its own opaque stream");
            auto config = env.MakeConfig();
            Cache cache;
            WhiteoutManager wm(config, &cache);

            Assert::IsTrue(wm.RemoveOpaque(L"link\\"), L"RemoveOpaque on a link must succeed");

            Assert::IsFalse(MetadataStore::HasOpaqueMetadata(link, nullptr),
                L"RemoveOpaque on a link with a trailing separator must remove the link's own "
                L"opaque stream");
            AssertTargetMarkersKept(env);
        }
    }

    TEST_METHOD(SetOpaqueAtPath_UpperLink_FailsWithNotADirectoryAndWritesNothingInTheTarget) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            env.CreateDir(env.Root(), L"target");
            const std::wstring link = env.Upper() + L"\\link";
            if (!LinkCreatedOrSkipped(createLink, link, env.Root() + L"\\target")) {
                continue;
            }
            auto config = env.MakeConfig();
            Cache cache;
            WhiteoutManager wm(config, &cache);

            AssertStatus(STATUS_NOT_A_DIRECTORY, wm.SetOpaqueAtPath(link),
                L"SetOpaqueAtPath on a link must fail");

            Assert::IsFalse(env.FileExists(env.Root(), OpaqueMarkerPath(L"target")),
                L"SetOpaqueAtPath on a link must write no marker file into the target");
            Assert::IsFalse(MetadataStore::HasOpaqueMetadata(link, nullptr),
                L"SetOpaqueAtPath on a link must write no opaque stream onto the link");
        }
    }

    TEST_METHOD(SetOpaque_JunctionWithUnreadableReparseTag_FailsWithAccessDeniedAndWritesNothing) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            TempLayerEnvironment env(1);
            env.CreateDir(env.Root(), L"target\\sub");
            const std::wstring junction = LinkLayerPath(env, linkSource) + L"\\link";
            if (!LinkCreatedOrSkipped(CreateDirectoryJunction, junction, env.Root() + L"\\target")) {
                continue;
            }
            LinkAccessDenied synchronizeDenied(junction, SYNCHRONIZE);
            auto config = env.MakeConfig();
            Cache cache;
            WhiteoutManager wm(config, &cache);
            BackupPrivilegeDisabledOnThread noBackupPrivilege;
            DisableRestorePrivilegeOnThread();
            Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, LinkTagOpenError(junction),
                L"The deny ACE must make the junction's reparse tag unreadable");

            AssertStatus(STATUS_ACCESS_DENIED, wm.SetOpaque(L"link"),
                L"SetOpaque on a component whose reparse tag the walk cannot read must fail");
            AssertStatus(STATUS_ACCESS_DENIED, wm.SetOpaque(L"link\\sub"),
                L"SetOpaque under a component whose reparse tag the walk cannot read must fail");

            Assert::IsFalse(env.FileExists(env.Root(), OpaqueMarkerPath(L"target")),
                L"The failed SetOpaque must write no marker file into the junction target");
            Assert::IsFalse(env.FileExists(env.Root(), OpaqueMarkerPath(L"target\\sub")),
                L"The failed SetOpaque must write no marker file under the junction target");
            Assert::IsTrue(linkSource == LayerSource::Upper || !env.FileExists(env.Upper(), L"link"),
                L"The failed SetOpaque must make no link directory in the upper");
        }
    }

    TEST_METHOD(RemoveOpaque_UpperJunctionWithUnreadableReparseTag_ReturnsFalseAndKeepsTheTargetMarkers) {
        TempLayerEnvironment env(1);
        if (!UpperLinkToMarkedTargetCreatedOrSkipped(env, CreateDirectoryJunction)) {
            return;
        }
        const std::wstring junction = env.Upper() + L"\\link";
        LinkAccessDenied synchronizeDenied(junction, SYNCHRONIZE);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        BackupPrivilegeDisabledOnThread noBackupPrivilege;
        DisableRestorePrivilegeOnThread();
        Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, LinkTagOpenError(junction),
            L"The deny ACE must make the junction's reparse tag unreadable");

        Assert::IsFalse(wm.RemoveOpaque(L"link"),
            L"RemoveOpaque on a component whose reparse tag the walk cannot read must fail");
        Assert::IsFalse(wm.RemoveOpaque(L"link\\sub"),
            L"RemoveOpaque under a component whose reparse tag the walk cannot read must fail");

        AssertTargetMarkersKept(env);
    }

    TEST_METHOD(PathResolve_OpaqueUpperDir_HidesAllLowerChildren) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        env.WriteFile(env.Lower(0), L"sub\\hidden.txt", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        wm.SetOpaque(L"sub");

        ResolvedPath result = resolver.ResolvePath(L"sub\\hidden.txt");
        Assert::IsFalse(result.Found(),
            L"Opaque upper dir should hide lower children");
    }

    TEST_METHOD(PathResolve_OpaqueDirInherited_HidesGrandchildren) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        env.WriteFile(env.Lower(0), L"sub\\deep\\hidden.txt", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        wm.SetOpaque(L"sub");

        ResolvedPath result = resolver.ResolvePath(L"sub\\deep\\hidden.txt");
        Assert::IsFalse(result.Found(),
            L"Opacity should propagate to grandchildren");
    }

    TEST_METHOD(PathResolve_AfterRemoveOpaque_LowerChildrenReExposed) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        env.WriteFile(env.Lower(0), L"sub\\file.txt", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        wm.SetOpaque(L"sub");
        Assert::IsFalse(resolver.ResolvePath(L"sub\\file.txt").Found());

        wm.RemoveOpaque(L"sub");
        ResolvedPath result = resolver.ResolvePath(L"sub\\file.txt");
        Assert::IsTrue(result.Found(), L"Lower file should be re-exposed");
        Assert::IsTrue(result.source == LayerSource::Lower);
    }

    TEST_METHOD(MergeDirectoryEntries_DirOpaqueInLower_ShowsThatLowersEntriesAndHidesDeeperLower) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"sub"), "");
        env.WriteFile(env.Lower(0), L"sub\\own.txt", "lower0");
        env.WriteFile(env.Lower(1), L"sub\\below.txt", "lower1");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L"own.txt") == 1,
            L"The listing must show the entry of the lower that holds the opaque marker");
        Assert::IsTrue(merged.count(L"below.txt") == 0,
            L"The listing must hide the entry of the lower below the opaque marker");
    }

    TEST_METHOD(MergeDirectoryEntries_DirOpaqueInMiddleLower_ShowsLowersAboveAndHidesLowersBelow) {
        TempLayerEnvironment env(3);
        env.WriteFile(env.Lower(0), L"sub\\top.txt", "lower0");
        env.WriteFile(env.Lower(1), OpaqueMarkerPath(L"sub"), "");
        env.WriteFile(env.Lower(1), L"sub\\middle.txt", "lower1");
        env.WriteFile(env.Lower(2), L"sub\\bottom.txt", "lower2");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L"top.txt") == 1,
            L"The listing must show the entry of the lower above the opaque marker");
        Assert::IsTrue(merged.count(L"middle.txt") == 1,
            L"The listing must show the entry of the lower that holds the opaque marker");
        Assert::IsTrue(merged.count(L"bottom.txt") == 0,
            L"The listing must hide the entry of the lower below the opaque marker");
    }

    TEST_METHOD(MergeDirectoryEntries_DirOpaqueInLowerWithWhiteout_WhiteoutHidesNameInThatLowerAndBelow) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"sub"), "");
        env.WriteFile(env.Lower(0), WhiteoutMarkerPath(L"sub\\gone.txt"), "");
        env.WriteFile(env.Lower(0), L"sub\\gone.txt", "lower0");
        env.WriteFile(env.Lower(0), L"sub\\own.txt", "lower0");
        env.WriteFile(env.Lower(1), L"sub\\gone.txt", "lower1");
        env.WriteFile(env.Lower(1), L"sub\\below.txt", "lower1");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L"own.txt") == 1,
            L"The listing must show the entry of the lower that holds the opaque marker");
        Assert::IsTrue(merged.count(L"gone.txt") == 0,
            L"The whiteout must hide the name in its own lower and in the lower below");
        Assert::IsTrue(merged.count(L"below.txt") == 0,
            L"The listing must hide the entry of the lower below the opaque marker");
    }

    TEST_METHOD(MergeDirectoryEntries_DirOpaqueInLower_OpaqueMarkerFileNotListed) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"sub"), "");
        env.WriteFile(env.Lower(0), L"sub\\own.txt", "lower0");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L".wh..wh..opq") == 0,
            L"The listing must not show the opaque marker file");
        Assert::AreEqual(static_cast<size_t>(1), merged.size(),
            L"The listing must hold only the entry of the lower");
    }

    TEST_METHOD(MergeDirectoryEntries_AncestorOpaqueInLower_ShowsThatLowersEntriesAndHidesDeeperLower) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"d"), "");
        env.WriteFile(env.Lower(0), L"d\\sub\\own.txt", "lower0");
        env.WriteFile(env.Lower(1), L"d\\sub\\below.txt", "lower1");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"d\\sub").entries;

        Assert::IsTrue(merged.count(L"own.txt") == 1,
            L"The listing must show the entry of the lower that holds the opaque marker on the ancestor");
        Assert::IsTrue(merged.count(L"below.txt") == 0,
            L"The listing must hide the entry of the lower below the opaque marker on the ancestor");

        AssertEveryListedEntryResolves(env, L"d\\sub", merged);
    }

    TEST_METHOD(MergeDirectoryEntries_AncestorOpaqueInUpper_ShowsUpperEntriesAndHidesEveryLower) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"d"), "");
        env.WriteFile(env.Upper(), L"d\\sub\\up.txt", "upper");
        env.WriteFile(env.Lower(0), L"d\\sub\\low0.txt", "lower0");
        env.WriteFile(env.Lower(1), L"d\\sub\\low1.txt", "lower1");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"d\\sub").entries;

        Assert::IsTrue(merged.count(L"up.txt") == 1,
            L"The listing must show the entry of the upper");
        Assert::IsTrue(merged.count(L"low0.txt") == 0,
            L"The listing must hide the entry of the first lower below the opaque marker on the ancestor");
        Assert::IsTrue(merged.count(L"low1.txt") == 0,
            L"The listing must hide the entry of the second lower below the opaque marker on the ancestor");

        AssertEveryListedEntryResolves(env, L"d\\sub", merged);
    }

    TEST_METHOD(PathResolve_DirOpaqueInLower_ResolvesChildOfThatLowerAndHidesChildOfDeeperLower) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"sub"), "");
        env.WriteFile(env.Lower(0), L"sub\\own.txt", "lower0");
        env.WriteFile(env.Lower(1), L"sub\\below.txt", "lower1");

        ResolverUnderTest r(env);

        ResolvedPath own = r.resolver.ResolvePath(L"sub\\own.txt");
        Assert::IsTrue(own.Found(),
            L"A child in the lower that holds the opaque marker must resolve");
        Assert::IsTrue(own.source == LayerSource::Lower);
        Assert::AreEqual(0, own.lowerIndex);

        ResolvedPath below = r.resolver.ResolvePath(L"sub\\below.txt");
        Assert::IsFalse(below.Found(),
            L"A child that only a deeper lower has must not resolve");
    }

    TEST_METHOD(ResolveLowerPath_DirOpaqueInLower_ResolvesChildOfThatLowerAndHidesChildOfDeeperLower) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"sub"), "");
        env.WriteFile(env.Lower(0), L"sub\\own.txt", "lower0");
        env.WriteFile(env.Lower(1), L"sub\\below.txt", "lower1");

        ResolverUnderTest r(env);

        ResolvedPath own = r.resolver.ResolveLowerPath(L"sub\\own.txt");
        Assert::IsTrue(own.Found(),
            L"A child in the lower that holds the opaque marker must resolve");
        Assert::IsTrue(own.source == LayerSource::Lower);
        Assert::AreEqual(0, own.lowerIndex);

        ResolvedPath below = r.resolver.ResolveLowerPath(L"sub\\below.txt");
        Assert::IsFalse(below.Found(),
            L"A child that only a deeper lower has must not resolve");
    }

    TEST_METHOD(PathResolve_DirOpaqueInLowerWithWhiteoutForChild_HidesChild) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"sub"), "");
        env.WriteFile(env.Lower(0), WhiteoutMarkerPath(L"sub\\gone.txt"), "");
        env.WriteFile(env.Lower(0), L"sub\\gone.txt", "lower0");
        env.WriteFile(env.Lower(1), L"sub\\gone.txt", "lower1");

        ResolverUnderTest r(env);

        Assert::IsFalse(r.resolver.ResolvePath(L"sub\\gone.txt").Found(),
            L"The whiteout must hide the child in its own lower and in the lower below");
        Assert::IsFalse(r.resolver.ResolveLowerPath(L"sub\\gone.txt").Found(),
            L"The whiteout must hide the child in its own lower and in the lower below");
    }

    TEST_METHOD(PathResolve_DirOpaqueInLowerWithWhiteoutForAncestor_HidesDescendant) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"a"), "");
        env.WriteFile(env.Lower(0), WhiteoutMarkerPath(L"a\\b"), "");
        env.WriteFile(env.Lower(0), L"a\\b\\c.txt", "lower0");
        env.WriteFile(env.Lower(1), L"a\\b\\c.txt", "lower1");

        ResolverUnderTest r(env);

        Assert::IsFalse(r.resolver.ResolvePath(L"a\\b\\c.txt").Found(),
            L"The whiteout for the ancestor must hide the descendant in its own lower and in the lower below");
        Assert::IsFalse(r.resolver.ResolveLowerPath(L"a\\b\\c.txt").Found(),
            L"The whiteout for the ancestor must hide the descendant in its own lower and in the lower below");
    }

    TEST_METHOD(PathResolve_OpaqueMarkerAtRootOfLower_HidesDeeperLowersTopLevelEntryAsTheListingDoes) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L".wh..wh..opq", "");
        env.WriteFile(env.Lower(1), L"deep.txt", "lower1");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        ResolverUnderTest r(env);

        auto merged = mount.MergeDirectoryEntries(L"").entries;

        AssertHiddenFromListingAndLookup(merged, r.resolver, L"deep.txt", L"deep.txt",
            L"The opaque marker at the root of the lower");
    }

    TEST_METHOD(PathResolve_OpaqueMarkerAtRootOfExtendedFormLowerWithTrailingSeparator_HidesDeeperLowersTopLevelEntryAsTheListingDoes) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L".wh..wh..opq", "");
        env.WriteFile(env.Lower(1), L"deep.txt", "lower1");
        auto config = env.MakeConfig();
        config.lowerPaths[0] = HostPath(ExtendedDirWithSeparator(env.Lower(0)));
        ::LayerMount::LayerMount mount(config);
        ResolverUnderTest r(config);

        auto merged = mount.MergeDirectoryEntries(L"").entries;

        AssertHiddenFromListingAndLookup(merged, r.resolver, L"deep.txt", L"deep.txt",
            L"The opaque marker at the root of the extended-form lower that ends in a separator");
    }

    TEST_METHOD(PathResolve_OpaqueMarkerAtRootOfLower_ResolvesThatLowersTopLevelEntry) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L".wh..wh..opq", "");
        env.WriteFile(env.Lower(0), L"own.txt", "lower0");
        env.WriteFile(env.Lower(1), L"own.txt", "lower1");
        ResolverUnderTest r(env);

        const ResolvedPath own = r.resolver.ResolvePath(L"own.txt");

        Assert::IsTrue(own.Found(),
            L"The entry at the root of the opaque lower must resolve");
        Assert::IsTrue(own.source == LayerSource::Lower,
            L"The entry at the root of the opaque lower must resolve from a lower");
        Assert::AreEqual(0, own.lowerIndex,
            L"The entry at the root of the opaque lower must resolve to that lower");
    }

    TEST_METHOD(PathResolve_OpaqueMarkerAtRootOfUpper_HidesLowersTopLevelEntryAsTheListingDoes) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L".wh..wh..opq", "");
        env.WriteFile(env.Lower(0), L"low.txt", "lower0");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        ResolverUnderTest r(env);

        auto merged = mount.MergeDirectoryEntries(L"").entries;

        AssertHiddenFromListingAndLookup(merged, r.resolver, L"low.txt", L"low.txt",
            L"The opaque marker at the root of the upper");
    }

    TEST_METHOD(PathResolve_OpaqueMarkerAtRootOfLowerWithoutTheDirectory_HidesDeeperLowersNestedEntryAsTheListingDoes) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L".wh..wh..opq", "");
        env.WriteFile(env.Lower(1), L"a\\x.txt", "lower1");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        ResolverUnderTest r(env);

        auto merged = mount.MergeDirectoryEntries(L"").entries;

        AssertHiddenFromListingAndLookup(merged, r.resolver, L"a", L"a\\x.txt",
            L"The opaque marker at the root of the lower");
    }

    TEST_METHOD(PathResolve_OpaqueMarkerAtRootOfLowerHoldingTheDirectory_HidesDeeperLowersNestedEntryAsTheListingDoes) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L".wh..wh..opq", "");
        env.WriteFile(env.Lower(0), L"a\\own.txt", "lower0");
        env.WriteFile(env.Lower(1), L"a\\x.txt", "lower1");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        ResolverUnderTest r(env);

        auto merged = mount.MergeDirectoryEntries(L"a").entries;

        Assert::IsTrue(merged.count(L"own.txt") == 1,
            L"The listing must show the entry of the opaque lower's own directory");
        Assert::IsTrue(r.resolver.ResolvePath(L"a\\own.txt").Found(),
            L"The entry of the opaque lower's own directory must resolve");
        AssertHiddenFromListingAndLookup(merged, r.resolver, L"x.txt", L"a\\x.txt",
            L"The opaque marker at the root of the lower");
    }

    TEST_METHOD(PathResolve_OpaqueMarkerAtRootOfUpper_HidesLowersNestedEntryAsTheListingDoes) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L".wh..wh..opq", "");
        env.WriteFile(env.Upper(), L"a\\own.txt", "upper");
        env.WriteFile(env.Lower(0), L"a\\low.txt", "lower0");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        ResolverUnderTest r(env);

        auto merged = mount.MergeDirectoryEntries(L"a").entries;

        Assert::IsTrue(merged.count(L"own.txt") == 1,
            L"The listing must show the entry of the upper's own directory");
        AssertHiddenFromListingAndLookup(merged, r.resolver, L"low.txt", L"a\\low.txt",
            L"The opaque marker at the root of the upper");
    }
};

TEST_CLASS(SidecarDirectoryListingTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(MergeDirectoryEntries_SidecarNamedDirBelowRootInUpper_IsListed) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub\\.overlay");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L".overlay") == 1,
            L"A user's .overlay directory below the root must show in the listing");
        AssertEveryListedEntryResolves(env, L"sub", merged);
    }

    TEST_METHOD(MergeDirectoryEntries_SidecarNamedDirBelowRootInLower_IsListed) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"sub\\.overlay");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L".overlay") == 1,
            L"A lower's .overlay directory below the root must show in the listing");
        AssertEveryListedEntryResolves(env, L"sub", merged);
    }

    TEST_METHOD(MergeDirectoryEntries_SidecarNamedDirCreatedBelowRoot_IsListed) {
        TempLayerEnvironment env(1);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        for (const wchar_t* dir : {L"sub", L"sub\\.overlay"}) {
            Assert::IsTrue(NT_SUCCESS(CreateThroughMount(mount, dir, FILE_DIRECTORY_FILE)),
                (std::wstring(L"The directory create must succeed: ") + dir).c_str());
        }

        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L".overlay") == 1,
            L"A .overlay directory that the engine created below the root must show in the listing");
    }

    TEST_METHOD(MergeDirectoryEntries_SidecarDirAtRoot_IsNotListed) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L".overlay");
        env.CreateDir(env.Lower(0), L".overlay");
        env.WriteFile(env.Upper(), L"visible.txt", "v");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"").entries;

        Assert::IsTrue(merged.count(L".overlay") == 0,
            L"The sidecar directory at the root must not show in the listing");
        Assert::IsTrue(merged.count(L"visible.txt") == 1,
            L"The listing of the root must show the upper's file");
    }
};

TEST_CLASS(MarkerNameResolutionTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(PathResolve_WhiteoutMarkerInUpper_IsNotFound) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"sub\\.wh.name", "");

        ResolverUnderTest r(env);

        Assert::IsFalse(r.resolver.ResolvePath(L"sub\\.wh.name").Found(),
            L"A whiteout marker in the upper must not resolve");
    }

    TEST_METHOD(PathResolve_OpaqueMarkerInUpper_IsNotFound) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"sub\\.wh..wh..opq", "");

        ResolverUnderTest r(env);

        Assert::IsFalse(r.resolver.ResolvePath(L"sub\\.wh..wh..opq").Found(),
            L"An opaque marker in the upper must not resolve");
    }

    TEST_METHOD(PathResolve_WhiteoutMarkerInLower_IsNotFound) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\.wh.name", "");

        ResolverUnderTest r(env);

        Assert::IsFalse(r.resolver.ResolvePath(L"sub\\.wh.name").Found(),
            L"A whiteout marker in a lower must not resolve");
        Assert::IsFalse(r.resolver.ResolveLowerPath(L"sub\\.wh.name").Found(),
            L"A whiteout marker in a lower must not resolve in the lowers");
    }

    TEST_METHOD(PathResolve_OpaqueMarkerInLower_IsNotFound) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\.wh..wh..opq", "");

        ResolverUnderTest r(env);

        Assert::IsFalse(r.resolver.ResolvePath(L"sub\\.wh..wh..opq").Found(),
            L"An opaque marker in a lower must not resolve");
        Assert::IsFalse(r.resolver.ResolveLowerPath(L"sub\\.wh..wh..opq").Found(),
            L"An opaque marker in a lower must not resolve in the lowers");
    }

    TEST_METHOD(PathResolve_UpperCaseMarkerName_IsNotFound) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\.wh.name", "");

        ResolverUnderTest r(env);

        Assert::IsFalse(r.resolver.ResolvePath(L"sub\\.WH.name").Found(),
            L"A marker must not resolve when the caller spells its prefix in upper case");
        Assert::IsFalse(r.resolver.ResolveLowerPath(L"sub\\.WH.name").Found(),
            L"A marker must not resolve in the lowers when the caller spells its prefix in upper case");
    }

    TEST_METHOD(PathResolve_ChildOfMarkerNamedDirInLower_IsNotFound) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\.wh.x\\child.txt", "lower");

        ResolverUnderTest r(env);

        Assert::IsFalse(r.resolver.ResolvePath(L"sub\\.wh.x\\child.txt").Found(),
            L"A path below a marker-named directory must not resolve");
        Assert::IsFalse(r.resolver.ResolveLowerPath(L"sub\\.wh.x\\child.txt").Found(),
            L"A path below a marker-named directory must not resolve in the lowers");
    }

    TEST_METHOD(PathResolve_NameContainingMarkerPrefixMidName_Resolves) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\file.wh.txt", "lower");

        ResolverUnderTest r(env);

        Assert::IsTrue(r.resolver.ResolvePath(L"sub\\file.wh.txt").Found(),
            L"A name that holds .wh. after its first character is an ordinary file and must resolve");
    }
};

TEST_CLASS(MarkerNameMountTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(Open_WhiteoutMarkerInUpper_IsNotFound) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"sub\\.wh.name", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"sub\\.wh.name"),
            L"Opening a whiteout marker in the upper must report not found");
    }

    TEST_METHOD(Open_OpaqueMarkerInLower_IsNotFound) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\.wh..wh..opq", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"sub\\.wh..wh..opq"),
            L"Opening an opaque marker in a lower must report not found");
    }

    TEST_METHOD(MergeDirectoryEntries_UpperCaseMarkerInLower_IsNotListed) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\.WH.foo", "");
        env.WriteFile(env.Lower(0), L"sub\\keep.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L".wh.foo") == 0,
            L"A marker spelled in upper case must not be listed");
        Assert::IsTrue(merged.count(L"keep.txt") == 1,
            L"An ordinary sibling of the marker must stay listed");
        AssertEveryListedEntryResolves(env, L"sub", merged);
    }

    TEST_METHOD(MergeDirectoryEntries_UpperCaseWhiteoutInUpper_HidesLowerEntry) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\gone.txt", "lower");
        env.WriteFile(env.Upper(), L"sub\\.WH.gone.txt", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L"gone.txt") == 0,
            L"A whiteout spelled in upper case must hide the lower entry in the listing");
        Assert::IsTrue(merged.count(L".wh.gone.txt") == 0,
            L"A whiteout spelled in upper case must not be listed");
        AssertEveryListedEntryResolves(env, L"sub", merged);
    }

    TEST_METHOD(MergeDirectoryEntries_MarkerNamedDirInLower_IsEmpty) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\.wh.x\\child.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        const MergedDirectory merged = mount.MergeDirectoryEntries(L"sub\\.wh.x");

        AssertStatus(STATUS_SUCCESS, merged.status, L"A marker-named directory is a reserved path, not a failed scan");
        Assert::IsTrue(merged.entries.empty(),
            L"A marker-named directory must list no entries");
    }

    TEST_METHOD(Create_WhiteoutMarkerName_IsDeniedAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        env.WriteFile(env.Lower(0), L"sub\\name", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_ACCESS_DENIED, CreateThroughMount(mount, L"sub\\.wh.name", kNoCreateOptions),
            L"Creating a whiteout marker name must be denied");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\.wh.name"),
            L"A denied create must write no marker in the upper");
        Assert::IsTrue(NT_SUCCESS(OpenThroughMount(mount, L"sub\\name")),
            L"A denied create must not hide the lower file");
    }

    TEST_METHOD(Create_OpaqueMarkerName_IsDeniedAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        env.WriteFile(env.Lower(0), L"sub\\lower.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_ACCESS_DENIED, CreateThroughMount(mount, L"sub\\.wh..wh..opq", kNoCreateOptions),
            L"Creating the opaque marker name must be denied");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\.wh..wh..opq"),
            L"A denied create must write no opaque marker in the upper");
        Assert::IsTrue(NT_SUCCESS(OpenThroughMount(mount, L"sub\\lower.txt")),
            L"A denied create must not make the directory opaque");
    }

    TEST_METHOD(Create_UpperCaseMarkerName_IsDenied) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_ACCESS_DENIED, CreateThroughMount(mount, L"sub\\.WH.name", kNoCreateOptions),
            L"Creating a marker name spelled in upper case must be denied");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\.wh.name"),
            L"A denied create must write no marker in the upper");
    }

    TEST_METHOD(Create_MarkerNameWithStream_IsDenied) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_ACCESS_DENIED, CreateThroughMount(mount, L"sub\\.wh.name:stream", kNoCreateOptions),
            L"Creating a stream on a marker name must be denied");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\.wh.name"),
            L"A denied create must write no marker in the upper");
    }

    TEST_METHOD(Create_MarkerNamedDirectory_IsDenied) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_ACCESS_DENIED, CreateThroughMount(mount, L"sub\\.wh.dir", FILE_DIRECTORY_FILE),
            L"Creating a directory with a marker name must be denied");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\.wh.dir"),
            L"A denied create must write no directory in the upper");
    }

    TEST_METHOD(Rename_ToMarkerName_IsDeniedAndLeavesSource) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"sub\\a.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_ACCESS_DENIED, mount.Rename(L"sub\\a.txt", L"sub\\.wh.a.txt", kFailIfExists, kNoCallerPid),
            L"Renaming onto a marker name must be denied");
        Assert::AreEqual(std::string("upper"), env.ReadFile(env.Upper(), L"sub\\a.txt"),
            L"A denied rename must leave the source in place");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\.wh.a.txt"),
            L"A denied rename must write no marker in the upper");
    }

    TEST_METHOD(RenameContext_ToMarkerName_IsDeniedAndLeavesSource) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"sub\\a.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        Assert::IsTrue(NT_SUCCESS(mount.Open(L"sub\\a.txt", FILE_READ_DATA | DELETE,
                                             kNoCreateOptions, kNoCallerPid,
                                             &ctx, &info)));

        const NTSTATUS status = mount.Rename(ctx.get(), L"sub\\.wh.a.txt",
                                             kFailIfExists, kNoCallerPid);
        mount.Close(ctx.get());

        AssertStatus(STATUS_ACCESS_DENIED, status, L"Renaming an open file onto a marker name must be denied");
        Assert::AreEqual(std::string("upper"), env.ReadFile(env.Upper(), L"sub\\a.txt"),
            L"A denied rename must leave the source in place");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\.wh.a.txt"),
            L"A denied rename must write no marker in the upper");
    }

    TEST_METHOD(UpdateContextPath_ToMarkerName_IsInvalidParameter) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"sub\\a.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        Assert::IsTrue(NT_SUCCESS(mount.Open(L"sub\\a.txt", FILE_READ_DATA,
                                             kNoCreateOptions, kNoCallerPid,
                                             &ctx, &info)));

        const NTSTATUS status = mount.UpdateContextPath(ctx.get(), L"sub\\.wh.a.txt");
        const std::wstring pathAfter = ctx->relativePath;
        mount.Close(ctx.get());

        AssertStatus(STATUS_INVALID_PARAMETER, status, L"Moving an open context onto a marker name must be rejected");
        Assert::AreEqual(std::wstring(L"sub\\a.txt"), pathAfter,
            L"A rejected update must leave the context path unchanged");
    }

    TEST_METHOD(Delete_WhiteoutMarkerInUpper_IsNotFoundAndLeavesMarker) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\gone.txt", "lower");
        env.WriteFile(env.Upper(), L"sub\\.wh.gone.txt", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, mount.Delete(L"sub\\.wh.gone.txt", kNoCallerPid),
            L"Deleting a whiteout marker must report not found");
        Assert::IsTrue(env.FileExists(env.Upper(), L"sub\\.wh.gone.txt"),
            L"A refused delete must leave the marker in place");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"sub\\gone.txt"),
            L"A refused delete must keep the lower file hidden");
    }

    TEST_METHOD(Rename_FromMarkerName_IsDeniedAndLeavesMarker) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\gone.txt", "lower");
        env.WriteFile(env.Upper(), L"sub\\.wh.gone.txt", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_ACCESS_DENIED,
                     mount.Rename(L"sub\\.wh.gone.txt", L"sub\\back.txt", kFailIfExists, kNoCallerPid),
                     L"Renaming a marker must be denied");
        Assert::IsTrue(env.FileExists(env.Upper(), L"sub\\.wh.gone.txt"),
            L"A denied rename must leave the marker in place");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"sub\\gone.txt"),
            L"A denied rename must keep the lower file hidden");
    }
};

TEST_CLASS(LinkTargetWhiteoutTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(HasWhiteout_WhiteoutNamedFilesInLinkTarget_ReturnsFalse) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                env.WriteFile(env.Root(), WhiteoutMarkerPath(L"target\\sub"), "target");
                env.WriteFile(env.Root(), WhiteoutMarkerPath(L"target\\deep\\name"), "target");
                if (!LinkToWhiteoutNamedFileCreatedOrSkipped(env, linkSource, createLink)) {
                    continue;
                }
                const std::wstring& layer = LinkLayerPath(env, linkSource);
                auto config = env.MakeConfig();
                Cache cache;
                WhiteoutManager wm(config, &cache);

                Assert::IsFalse(wm.HasWhiteout(L"link\\foo", layer),
                    L"A .wh.foo file in a link target must not be a whiteout");
                Assert::IsFalse(wm.HasWhiteout(L"link\\deep\\name", layer),
                    L"A .wh. file under a link target must not be a whiteout");
                Assert::IsFalse(wm.HasWhiteoutInAnyLayer(L"link\\foo"),
                    L"No layer must count a .wh.foo file in a link target as a whiteout");
                Assert::IsFalse(wm.HasWhitedOutAncestorInLayer(L"link\\sub\\x", layer),
                    L"A .wh.sub file in a link target must not white out link\\sub");
            }
        }
    }

    TEST_METHOD(ResolvePath_MissingNameNextToWhiteoutNamedFileInLinkTarget_IsNotFoundAndNotWhiteout) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                if (!LinkToWhiteoutNamedFileCreatedOrSkipped(env, linkSource, createLink)) {
                    continue;
                }
                ResolverUnderTest r(env);

                const ResolvedPath resolved = r.resolver.ResolvePath(L"link\\foo");

                Assert::IsFalse(resolved.Found(), L"link\\foo must not resolve, as the target holds no foo");
                Assert::IsFalse(resolved.isWhiteout,
                    L"The .wh.foo file in the link target must not make link\\foo a whiteout");
            }
        }
    }

    TEST_METHOD(Open_FileNextToWhiteoutNamedFileInLinkTarget_ReadsTheTargetFile) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                env.WriteFile(env.Root(), L"target\\foo", "shown");
                if (!LinkToWhiteoutNamedFileCreatedOrSkipped(env, linkSource, createLink)) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());

                Assert::AreEqual(std::string("shown"), ReadThroughMount(mount, L"link\\foo"),
                    L"The .wh.foo file in the link target must not hide the target's foo");
            }
        }
    }

    TEST_METHOD(Open_WhiteoutNamedFileInLinkTarget_ReadsTheTargetFile) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                if (!LinkToWhiteoutNamedFileCreatedOrSkipped(env, linkSource, createLink)) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());

                Assert::AreEqual(std::string("target"), ReadThroughMount(mount, L"link\\.wh.foo"),
                    L"A .wh.foo file in a link target must open as an ordinary file");
            }
        }
    }

    TEST_METHOD(MergeDirectoryEntries_LinkToTargetWithWhiteoutNamedFiles_ListsThemAndTheNamesTheyMatch) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                env.WriteFile(env.Root(), L"target\\shown", "target");
                env.WriteFile(env.Root(), WhiteoutMarkerPath(L"target\\shown"), "target");
                env.WriteFile(env.Root(), OpaqueMarkerPath(L"target"), "target");
                env.WriteFile(env.Root(), WhiteoutMarkerPath(L"target\\sub\\name"), "target");
                if (!LinkToWhiteoutNamedFileCreatedOrSkipped(env, linkSource, createLink)) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());

                const MergedDirectory atLink = mount.MergeDirectoryEntries(L"link");
                const MergedDirectory underLink = mount.MergeDirectoryEntries(L"link\\sub");

                AssertStatus(STATUS_SUCCESS, atLink.status, L"The merge at the link must succeed");
                Assert::AreEqual<size_t>(5, atLink.entries.size(),
                    L"The listing of the link must hold every entry of the target");
                for (const wchar_t* name : {L".wh.foo", L".wh.shown", L"shown", L".wh..wh..opq", L"sub"}) {
                    Assert::IsTrue(atLink.entries.count(name) == 1,
                        (std::wstring(L"The listing of the link must hold ") + name).c_str());
                }
                AssertEveryListedEntryResolves(env, L"link", atLink.entries);
                AssertStatus(STATUS_SUCCESS, underLink.status, L"The merge under the link must succeed");
                Assert::IsTrue(underLink.entries.count(L".wh.name") == 1,
                    L"The listing under the link must hold the .wh.name file");
                AssertEveryListedEntryResolves(env, L"link\\sub", underLink.entries);
            }
        }
    }

    TEST_METHOD(WhiteoutInUpperDirectoryOverLowerLink_StaysAMarker) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"link\\foo"), "");
            env.WriteFile(env.Upper(), L"link\\kept.txt", "upper");
            if (!LinkToWhiteoutNamedFileCreatedOrSkipped(env, LayerSource::Lower, createLink)) {
                continue;
            }
            ::LayerMount::LayerMount mount(env.MakeConfig());

            const MergedDirectory listed = mount.MergeDirectoryEntries(L"link");

            Assert::IsTrue(listed.entries.count(L".wh.foo") == 0,
                L"A whiteout in an upper directory over a lower link must not be listed");
            Assert::IsTrue(listed.entries.count(L"kept.txt") == 1,
                L"The upper directory's own file must be listed");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"link\\.wh.foo"),
                L"A whiteout in an upper directory over a lower link must not open");
            AssertStatus(STATUS_ACCESS_DENIED, CreateThroughMount(mount, L"link\\.wh.new", kNoCreateOptions),
                L"A create of a .wh. name in an upper directory over a lower link must be denied");
        }
    }

    TEST_METHOD(CreateWhiteout_UnderLink_FailsWithNotADirectoryAndWritesNothing) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                env.CreateDir(env.Root(), L"target\\sub");
                const std::wstring target = env.Root() + L"\\target";
                const FILETIME stamped = MakeFileTime(2016, 4, 5);
                StampTimes(target, MakeFileTime(2016, 1, 2), MakeFileTime(2016, 3, 4), stamped);
                if (!LinkCreatedOrSkipped(createLink, LinkLayerPath(env, linkSource) + L"\\link", target)) {
                    continue;
                }
                auto config = env.MakeConfig();
                Cache cache;
                WhiteoutManager wm(config, &cache);
                const LayerSnapshot upperBefore(env.Upper());

                AssertStatus(STATUS_NOT_A_DIRECTORY, wm.CreateWhiteout(L"link\\foo", WhiteoutType::File),
                    L"A whiteout in a link must fail");
                AssertStatus(STATUS_NOT_A_DIRECTORY, wm.CreateWhiteout(L"link\\sub\\foo", WhiteoutType::Directory),
                    L"A whiteout under a link must fail");
                AssertStatus(STATUS_NOT_A_DIRECTORY, wm.CreateWhiteout(L"link\\new\\foo", WhiteoutType::File),
                    L"A whiteout in a missing directory under a link must fail");

                Assert::IsFalse(env.FileExists(env.Root(), WhiteoutMarkerPath(L"target\\foo")),
                    L"The failed whiteout must write no marker into the target");
                Assert::IsFalse(env.FileExists(env.Root(), WhiteoutMarkerPath(L"target\\sub\\foo")),
                    L"The failed whiteout must write no marker under the target");
                Assert::IsFalse(env.FileExists(env.Root(), L"target\\new"),
                    L"The failed whiteout must make no directory in the target");
                FILETIME creation{}, access{}, write{};
                GetTimes(target, &creation, &access, &write);
                Assert::AreEqual(0L, ::CompareFileTime(&stamped, &write),
                    L"The failed whiteout must leave the target's last-write time as it was");
                upperBefore.AssertUnchanged(L"The failed whiteout must write nothing in the upper");
            }
        }
    }

    TEST_METHOD(CreateWhiteoutAndSetOpaque_UnderLowerLinkThatAnUpperDirectoryHides_WriteTheMarkersInTheUpper) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            env.CreateDir(env.Root(), L"target");
            env.CreateDir(env.Upper(), L"link\\sub");
            if (!LinkCreatedOrSkipped(createLink, env.Lower(0) + L"\\link", env.Root() + L"\\target")) {
                continue;
            }
            auto config = env.MakeConfig();
            Cache cache;
            WhiteoutManager wm(config, &cache);

            AssertStatus(STATUS_SUCCESS, wm.CreateWhiteout(L"link\\foo", WhiteoutType::File),
                L"A whiteout in an upper directory over a lower link must succeed");
            AssertStatus(STATUS_SUCCESS, wm.SetOpaque(L"link\\sub"),
                L"SetOpaque on an upper directory over a lower link must succeed");

            Assert::IsTrue(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"link\\foo")),
                L"The whiteout must go into the upper directory");
            Assert::IsTrue(wm.IsOpaque(L"link\\sub"), L"The upper directory must be opaque");
            Assert::IsFalse(env.FileExists(env.Root(), WhiteoutMarkerPath(L"target\\foo")),
                L"The whiteout must write nothing into the link target");
        }
    }

    TEST_METHOD(CreateWhiteout_UnderJunctionWithUnreadableReparseTag_FailsWithAccessDeniedAndWritesNothing) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            TempLayerEnvironment env(1);
            env.CreateDir(env.Root(), L"target\\sub");
            const std::wstring junction = LinkLayerPath(env, linkSource) + L"\\link";
            if (!LinkCreatedOrSkipped(CreateDirectoryJunction, junction, env.Root() + L"\\target")) {
                continue;
            }
            LinkAccessDenied synchronizeDenied(junction, SYNCHRONIZE);
            auto config = env.MakeConfig();
            Cache cache;
            WhiteoutManager wm(config, &cache);
            BackupPrivilegeDisabledOnThread noBackupPrivilege;
            DisableRestorePrivilegeOnThread();
            Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, LinkTagOpenError(junction),
                L"The deny ACE must make the junction's reparse tag unreadable");

            AssertStatus(STATUS_ACCESS_DENIED, wm.CreateWhiteout(L"link\\foo", WhiteoutType::File),
                L"A whiteout in a component whose reparse tag the walk cannot read must fail");
            AssertStatus(STATUS_ACCESS_DENIED, wm.CreateWhiteout(L"link\\sub\\foo", WhiteoutType::File),
                L"A whiteout under a component whose reparse tag the walk cannot read must fail");

            Assert::IsFalse(env.FileExists(env.Root(), WhiteoutMarkerPath(L"target\\foo")),
                L"The failed whiteout must write no marker into the junction target");
            Assert::IsFalse(env.FileExists(env.Root(), WhiteoutMarkerPath(L"target\\sub\\foo")),
                L"The failed whiteout must write no marker under the junction target");
            Assert::IsTrue(linkSource == LayerSource::Upper || !env.FileExists(env.Upper(), L"link"),
                L"The failed whiteout must make no link directory in the upper");
        }
    }

    TEST_METHOD(RemoveWhiteout_UnderUpperJunctionWithUnreadableReparseTag_FailsWithAccessDeniedAndKeepsTheTargetFile) {
        TempLayerEnvironment env(1);
        if (!LinkToWhiteoutNamedFileCreatedOrSkipped(env, LayerSource::Upper,
                                                     CreateDirectoryJunction)) {
            return;
        }
        const std::wstring junction = env.Upper() + L"\\link";
        LinkAccessDenied synchronizeDenied(junction, SYNCHRONIZE);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        BackupPrivilegeDisabledOnThread noBackupPrivilege;
        DisableRestorePrivilegeOnThread();
        Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, LinkTagOpenError(junction),
            L"The deny ACE must make the junction's reparse tag unreadable");

        AssertStatus(STATUS_ACCESS_DENIED, wm.RemoveWhiteout(L"link\\foo"),
            L"RemoveWhiteout under a component whose reparse tag the walk cannot read must fail");

        Assert::IsTrue(env.FileExists(env.Root(), WhiteoutMarkerPath(L"target\\foo")),
            L"The failed RemoveWhiteout must keep the .wh.foo file in the target");
    }

    TEST_METHOD(RemoveWhiteout_UnderUpperLink_SucceedsAndKeepsTheTargetFile) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            if (!LinkToWhiteoutNamedFileCreatedOrSkipped(env, LayerSource::Upper, createLink)) {
                continue;
            }
            auto config = env.MakeConfig();
            Cache cache;
            WhiteoutManager wm(config, &cache);

            AssertStatus(STATUS_SUCCESS, wm.RemoveWhiteout(L"link\\foo"),
                L"RemoveWhiteout under a link must succeed");

            Assert::IsTrue(env.FileExists(env.Root(), WhiteoutMarkerPath(L"target\\foo")),
                L"RemoveWhiteout under a link must keep the .wh.foo file in the target");
        }
    }
};

}
