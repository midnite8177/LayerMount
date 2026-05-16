// Unit tests for WhiteoutManager. Test the class in isolation — do NOT mount
// an overlay, do NOT invoke host-adapter callbacks. The "directory listing
// excludes .wh.* files" merged-view behavior is covered by
// LayerMount::MergeDirectoryEntries integration tests — this file
// covers the primitives used there.

#include "pch.h"
#include "TestFixture.h"

#include "WhiteoutManager.h"
#include "PathResolver.h"
#include "Cache.h"
#include "MetadataADS.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

// ============================================================================
// 9.3 — Whiteout primitives
// ============================================================================

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

    // --- Integration with PathResolver (whiteout hides lower file) ---

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

    // --- Directory enumeration ---

    TEST_METHOD(ListWhiteoutsInDirectory_ReturnsOriginalNamesWithPrefixStripped) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);

        wm.CreateWhiteout(L"a.txt");
        wm.CreateWhiteout(L"b.txt");
        wm.CreateWhiteout(L"c.txt");

        auto whiteouts = wm.ListWhiteoutsInDirectory(L"", env.Upper());

        // Verify all three original names are present (regardless of order)
        auto contains = [&](const std::wstring& name) {
            return std::find(whiteouts.begin(), whiteouts.end(), name) != whiteouts.end();
        };
        Assert::AreEqual(size_t{3}, whiteouts.size());
        Assert::IsTrue(contains(L"a.txt"));
        Assert::IsTrue(contains(L"b.txt"));
        Assert::IsTrue(contains(L"c.txt"));
    }
};

// ============================================================================
// 9.4 — Opaque directory support
// ============================================================================

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
        Assert::IsTrue(MetadataADS::HasOpaqueADS(dirPath),
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
        Assert::IsTrue(MetadataADS::SetOpaqueADS(dirPath));

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
        MetadataADS::SetOpaqueADS(upperDir);

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
        Assert::IsFalse(MetadataADS::HasOpaqueADS(dirPath), L"ADS should be removed");

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
};

} // namespace LayerMountTests
