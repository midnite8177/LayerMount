#include "pch.h"
#include "TestFixture.h"

#include "WhiteoutManager.h"
#include "PathResolver.h"
#include "Cache.h"
#include "MetadataADS.h"
#include "AclTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::BackupPrivilegeDisabledOnThread;
using LayerMountTestShared::DirectoryListingDenied;
using LayerMountTestShared::AssertListingDenied;

namespace LayerMountTests {

namespace {

struct ResolverUnderTest {
    explicit ResolverUnderTest(const TempLayerEnvironment& env)
        : config(env.MakeConfig())
        , wm(config, &cache)
        , resolver(config, wm, cache) {}

    ::LayerMount::LayerConfig config;
    Cache cache;
    WhiteoutManager wm;
    PathResolver resolver;
};

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

}

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

        Assert::IsTrue(wm.CreateWhiteout(L"foo.txt", WhiteoutType::File));

        std::wstring whPath = env.Upper() + L"\\.wh.foo.txt";
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

        Assert::IsTrue(wm.CreateWhiteout(L"sub\\foo.txt", WhiteoutType::File));

        std::wstring parentDir = env.Upper() + L"\\sub";
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(parentDir.c_str()),
            L"Parent directory should be auto-created");

        std::wstring whPath = env.Upper() + L"\\sub\\.wh.foo.txt";
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

        std::wstring lowerWh = env.Lower(0) + L"\\.wh.foo.txt";
        HANDLE h = ::CreateFileW(lowerWh.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h);

        Assert::IsTrue(wm.HasWhiteoutInAnyLayer(L"foo.txt"));
    }

    TEST_METHOD(RemoveWhiteout_ExistingMarker_DeletesAndReturnsTrue) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.CreateWhiteout(L"foo.txt", WhiteoutType::File);
        Assert::IsTrue(wm.HasWhiteout(L"foo.txt", env.Upper()));

        Assert::IsTrue(wm.RemoveWhiteout(L"foo.txt"));
        Assert::IsFalse(wm.HasWhiteout(L"foo.txt", env.Upper()));
    }

    TEST_METHOD(RemoveWhiteout_NoMarker_ReturnsTrue) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsTrue(wm.RemoveWhiteout(L"nonexistent.txt"),
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

        Assert::AreEqual(static_cast<long>(STATUS_ACCESS_DENIED), static_cast<long>(merged.status),
            L"A failed lower scan must report the scan's status");
        Assert::IsTrue(merged.entries.empty(),
            L"A failed lower scan must give no entries, not the upper's entries alone");
    }

    TEST_METHOD(MergeDirectoryEntries_UpperDirUnreadable_ReturnsTheScanFailure) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"sub\\up.txt", "upper");
        env.WriteFile(env.Upper(), L"sub\\.wh.gone.txt", "");
        env.WriteFile(env.Lower(0), L"sub\\gone.txt", "lower0");
        env.WriteFile(env.Lower(0), L"sub\\below.txt", "lower0");
        DirectoryListingDenied denied(env.Upper() + L"\\sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        BackupPrivilegeDisabledOnThread noBackupPrivilege;
        AssertListingDenied(env.Upper() + L"\\sub");

        const MergedDirectory merged = mount.MergeDirectoryEntries(L"sub");

        Assert::AreEqual(static_cast<long>(STATUS_ACCESS_DENIED), static_cast<long>(merged.status),
            L"A failed upper scan must report the scan's status");
        Assert::IsTrue(merged.entries.empty(),
            L"An unreadable upper can hold whiteouts, so a failed upper scan must give no entries");
    }

    TEST_METHOD(MergeDirectoryEntries_FileInLowerAtListedDir_ShowsDeeperLowersEntries) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L"sub", "lower0");
        env.WriteFile(env.Lower(1), L"sub\\below.txt", "lower1");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        auto merged = mount.MergeDirectoryEntries(L"sub").entries;

        Assert::IsTrue(merged.count(L"below.txt") == 1,
            L"A file in a lower where the listed directory should be must not hide the deeper lower's entry");
    }

    TEST_METHOD(MergeDirectoryEntries_WhiteoutsInLower_HideTheirNamesInDeeperLower) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L"sub\\.wh.a.txt", "");
        env.WriteFile(env.Lower(0), L"sub\\.wh.b.txt", "");
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
        env.WriteFile(env.Lower(0), L".wh.a.txt", "");
        env.WriteFile(env.Lower(0), L"own.txt", "lower0");
        env.WriteFile(env.Lower(1), L"a.txt", "lower1");
        env.WriteFile(env.Lower(1), L"deep.txt", "lower1");
        auto config = env.MakeConfig();
        config.lowerPaths[0] = L"\\\\?\\" + env.Lower(0);
        ::LayerMount::LayerMount mount(config);

        auto merged = mount.MergeDirectoryEntries(L"").entries;

        Assert::IsTrue(merged.count(L"own.txt") == 1,
            L"The listing must show the entry at the root of the extended-form lower");
        Assert::IsTrue(merged.count(L"a.txt") == 0,
            L"The whiteout at the root of the extended-form lower must hide a.txt below it");
        Assert::IsTrue(merged.count(L"deep.txt") == 1,
            L"The listing must show the deeper lower's entry that no whiteout hides");
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

        Assert::IsTrue(wm.SetOpaque(L"sub"));

        std::wstring dirPath = env.Upper() + L"\\sub";
        Assert::IsTrue(MetadataADS::HasOpaqueADS(dirPath, nullptr),
            L"SetOpaque should write :overlay.opaque ADS");
    }

    TEST_METHOD(SetOpaque_WritesDotWhOpqMarker) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.SetOpaque(L"sub");

        std::wstring marker = env.Upper() + L"\\sub\\.wh..wh..opq";
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(marker.c_str()),
            L"SetOpaque should create .wh..wh..opq sentinel file");
    }

    TEST_METHOD(IsOpaque_ADSOnly_ReturnsTrue) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");

        std::wstring dirPath = env.Upper() + L"\\sub";
        Assert::IsTrue(MetadataADS::SetOpaqueADS(dirPath, nullptr));

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsTrue(wm.IsOpaque(L"sub"));
    }

    TEST_METHOD(IsOpaque_MarkerFileOnly_ReturnsTrue) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");

        std::wstring marker = env.Upper() + L"\\sub\\.wh..wh..opq";
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
        MetadataADS::SetOpaqueADS(upperDir, nullptr);

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
        Assert::IsFalse(MetadataADS::HasOpaqueADS(dirPath, nullptr), L"the opaque marker is gone");

        std::wstring marker = env.Upper() + L"\\sub\\.wh..wh..opq";
        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(marker.c_str()),
            L"Sentinel file should be removed");
    }

    TEST_METHOD(HasOpaqueAncestor_ParentOpaque_ReturnsTrue) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.SetOpaque(L"sub");
        Assert::IsTrue(wm.HasOpaqueAncestor(L"sub\\child.txt"));
    }

    TEST_METHOD(HasOpaqueAncestor_NoAncestorOpaque_ReturnsFalse) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsFalse(wm.HasOpaqueAncestor(L"sub\\child.txt"));
    }

    TEST_METHOD(HasOpaqueSelfOrAncestorInLayer_DirOpaqueInLayer_ReturnsTrue) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\.wh..wh..opq", "");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsTrue(wm.HasOpaqueSelfOrAncestorInLayer(L"d", env.Lower(0)));
    }

    TEST_METHOD(HasOpaqueSelfOrAncestorInLayer_AncestorOpaqueInLayer_ReturnsTrue) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a\\.wh..wh..opq", "");
        env.CreateDir(env.Lower(0), L"a\\b\\c");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsTrue(wm.HasOpaqueSelfOrAncestorInLayer(L"a\\b\\c", env.Lower(0)));
    }

    TEST_METHOD(HasOpaqueSelfOrAncestorInLayer_OpaqueOnlyInOtherLayer_ReturnsFalse) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"a\\.wh..wh..opq", "");
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

    TEST_METHOD(HasOpaqueAncestor_GrandparentOpaque_ReturnsTrue) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"a\\b\\c");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.SetOpaque(L"a");
        Assert::IsTrue(wm.HasOpaqueAncestor(L"a\\b\\c\\deep.txt"));
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
        env.WriteFile(env.Lower(0), L"sub\\.wh..wh..opq", "");
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
        env.WriteFile(env.Lower(1), L"sub\\.wh..wh..opq", "");
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
        env.WriteFile(env.Lower(0), L"sub\\.wh..wh..opq", "");
        env.WriteFile(env.Lower(0), L"sub\\.wh.gone.txt", "");
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
        env.WriteFile(env.Lower(0), L"sub\\.wh..wh..opq", "");
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
        env.WriteFile(env.Lower(0), L"d\\.wh..wh..opq", "");
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
        env.WriteFile(env.Upper(), L"d\\.wh..wh..opq", "");
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
        env.WriteFile(env.Lower(0), L"sub\\.wh..wh..opq", "");
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
        env.WriteFile(env.Lower(0), L"sub\\.wh..wh..opq", "");
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
        env.WriteFile(env.Lower(0), L"sub\\.wh..wh..opq", "");
        env.WriteFile(env.Lower(0), L"sub\\.wh.gone.txt", "");
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
        env.WriteFile(env.Lower(0), L"a\\.wh..wh..opq", "");
        env.WriteFile(env.Lower(0), L"a\\.wh.b", "");
        env.WriteFile(env.Lower(0), L"a\\b\\c.txt", "lower0");
        env.WriteFile(env.Lower(1), L"a\\b\\c.txt", "lower1");

        ResolverUnderTest r(env);

        Assert::IsFalse(r.resolver.ResolvePath(L"a\\b\\c.txt").Found(),
            L"The whiteout for the ancestor must hide the descendant in its own lower and in the lower below");
        Assert::IsFalse(r.resolver.ResolveLowerPath(L"a\\b\\c.txt").Found(),
            L"The whiteout for the ancestor must hide the descendant in its own lower and in the lower below");
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
        const PSECURITY_DESCRIPTOR noSecurityDescriptor = nullptr;
        const UINT64 noAllocationSize = 0;
        const DWORD untrackedCallerPid = 0;

        for (const wchar_t* dir : {L"sub", L"sub\\.overlay"}) {
            std::unique_ptr<FileContext> ctx;
            InternalFileInfo info{};
            Assert::IsTrue(NT_SUCCESS(mount.Create(dir, FILE_DIRECTORY_FILE,
                                                   FILE_ALL_ACCESS, FILE_ATTRIBUTE_DIRECTORY,
                                                   noSecurityDescriptor, noAllocationSize,
                                                   untrackedCallerPid, &ctx, &info)),
                (std::wstring(L"The directory create must succeed: ") + dir).c_str());
            mount.Close(ctx.get());
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

    static constexpr UINT32 kNoCreateOptions = 0u;
    static constexpr DWORD kNoCallerPid = 0u;
    static constexpr UINT64 kNoAllocationSize = 0u;
    static constexpr BOOLEAN kFailIfExists = FALSE;
    static constexpr PSECURITY_DESCRIPTOR kDefaultSecurity = nullptr;

    static NTSTATUS OpenThroughMount(::LayerMount::LayerMount& mount,
                                     const std::wstring& path) {
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        const NTSTATUS status = mount.Open(path, FILE_READ_DATA,
                                           kNoCreateOptions, kNoCallerPid,
                                           &ctx, &info);
        if (ctx) mount.Close(ctx.get());
        return status;
    }

    static NTSTATUS CreateThroughMount(::LayerMount::LayerMount& mount,
                                       const std::wstring& path,
                                       UINT32 createOptions) {
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        const UINT32 attributes = (createOptions & FILE_DIRECTORY_FILE) != 0
            ? FILE_ATTRIBUTE_DIRECTORY
            : FILE_ATTRIBUTE_NORMAL;
        const NTSTATUS status = mount.Create(path, createOptions,
                                             FILE_ALL_ACCESS, attributes,
                                             kDefaultSecurity, kNoAllocationSize,
                                             kNoCallerPid, &ctx, &info);
        if (ctx) mount.Close(ctx.get());
        return status;
    }

    TEST_METHOD(Open_WhiteoutMarkerInUpper_IsNotFound) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"sub\\.wh.name", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_OBJECT_NAME_NOT_FOUND),
                         static_cast<long>(OpenThroughMount(mount, L"sub\\.wh.name")),
                         L"Opening a whiteout marker in the upper must report not found");
    }

    TEST_METHOD(Open_OpaqueMarkerInLower_IsNotFound) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\.wh..wh..opq", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_OBJECT_NAME_NOT_FOUND),
                         static_cast<long>(OpenThroughMount(mount, L"sub\\.wh..wh..opq")),
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

        Assert::AreEqual(static_cast<long>(STATUS_SUCCESS), static_cast<long>(merged.status),
            L"A marker-named directory is a reserved path, not a failed scan");
        Assert::IsTrue(merged.entries.empty(),
            L"A marker-named directory must list no entries");
    }

    TEST_METHOD(Create_WhiteoutMarkerName_IsDeniedAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        env.WriteFile(env.Lower(0), L"sub\\name", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_ACCESS_DENIED),
                         static_cast<long>(CreateThroughMount(mount, L"sub\\.wh.name", kNoCreateOptions)),
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

        Assert::AreEqual(static_cast<long>(STATUS_ACCESS_DENIED),
                         static_cast<long>(CreateThroughMount(mount, L"sub\\.wh..wh..opq", kNoCreateOptions)),
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

        Assert::AreEqual(static_cast<long>(STATUS_ACCESS_DENIED),
                         static_cast<long>(CreateThroughMount(mount, L"sub\\.WH.name", kNoCreateOptions)),
                         L"Creating a marker name spelled in upper case must be denied");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\.wh.name"),
            L"A denied create must write no marker in the upper");
    }

    TEST_METHOD(Create_MarkerNameWithStream_IsDenied) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_ACCESS_DENIED),
                         static_cast<long>(CreateThroughMount(mount, L"sub\\.wh.name:stream", kNoCreateOptions)),
                         L"Creating a stream on a marker name must be denied");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\.wh.name"),
            L"A denied create must write no marker in the upper");
    }

    TEST_METHOD(Create_MarkerNamedDirectory_IsDenied) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_ACCESS_DENIED),
                         static_cast<long>(CreateThroughMount(mount, L"sub\\.wh.dir", FILE_DIRECTORY_FILE)),
                         L"Creating a directory with a marker name must be denied");
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\.wh.dir"),
            L"A denied create must write no directory in the upper");
    }

    TEST_METHOD(Rename_ToMarkerName_IsDeniedAndLeavesSource) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"sub\\a.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_ACCESS_DENIED),
                         static_cast<long>(mount.Rename(L"sub\\a.txt", L"sub\\.wh.a.txt",
                                                        kFailIfExists, kNoCallerPid)),
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

        Assert::AreEqual(static_cast<long>(STATUS_ACCESS_DENIED), static_cast<long>(status),
            L"Renaming an open file onto a marker name must be denied");
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

        Assert::AreEqual(static_cast<long>(STATUS_INVALID_PARAMETER), static_cast<long>(status),
            L"Moving an open context onto a marker name must be rejected");
        Assert::AreEqual(std::wstring(L"sub\\a.txt"), pathAfter,
            L"A rejected update must leave the context path unchanged");
    }

    TEST_METHOD(Delete_WhiteoutMarkerInUpper_IsNotFoundAndLeavesMarker) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\gone.txt", "lower");
        env.WriteFile(env.Upper(), L"sub\\.wh.gone.txt", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_OBJECT_NAME_NOT_FOUND),
                         static_cast<long>(mount.Delete(L"sub\\.wh.gone.txt", kNoCallerPid)),
                         L"Deleting a whiteout marker must report not found");
        Assert::IsTrue(env.FileExists(env.Upper(), L"sub\\.wh.gone.txt"),
            L"A refused delete must leave the marker in place");
        Assert::AreEqual(static_cast<long>(STATUS_OBJECT_NAME_NOT_FOUND),
                         static_cast<long>(OpenThroughMount(mount, L"sub\\gone.txt")),
                         L"A refused delete must keep the lower file hidden");
    }

    TEST_METHOD(Rename_FromMarkerName_IsDeniedAndLeavesMarker) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\gone.txt", "lower");
        env.WriteFile(env.Upper(), L"sub\\.wh.gone.txt", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(static_cast<long>(STATUS_ACCESS_DENIED),
                         static_cast<long>(mount.Rename(L"sub\\.wh.gone.txt", L"sub\\back.txt",
                                                        kFailIfExists, kNoCallerPid)),
                         L"Renaming a marker must be denied");
        Assert::IsTrue(env.FileExists(env.Upper(), L"sub\\.wh.gone.txt"),
            L"A denied rename must leave the marker in place");
        Assert::AreEqual(static_cast<long>(STATUS_OBJECT_NAME_NOT_FOUND),
                         static_cast<long>(OpenThroughMount(mount, L"sub\\gone.txt")),
                         L"A denied rename must keep the lower file hidden");
    }
};

}
