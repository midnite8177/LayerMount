#include "pch.h"
#include "TestFixture.h"

#include "WhiteoutManager.h"
#include "PathResolver.h"
#include "Cache.h"
#include "MetadataADS.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

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

    // --- Static helper tests ---

    TEST_METHOD(IsWhiteoutName_WithPrefix_ReturnsTrue) {
        Assert::IsTrue(WhiteoutManager::IsWhiteoutName(L".wh.foo.txt"));
    }

    TEST_METHOD(IsWhiteoutName_NoPrefix_ReturnsFalse) {
        Assert::IsFalse(WhiteoutManager::IsWhiteoutName(L"foo.txt"));
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

    // --- CreateWhiteout / HasWhiteout ---

    TEST_METHOD(CreateWhiteout_File_ProducesHiddenSystemMarkerInUpper) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        Assert::IsTrue(wm.CreateWhiteout(L"foo.txt"));

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

        Assert::IsTrue(wm.CreateWhiteout(L"sub\\foo.txt"));

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

        wm.CreateWhiteout(L"foo.txt");
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

        // Hand-place a whiteout marker in the lower layer
        std::wstring lowerWh = env.Lower(0) + L"\\.wh.foo.txt";
        HANDLE h = ::CreateFileW(lowerWh.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h);

        Assert::IsTrue(wm.HasWhiteoutInAnyLayer(L"foo.txt"));
    }

    // --- RemoveWhiteout ---

    TEST_METHOD(RemoveWhiteout_ExistingMarker_DeletesAndReturnsTrue) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.CreateWhiteout(L"foo.txt");
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

    // --- Cache invalidation ---

    TEST_METHOD(CreateWhiteout_InvalidatesCache) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        // Pre-populate cache with a resolved path
        ResolvedPath fake;
        fake.absolutePath = env.Lower(0) + L"\\foo.txt";
        fake.source = LayerSource::Lower;
        fake.lowerIndex = 0;
        cache.Put(L"foo.txt", fake);
        Assert::IsTrue(cache.Get(L"foo.txt").has_value(), L"Cache should have entry before whiteout");

        wm.CreateWhiteout(L"foo.txt");

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

        // Before whiteout: resolves to lower
        ResolvedPath before = resolver.ResolvePath(L"secret.txt");
        Assert::IsTrue(before.Found());
        Assert::IsTrue(before.source == LayerSource::Lower);

        // Whiteout invalidates the cache entry
        wm.CreateWhiteout(L"secret.txt");

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

        wm.CreateWhiteout(L"secret.txt");
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

    TEST_METHOD(ListWhiteoutsInDirectory_ReturnsOriginalNamesWithPrefixStripped) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.CreateWhiteout(L"a.txt");
        wm.CreateWhiteout(L"b.txt");
        wm.CreateWhiteout(L"c.txt");

        auto whiteouts = wm.ListWhiteoutsInDirectory(L"", env.Upper());

        auto contains = [&](const std::wstring& name) {
            return std::find(whiteouts.begin(), whiteouts.end(), name) != whiteouts.end();
        };
        Assert::AreEqual(size_t{3}, whiteouts.size());
        Assert::IsTrue(contains(L"a.txt"));
        Assert::IsTrue(contains(L"b.txt"));
        Assert::IsTrue(contains(L"c.txt"));
    }

    TEST_METHOD(ListWhiteoutsInDirectory_RootOfExtendedFormLayerPath_ScansSuccessfully) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.CreateWhiteout(L"a.txt");

        std::wstring extendedUpper = L"\\\\?\\" + env.Upper();
        bool ok = false;
        auto whiteouts = wm.ListWhiteoutsInDirectory(L"", extendedUpper, &ok);

        Assert::IsTrue(ok, L"Root scan of an extended-form layer path should succeed");
        Assert::AreEqual(size_t{1}, whiteouts.size());
        Assert::AreEqual(std::wstring(L"a.txt"), whiteouts[0]);
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

        // Drop only the sentinel file (no ADS)
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

        // Set opaque on upper/sub, NOT on lower/sub
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
        auto merged = mount.MergeDirectoryEntries(L"sub");

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
        auto merged = mount.MergeDirectoryEntries(L"sub");

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
        auto merged = mount.MergeDirectoryEntries(L"sub");

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
        auto merged = mount.MergeDirectoryEntries(L"sub");

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
        auto merged = mount.MergeDirectoryEntries(L"d\\sub");

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
        auto merged = mount.MergeDirectoryEntries(L"d\\sub");

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
        auto merged = mount.MergeDirectoryEntries(L"sub");

        Assert::IsTrue(merged.count(L".overlay") == 1,
            L"A user's .overlay directory below the root must show in the listing");
        AssertEveryListedEntryResolves(env, L"sub", merged);
    }

    TEST_METHOD(MergeDirectoryEntries_SidecarNamedDirBelowRootInLower_IsListed) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"sub\\.overlay");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"sub");

        Assert::IsTrue(merged.count(L".overlay") == 1,
            L"A lower's .overlay directory below the root must show in the listing");
        AssertEveryListedEntryResolves(env, L"sub", merged);
    }

    TEST_METHOD(MergeDirectoryEntries_SidecarNamedDirCreatedBelowRoot_IsListed) {
        TempLayerEnvironment env(1);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        for (const wchar_t* dir : {L"sub", L"sub\\.overlay"}) {
            std::unique_ptr<FileContext> ctx;
            InternalFileInfo info{};
            Assert::IsTrue(NT_SUCCESS(mount.Create(dir, FILE_DIRECTORY_FILE,
                                                   FILE_ALL_ACCESS, FILE_ATTRIBUTE_DIRECTORY,
                                                   /*securityDescriptor*/ nullptr,
                                                   /*allocationSize*/ 0u,
                                                   /*callerPid*/ 0u, &ctx, &info)),
                (std::wstring(L"The directory create must succeed: ") + dir).c_str());
            mount.Close(ctx.get());
        }

        auto merged = mount.MergeDirectoryEntries(L"sub");

        Assert::IsTrue(merged.count(L".overlay") == 1,
            L"A .overlay directory that the engine created below the root must show in the listing");
    }

    TEST_METHOD(MergeDirectoryEntries_SidecarDirAtRoot_IsNotListed) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L".overlay");
        env.CreateDir(env.Lower(0), L".overlay");
        env.WriteFile(env.Upper(), L"visible.txt", "v");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"");

        Assert::IsTrue(merged.count(L".overlay") == 0,
            L"The sidecar directory at the root must not show in the listing");
        Assert::IsTrue(merged.count(L"visible.txt") == 1,
            L"The listing of the root must show the upper's file");
    }
};

}
