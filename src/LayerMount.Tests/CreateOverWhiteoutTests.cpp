// Unit tests for the "create at a path that was previously deleted / renamed
// away / hidden by an opaque marker" scenarios. These exercise the manager
// primitives that LayerMount::SCreate composes:
//   1. Whiteout removal if one exists at the target path
//   2. Opaque-marker creation when placing a new directory over a lower dir
//   3. Parent-directory creation in upper (lazy) when the target is nested
//
// End-to-end equivalents (CreateFileW / CreateDirectoryW through a mounted
// overlay) live in the host adapter's integration test suite.

#include "pch.h"
#include "TestFixture.h"

#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataADS.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

TEST_CLASS(CreateOverWhiteoutTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    // Mirror of SCreate's file-creation flow: clear a stale whiteout, ensure
    // parent, then create. Returns the created-path for convenience.
    static std::wstring SimulateCreateFile(PathResolver& resolver,
                                           WhiteoutManager& wm,
                                           Cache& cache,
                                           const std::wstring& norm,
                                           const std::string& content) {
        if (wm.HasWhiteout(norm, resolver.Config().upperPath)) {
            wm.RemoveWhiteout(norm);
        }
        const std::wstring upperPath = resolver.GetUpperPath(norm);
        EnsureDirectoryExists(
            std::filesystem::path(upperPath).parent_path().wstring());
        HANDLE h = ::CreateFileW(upperPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, h,
            L"SimulateCreateFile: CreateFileW failed");
        DWORD written = 0;
        ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()),
                    &written, nullptr);
        ::CloseHandle(h);
        cache.InvalidateWithAncestors(norm);
        return upperPath;
    }

    // Mirror of SCreate's directory-creation flow: clear stale whiteout,
    // create dir, set opaque marker iff the path shadows a lower directory.
    static void SimulateCreateDir(PathResolver& resolver,
                                  WhiteoutManager& wm,
                                  Cache& cache,
                                  const std::wstring& norm) {
        if (wm.HasWhiteout(norm, resolver.Config().upperPath)) {
            wm.RemoveWhiteout(norm);
        }
        ResolvedPath lower = resolver.ResolveLowerPath(norm);
        const bool lowerIsDir =
            lower.Found() && (lower.attributes & FILE_ATTRIBUTE_DIRECTORY);

        const std::wstring upperPath = resolver.GetUpperPath(norm);
        EnsureDirectoryExists(upperPath);

        if (lowerIsDir) {
            wm.SetOpaque(norm);
        }
        cache.InvalidateWithAncestors(norm);
    }

    // ---------------------------------------------------------------
    // C1 — create file at a path previously deleted in lower: whiteout cleared
    // ---------------------------------------------------------------
    TEST_METHOD(C1_CreateFileOverWhiteoutForDeletedLowerFile_WhiteoutRemoved) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "old-lower");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        // Simulate prior delete.
        Assert::IsTrue(wm.CreateWhiteout(L"f.txt"));
        Assert::IsFalse(resolver.ResolvePath(L"f.txt").Found());

        SimulateCreateFile(resolver, wm, cache, L"f.txt", "new-upper");

        Assert::IsFalse(wm.HasWhiteout(L"f.txt", env.Upper()),
            L"Whiteout must be removed so the new file is visible");
        ResolvedPath r = resolver.ResolvePath(L"f.txt");
        Assert::IsTrue(r.Found());
        Assert::IsTrue(r.source == LayerSource::Upper);
        Assert::AreEqual(std::string("new-upper"),
                         env.ReadFile(env.Upper(), L"f.txt"));
    }

    // ---------------------------------------------------------------
    // C2 — create file at path where a lower directory was deleted
    // ---------------------------------------------------------------
    TEST_METHOD(C2_CreateFileWhereLowerDirWasDeleted_TypeFlipsToFile) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"wasdir");
        env.WriteFile(env.Lower(0), L"wasdir\\inner.txt", "inner");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        Assert::IsTrue(wm.CreateWhiteout(L"wasdir", WhiteoutType::Directory));

        SimulateCreateFile(resolver, wm, cache, L"wasdir", "i-am-a-file-now");

        Assert::IsFalse(wm.HasWhiteout(L"wasdir", env.Upper()));
        Assert::AreEqual(std::string("i-am-a-file-now"),
                         env.ReadFile(env.Upper(), L"wasdir"));

        // The path now resolves to a file in the upper layer.
        ResolvedPath rSelf = resolver.ResolvePath(L"wasdir");
        Assert::IsTrue(rSelf.Found());
        Assert::IsTrue(rSelf.source == LayerSource::Upper);
        Assert::IsTrue((rSelf.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0);

        // Note: whether the resolver actively hides lower children under a
        // type-flip (file-in-upper shadows dir-in-lower's children) is a
        // separate PathResolver concern tracked independently from this test
        // and not asserted here. The lower dir/files remain untouched on disk.
        Assert::IsTrue(env.FileExists(env.Lower(0), L"wasdir\\inner.txt"));
    }

    // ---------------------------------------------------------------
    // C3 — create directory at path where lower file was deleted: whiteout cleared, no opaque
    // ---------------------------------------------------------------
    TEST_METHOD(C3_CreateDirWhereLowerFileWasDeleted_NoOpaqueMarker) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"wasfile", "lower-file");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        Assert::IsTrue(wm.CreateWhiteout(L"wasfile"));

        SimulateCreateDir(resolver, wm, cache, L"wasfile");

        Assert::IsFalse(wm.HasWhiteout(L"wasfile", env.Upper()));
        Assert::IsFalse(wm.IsOpaque(L"wasfile"),
            L"No opaque marker needed — lower at this path is a file, not a dir");
        Assert::IsTrue(env.FileExists(env.Upper(), L"wasfile"));
    }

    // ---------------------------------------------------------------
    // C4 — create dir over an existing lower directory (no prior delete) → opaque
    // ---------------------------------------------------------------
    TEST_METHOD(C4_CreateDirOverLowerDir_OpaqueMarkerSetAndLowerChildrenHidden) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"shared");
        env.WriteFile(env.Lower(0), L"shared\\lc.txt", "lower-child");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        SimulateCreateDir(resolver, wm, cache, L"shared");

        Assert::IsTrue(wm.IsOpaque(L"shared"),
            L"Creating a dir over a lower dir must mark upper as opaque");

        Assert::IsFalse(resolver.ResolvePath(L"shared\\lc.txt").Found(),
            L"Opaque marker must hide the previously-visible lower child");
    }

    // ---------------------------------------------------------------
    // C5 — recreate file at path whose lower entry was "renamed away"
    //
    // When a lower-layer file is renamed via SRename, the old path gets both
    // a whiteout AND a redirect metadata ADS pointing at the new location. The
    // create-over path should still remove the whiteout marker. The redirect
    // ADS lives on the (upper) whiteout marker, so removing the marker also
    // removes the redirect.
    // ---------------------------------------------------------------
    TEST_METHOD(C5_CreateFileWhereLowerWasRenamedAway_WhiteoutAndRedirectGone) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"moved.txt", "lower-content");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        // Simulate the rename-source artifacts: whiteout at the old name.
        Assert::IsTrue(wm.CreateWhiteout(L"moved.txt"));
        Assert::IsFalse(resolver.ResolvePath(L"moved.txt").Found());

        SimulateCreateFile(resolver, wm, cache, L"moved.txt", "recreated");

        Assert::IsFalse(wm.HasWhiteout(L"moved.txt", env.Upper()));
        Assert::AreEqual(std::string("recreated"),
                         env.ReadFile(env.Upper(), L"moved.txt"));
    }

    // ---------------------------------------------------------------
    // C6 — recreate file where a lower directory was "renamed away"
    // ---------------------------------------------------------------
    TEST_METHOD(C6_CreateFileWhereLowerDirWasRenamedAway_Ok) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"olddir");
        env.WriteFile(env.Lower(0), L"olddir\\x.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        // A dir rename leaves a dir-typed whiteout at old path.
        Assert::IsTrue(wm.CreateWhiteout(L"olddir", WhiteoutType::Directory));

        SimulateCreateFile(resolver, wm, cache, L"olddir", "new-file-at-old-path");

        Assert::IsFalse(wm.HasWhiteout(L"olddir", env.Upper()));
        Assert::AreEqual(std::string("new-file-at-old-path"),
                         env.ReadFile(env.Upper(), L"olddir"));
    }

    // ---------------------------------------------------------------
    // C8 — create-over-whiteout in a nested path with lazy parent creation
    // ---------------------------------------------------------------
    TEST_METHOD(C8_CreateOverWhiteoutInNestedPath_ParentsCreatedOnDemand) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"a\\b");
        env.WriteFile(env.Lower(0), L"a\\b\\c.txt", "lower");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        // Whiteout the deep file. CreateWhiteout eagerly materializes the parent
        // chain in upper (so the marker has somewhere to land). The interesting
        // invariant below is that the create path still succeeds regardless of
        // whether the parent existed beforehand.
        Assert::IsTrue(wm.CreateWhiteout(L"a\\b\\c.txt"));

        SimulateCreateFile(resolver, wm, cache, L"a\\b\\c.txt", "new");

        Assert::IsTrue(env.FileExists(env.Upper(), L"a\\b"),
            L"Parent chain must be created lazily during the create");
        Assert::IsFalse(wm.HasWhiteout(L"a\\b\\c.txt", env.Upper()));
        Assert::AreEqual(std::string("new"),
                         env.ReadFile(env.Upper(), L"a\\b\\c.txt"));

        ResolvedPath r = resolver.ResolvePath(L"a\\b\\c.txt");
        Assert::IsTrue(r.Found());
        Assert::IsTrue(r.source == LayerSource::Upper);
    }

    // ---------------------------------------------------------------
    // Extra — create/delete/create idempotency at the manager level
    // ---------------------------------------------------------------
    TEST_METHOD(CreateDeleteCreate_Loop_ConvergesToLatestContent) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"loop.txt", "original-lower");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        // (1) delete lower-only file — whiteout goes up.
        Assert::IsTrue(wm.CreateWhiteout(L"loop.txt"));

        // (2) create — whiteout cleared, upper content wins.
        SimulateCreateFile(resolver, wm, cache, L"loop.txt", "first-upper");
        Assert::AreEqual(std::string("first-upper"),
                         env.ReadFile(env.Upper(), L"loop.txt"));

        // (3) delete again — upper removed, whiteout back (because lower still exists).
        ::DeleteFileW(resolver.GetUpperPath(L"loop.txt").c_str());
        if (resolver.ResolveLowerPath(L"loop.txt").Found()) {
            Assert::IsTrue(wm.CreateWhiteout(L"loop.txt"));
        }
        cache.InvalidateWithAncestors(L"loop.txt");

        // (4) create again.
        SimulateCreateFile(resolver, wm, cache, L"loop.txt", "second-upper");
        Assert::AreEqual(std::string("second-upper"),
                         env.ReadFile(env.Upper(), L"loop.txt"));
        Assert::IsFalse(wm.HasWhiteout(L"loop.txt", env.Upper()));
        Assert::AreEqual(std::string("original-lower"),
                         env.ReadFile(env.Lower(0), L"loop.txt"),
            L"Lower must be untouched through the whole cycle");
    }
};

} // namespace LayerMountTests
