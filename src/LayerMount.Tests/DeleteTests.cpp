// Unit tests for delete coordination across overlay layers. These exercise
// the manager primitives that LayerMount::SCleanup (delete-on-close) and
// SCanDelete compose: DeleteFileW/RemoveDirectoryW, conditional whiteout
// creation based on lower-layer presence, and MergeDirectoryEntries for the
// CanDelete emptiness check.
//
// End-to-end equivalents (DeleteFileW / RemoveDirectoryW through a mounted
// overlay) live in the host-adapter integration test suites.

#include "pch.h"
#include "TestFixture.h"

#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "CopyUp.h"
#include "MetadataADS.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

TEST_CLASS(DeleteTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    // Mirror of the file-delete flow in SCleanup: physically remove the upper
    // copy (if present), then create a whiteout IFF the path still exists in
    // some lower layer. Cache invalidation matches SCleanup.
    static void SimulateFileDelete(PathResolver& resolver,
                                   WhiteoutManager& wm,
                                   Cache& cache,
                                   const std::wstring& norm) {
        if (resolver.ExistsInUpper(norm)) {
            ::DeleteFileW(resolver.GetUpperPath(norm).c_str());
        }
        if (resolver.ResolveLowerPath(norm).Found()) {
            wm.CreateWhiteout(norm, WhiteoutType::File);
        }
        cache.InvalidateWithAncestors(norm);
    }

    static void SimulateDirDelete(PathResolver& resolver,
                                  WhiteoutManager& wm,
                                  Cache& cache,
                                  const std::wstring& norm) {
        if (resolver.ExistsInUpper(norm)) {
            // Clean out any opaque marker the dir is carrying so the
            // subsequent RemoveDirectoryW can actually succeed. SCleanup's
            // delete-on-close path needs the same preamble — tracked as a
            // separate follow-up against the real callback.
            if (wm.IsOpaque(norm)) {
                wm.RemoveOpaque(norm);
            }
            ::RemoveDirectoryW(resolver.GetUpperPath(norm).c_str());
        }
        if (resolver.ResolveLowerPath(norm).Found()) {
            wm.CreateWhiteout(norm, WhiteoutType::Directory);
        }
        cache.InvalidateWithAncestors(norm);
    }

    // ---------------------------------------------------------------
    // D1 — delete upper-only file: no whiteout needed
    // ---------------------------------------------------------------
    TEST_METHOD(D1_DeleteUpperOnlyFile_NoWhiteoutCreated) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"only.txt", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        SimulateFileDelete(resolver, wm, cache, L"only.txt");

        Assert::IsFalse(env.FileExists(env.Upper(), L"only.txt"));
        Assert::IsFalse(wm.HasWhiteout(L"only.txt", env.Upper()),
            L"No whiteout should be created when nothing existed in lower");
        Assert::IsFalse(resolver.ResolvePath(L"only.txt").Found());
    }

    // ---------------------------------------------------------------
    // D2 — delete lower-only file: whiteout created so the lower file hides
    // ---------------------------------------------------------------
    TEST_METHOD(D2_DeleteLowerOnlyFile_WhiteoutCreated) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"ghost.txt", "lower data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        SimulateFileDelete(resolver, wm, cache, L"ghost.txt");

        // Lower untouched, whiteout present, resolver returns "not found".
        Assert::IsTrue(env.FileExists(env.Lower(0), L"ghost.txt"));
        Assert::IsTrue(wm.HasWhiteout(L"ghost.txt", env.Upper()));

        ResolvedPath r = resolver.ResolvePath(L"ghost.txt");
        Assert::IsFalse(r.Found());
        Assert::IsTrue(r.isWhiteout);
    }

    // ---------------------------------------------------------------
    // D3 — delete shadowed file (upper + lower): upper removed, whiteout created
    // ---------------------------------------------------------------
    TEST_METHOD(D3_DeleteShadowedFile_UpperRemovedAndWhiteoutCreated) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"shared.txt", "lower");
        env.WriteFile(env.Upper(),  L"shared.txt", "upper");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        SimulateFileDelete(resolver, wm, cache, L"shared.txt");

        Assert::IsFalse(env.FileExists(env.Upper(), L"shared.txt"));
        Assert::IsTrue(env.FileExists(env.Lower(0), L"shared.txt"));
        Assert::IsTrue(wm.HasWhiteout(L"shared.txt", env.Upper()));
        Assert::IsFalse(resolver.ResolvePath(L"shared.txt").Found());
    }

    // ---------------------------------------------------------------
    // D4 — delete upper-only empty directory: no whiteout
    // ---------------------------------------------------------------
    TEST_METHOD(D4_DeleteUpperOnlyEmptyDir_NoWhiteoutCreated) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"empty");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        SimulateDirDelete(resolver, wm, cache, L"empty");

        Assert::IsFalse(env.FileExists(env.Upper(), L"empty"));
        Assert::IsFalse(wm.HasWhiteout(L"empty", env.Upper()));
    }

    // ---------------------------------------------------------------
    // D6 — CanDelete must refuse when upper is empty but lower still contributes children
    // ---------------------------------------------------------------
    TEST_METHOD(D6_DirEmptyInUpperButLowerChildren_MergedViewNotEmpty) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"mix");
        env.WriteFile(env.Lower(0), L"mix\\child.txt", "data");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"mix");

        Assert::IsFalse(merged.empty(),
            L"CanDelete merges across layers — merged view must still include child.txt");
        Assert::IsTrue(merged.count(L"child.txt") == 1);
    }

    // ---------------------------------------------------------------
    // D7 — directory where every lower child is whited-out: merged view IS empty
    // ---------------------------------------------------------------
    TEST_METHOD(D7_AllLowerChildrenWhitedOut_MergedViewEmpty_AllowsDelete) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"box");
        env.WriteFile(env.Lower(0), L"box\\a.txt", "a");
        env.WriteFile(env.Lower(0), L"box\\b.txt", "b");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        Assert::IsTrue(wm.CreateWhiteout(L"box\\a.txt"));
        Assert::IsTrue(wm.CreateWhiteout(L"box\\b.txt"));

        ::LayerMount::LayerMount mount(config);
        auto merged = mount.MergeDirectoryEntries(L"box");

        Assert::IsTrue(merged.empty(),
            L"With every lower child whited-out, the merged dir view is empty");
    }

    // ---------------------------------------------------------------
    // D8 — delete a shadowed directory: whiteout at the path hides lower subtree
    // ---------------------------------------------------------------
    TEST_METHOD(D8_DeleteShadowedDir_WhiteoutHidesLowerSubtree) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"sd");
        env.WriteFile(env.Lower(0), L"sd\\inner.txt", "inner-lower");
        env.CreateDir(env.Upper(),  L"sd");
        Assert::IsTrue(WhiteoutManager(env.MakeConfig()).SetOpaque(L"sd"));

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        // Upper is opaque + empty-of-its-own-children → merged view is empty, delete can proceed.
        ::LayerMount::LayerMount mount(config);
        Assert::IsTrue(mount.MergeDirectoryEntries(L"sd").empty());

        SimulateDirDelete(resolver, wm, cache, L"sd");

        Assert::IsFalse(env.FileExists(env.Upper(), L"sd"));
        Assert::IsTrue(wm.HasWhiteout(L"sd", env.Upper()));

        // Lower subtree remains invisible because the dir itself is whited-out.
        Assert::IsFalse(resolver.ResolvePath(L"sd\\inner.txt").Found());
    }

    // ---------------------------------------------------------------
    // D9 — delete lower-only directory: whiteout at path, nothing to remove in upper
    // ---------------------------------------------------------------
    TEST_METHOD(D9_DeleteLowerOnlyDir_WhiteoutCreatedOnly) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"lo_only");
        env.WriteFile(env.Lower(0), L"lo_only\\inner.txt", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        SimulateDirDelete(resolver, wm, cache, L"lo_only");

        // Nothing was created or deleted in upper (other than the whiteout marker itself).
        // Lower remains present, whiteout is in upper.
        Assert::IsTrue(env.FileExists(env.Lower(0), L"lo_only"));
        Assert::IsTrue(wm.HasWhiteout(L"lo_only", env.Upper()));
        Assert::IsFalse(resolver.ResolvePath(L"lo_only\\inner.txt").Found());
    }

    // ---------------------------------------------------------------
    // D-extra — deleting a file invalidates the cached resolution for it
    // ---------------------------------------------------------------
    TEST_METHOD(DeleteLowerFile_InvalidatesCachedResolution) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"cached.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        // Prime cache.
        Assert::IsTrue(resolver.ResolvePath(L"cached.txt").Found());
        Assert::IsTrue(cache.Get(L"cached.txt").has_value());

        SimulateFileDelete(resolver, wm, cache, L"cached.txt");
        Assert::IsFalse(cache.Get(L"cached.txt").has_value());
    }

    // ---------------------------------------------------------------
    // D-extra — whiteout created by delete does not itself surface in enumeration
    // ---------------------------------------------------------------
    TEST_METHOD(DeletedLowerFile_WhiteoutMarkerNotVisibleInMergedDir) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"visible.txt", "v");
        env.WriteFile(env.Lower(0), L"hide.txt",    "h");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        SimulateFileDelete(resolver, wm, cache, L"hide.txt");

        ::LayerMount::LayerMount mount(config);
        auto merged = mount.MergeDirectoryEntries(L"");

        Assert::IsTrue(merged.count(L"visible.txt") == 1,
            L"Other lower files should remain visible");
        Assert::IsTrue(merged.count(L"hide.txt") == 0,
            L"Whited-out lower file must not appear in merged listing");
        Assert::IsTrue(merged.count(L".wh.hide.txt") == 0,
            L"Whiteout marker file must itself be filtered from the listing");
    }
};

} // namespace LayerMountTests
