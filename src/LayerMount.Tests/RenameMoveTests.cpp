// Unit tests for rename / move coordination across overlay layers. These
// exercise the manager primitives that LayerMount::SRename composes at the
// host-adapter callback layer: CopyUp::CopyUpFile + MoveFileExW +
// WhiteoutManager for files, and CopyUp::HandleDirectoryRename for
// directories.
//
// The equivalent end-to-end tests through a mounted overlay live in the
// host-adapter integration test suites.

#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataADS.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

// ============================================================================
// RenameMoveTests — manager-level composition tests for rename/move
// ============================================================================

TEST_CLASS(RenameMoveTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    // Minimal re-implementation of the file-rename flow SRename uses for a
    // lower-only source file. Kept tight — same order of operations as the
    // callback so unit tests detect state-transition regressions even when the
    // real callback is out of reach.
    static NTSTATUS SimulateLowerFileRename(CopyUp& cu,
                                            PathResolver& resolver,
                                            WhiteoutManager& wm,
                                            Cache& cache,
                                            const std::wstring& oldNorm,
                                            const std::wstring& newNorm,
                                            bool replaceIfExists) {
        // 1. Clear any whiteout at the new path so the moved entry is visible.
        if (wm.HasWhiteout(newNorm, resolver.Config().upperPath)) {
            wm.RemoveWhiteout(newNorm);
        }

        // 2. Copy-up the source to upper at the OLD location.
        NTSTATUS st = cu.CopyUpFile(oldNorm);
        if (!NT_SUCCESS(st)) return st;

        // 3. Move within upper to the new location (create parent if needed).
        const std::wstring oldUpper = resolver.GetUpperPath(oldNorm);
        const std::wstring newUpper = resolver.GetUpperPath(newNorm);
        EnsureDirectoryExists(
            std::filesystem::path(newUpper).parent_path().wstring());

        const DWORD flags = replaceIfExists ? MOVEFILE_REPLACE_EXISTING : 0;
        if (!::MoveFileExW(oldUpper.c_str(), newUpper.c_str(), flags)) {
            return HRESULT_FROM_WIN32(::GetLastError());
        }

        // 4. Whiteout at the old location so the lower file stops surfacing.
        wm.CreateWhiteout(oldNorm, WhiteoutType::File);

        // 5. Invalidate cache for both paths + ancestors.
        cache.InvalidateWithAncestors(oldNorm);
        cache.InvalidateWithAncestors(newNorm);
        return STATUS_SUCCESS;
    }

    // ---------------------------------------------------------------
    // R3 — upper-only empty dir rename, opaque not set
    // ---------------------------------------------------------------
    TEST_METHOD(R3_UpperOnlyEmptyDirRename_SucceedsWithoutOpaque) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"src");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const NTSTATUS st = cu.HandleDirectoryRename(L"src", L"dst", false);
        Assert::IsTrue(NT_SUCCESS(st));

        Assert::IsFalse(env.FileExists(env.Upper(), L"src"));
        Assert::IsTrue(env.FileExists(env.Upper(), L"dst"));
        Assert::IsFalse(wm.IsOpaque(L"dst"),
            L"Upper-to-upper move of a non-opaque dir must not fabricate opacity");
    }

    // ---------------------------------------------------------------
    // R4 — upper-only populated dir rename carries children
    // ---------------------------------------------------------------
    TEST_METHOD(R4_UpperOnlyPopulatedDirRename_CarriesChildren) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"src");
        env.WriteFile(env.Upper(), L"src\\inner.txt", "alpha");
        env.WriteFile(env.Upper(), L"src\\deep\\more.txt", "beta");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.HandleDirectoryRename(L"src", L"dst", false)));

        Assert::IsTrue(env.FileExists(env.Upper(), L"dst\\inner.txt"));
        Assert::IsTrue(env.FileExists(env.Upper(), L"dst\\deep\\more.txt"));
        Assert::AreEqual(std::string("alpha"),
                         env.ReadFile(env.Upper(), L"dst\\inner.txt"));
    }

    // ---------------------------------------------------------------
    // R5 — upper-only dir with opaque marker: marker moves with the dir
    // ---------------------------------------------------------------
    TEST_METHOD(R5_UpperOnlyOpaqueDirRename_OpaqueMarkerTransfers) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"src");
        env.WriteFile(env.Upper(), L"src\\child.txt", "a");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(wm.SetOpaque(L"src"));
        Assert::IsTrue(wm.IsOpaque(L"src"));

        Assert::IsTrue(NT_SUCCESS(cu.HandleDirectoryRename(L"src", L"dst", false)));

        Assert::IsFalse(wm.IsOpaque(L"src"),
            L"Opacity should no longer be reported for the vanished source path");
        Assert::IsTrue(wm.IsOpaque(L"dst"),
            L"Opaque marker must travel with the directory");
    }

    // ---------------------------------------------------------------
    // R6 — lower-only file rename: copy-up, move, whiteout, cache invalidation
    // ---------------------------------------------------------------
    TEST_METHOD(R6_LowerOnlyFileRename_CopiesUpMovesAndWhiteouts) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"source.txt", "payload");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // Prime cache by resolving — we expect the rename to invalidate it.
        resolver.ResolvePath(L"source.txt");
        Assert::IsTrue(cache.Get(L"source.txt").has_value());

        Assert::IsTrue(NT_SUCCESS(
            SimulateLowerFileRename(cu, resolver, wm, cache,
                                    L"source.txt", L"target.txt", false)));

        // Content lives at the new upper path, lower untouched.
        Assert::IsTrue(env.FileExists(env.Upper(), L"target.txt"));
        Assert::AreEqual(std::string("payload"),
                         env.ReadFile(env.Upper(), L"target.txt"));
        Assert::IsTrue(env.FileExists(env.Lower(0), L"source.txt"),
            L"Rename must never mutate the lower layer");

        // Whiteout at old path, no whiteout at new path.
        Assert::IsTrue(wm.HasWhiteout(L"source.txt", env.Upper()));
        Assert::IsFalse(wm.HasWhiteout(L"target.txt", env.Upper()));

        // Both path entries in cache invalidated.
        Assert::IsFalse(cache.Get(L"source.txt").has_value());
        Assert::IsFalse(cache.Get(L"target.txt").has_value());

        // Through the resolver: old path hidden by whiteout, new path surfaces upper.
        ResolvedPath rOld = resolver.ResolvePath(L"source.txt");
        Assert::IsFalse(rOld.Found());
        Assert::IsTrue(rOld.isWhiteout);

        ResolvedPath rNew = resolver.ResolvePath(L"target.txt");
        Assert::IsTrue(rNew.Found());
        Assert::IsTrue(rNew.source == LayerSource::Upper);
    }

    // ---------------------------------------------------------------
    // R7 — lower-only dir rename: recursive copy + opaque on new + whiteout on old
    // ---------------------------------------------------------------
    TEST_METHOD(R7_LowerOnlyDirRename_RecursiveCopyOpaqueAndWhiteout) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"ld\\nested");
        env.WriteFile(env.Lower(0), L"ld\\top.txt",            "top");
        env.WriteFile(env.Lower(0), L"ld\\nested\\inner.txt",  "inner");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.HandleDirectoryRename(L"ld", L"newdir", true)));

        // Children copied recursively into upper under the new name.
        Assert::IsTrue(env.FileExists(env.Upper(), L"newdir\\top.txt"));
        Assert::IsTrue(env.FileExists(env.Upper(), L"newdir\\nested\\inner.txt"));
        Assert::AreEqual(std::string("inner"),
                         env.ReadFile(env.Upper(), L"newdir\\nested\\inner.txt"));

        // New location is opaque, old has a whiteout.
        Assert::IsTrue(wm.IsOpaque(L"newdir"));
        Assert::IsTrue(wm.HasWhiteout(L"ld", env.Upper()));

        // Lower layer untouched.
        Assert::IsTrue(env.FileExists(env.Lower(0), L"ld\\top.txt"));
        Assert::IsTrue(env.FileExists(env.Lower(0), L"ld\\nested\\inner.txt"));
    }

    // ---------------------------------------------------------------
    // R8 — shadowed file rename (same name in both layers): upper is the source
    // ---------------------------------------------------------------
    TEST_METHOD(R8_ShadowedFileRename_UpperIsSource_LowerIntact) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"shared.txt", "LOWER");
        env.WriteFile(env.Upper(),  L"shared.txt", "UPPER");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // SRename treats this as upper-only (ExistsInUpper → true), so the
        // manager path is a straight MoveFileExW + cache invalidation. Simulate.
        const std::wstring oldUpper = resolver.GetUpperPath(L"shared.txt");
        const std::wstring newUpper = resolver.GetUpperPath(L"renamed.txt");
        Assert::IsTrue(::MoveFileExW(oldUpper.c_str(), newUpper.c_str(), 0) != FALSE);
        cache.InvalidateWithAncestors(L"shared.txt");
        cache.InvalidateWithAncestors(L"renamed.txt");

        // The old name now surfaces the *lower* value again — no whiteout was created.
        ResolvedPath rOld = resolver.ResolvePath(L"shared.txt");
        Assert::IsTrue(rOld.Found());
        Assert::IsTrue(rOld.source == LayerSource::Lower,
            L"With the upper copy gone and no whiteout, the lower file resurfaces");

        ResolvedPath rNew = resolver.ResolvePath(L"renamed.txt");
        Assert::IsTrue(rNew.Found());
        Assert::IsTrue(rNew.source == LayerSource::Upper);

        Assert::AreEqual(std::string("UPPER"),
                         env.ReadFile(env.Upper(), L"renamed.txt"));
        Assert::AreEqual(std::string("LOWER"),
                         env.ReadFile(env.Lower(0), L"shared.txt"));
    }

    // ---------------------------------------------------------------
    // R9 — rename onto target that already has a whiteout: whiteout must clear
    // ---------------------------------------------------------------
    TEST_METHOD(R9_RenameOntoWhitedOutTarget_WhiteoutIsCleared) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"src.txt",    "src-payload");
        env.WriteFile(env.Lower(0), L"target.txt", "target-lower");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // target.txt has a pre-existing whiteout (user previously deleted it).
        Assert::IsTrue(wm.CreateWhiteout(L"target.txt"));
        Assert::IsTrue(wm.HasWhiteout(L"target.txt", env.Upper()));

        Assert::IsTrue(NT_SUCCESS(
            SimulateLowerFileRename(cu, resolver, wm, cache,
                                    L"src.txt", L"target.txt", true)));

        // Whiteout at target was cleared so the moved file is visible.
        Assert::IsFalse(wm.HasWhiteout(L"target.txt", env.Upper()));
        Assert::AreEqual(std::string("src-payload"),
                         env.ReadFile(env.Upper(), L"target.txt"));

        // Whiteout at source was created (stops lower src.txt from resurfacing).
        Assert::IsTrue(wm.HasWhiteout(L"src.txt", env.Upper()));
    }

    // ---------------------------------------------------------------
    // R11 — renaming a metacopy-only file should leave copy-up state coherent
    // ---------------------------------------------------------------
    TEST_METHOD(R11_RenameMetacopyFile_MetadataSurvivesMove) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"lazy.bin", std::string(1024, 'Q'));

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // Stage the lazy copy-up, then rename in-upper.
        Assert::IsTrue(NT_SUCCESS(cu.CopyUpMetadataOnly(L"lazy.bin")));
        Assert::IsTrue(MetadataADS::ReadLayerMountMetadata(
                           env.Upper() + L"\\lazy.bin").metacopy);

        // Now upper-to-upper rename (source already in upper after metacopy).
        const std::wstring oldUpper = resolver.GetUpperPath(L"lazy.bin");
        const std::wstring newUpper = resolver.GetUpperPath(L"renamed.bin");
        Assert::IsTrue(::MoveFileExW(oldUpper.c_str(), newUpper.c_str(), 0) != FALSE);
        cache.InvalidateWithAncestors(L"lazy.bin");
        cache.InvalidateWithAncestors(L"renamed.bin");

        // Metacopy flag and logical size must survive the move.
        LayerMountMetadata md = MetadataADS::ReadLayerMountMetadata(
            env.Upper() + L"\\renamed.bin");
        Assert::IsTrue(md.metacopy, L"metacopy flag must survive MoveFileExW");
        Assert::IsFalse(md.originLayer.empty(),
            L"originLayer must survive MoveFileExW");

        // Completing the lazy copy at the new name must still fill data.
        Assert::IsTrue(NT_SUCCESS(cu.CompleteLazyCopyUp(L"renamed.bin")));
        Assert::AreEqual(std::string(1024, 'Q'),
                         env.ReadFile(env.Upper(), L"renamed.bin"));
    }

    // ---------------------------------------------------------------
    // R12 — rename cross-directory where the new parent doesn't exist in upper
    // ---------------------------------------------------------------
    TEST_METHOD(R12_RenameCrossDir_NewParentMissing_ParentIsCreated) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"foo.txt", "payload");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // Upper has no "sub\\deeper" yet — the rename must create it.
        Assert::IsTrue(NT_SUCCESS(
            SimulateLowerFileRename(cu, resolver, wm, cache,
                                    L"foo.txt", L"sub\\deeper\\moved.txt",
                                    false)));

        Assert::IsTrue(env.FileExists(env.Upper(), L"sub\\deeper\\moved.txt"));
        Assert::AreEqual(std::string("payload"),
                         env.ReadFile(env.Upper(), L"sub\\deeper\\moved.txt"));
        Assert::IsTrue(wm.HasWhiteout(L"foo.txt", env.Upper()));
    }

    // ---------------------------------------------------------------
    // R13 — rename within an opaque directory still works and keeps opacity
    // ---------------------------------------------------------------
    TEST_METHOD(R13_RenameInsideOpaqueDir_SucceedsAndOpacityPreserved) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"box");
        env.WriteFile(env.Upper(), L"box\\a.txt", "data");
        Assert::IsTrue(WhiteoutManager(env.MakeConfig()).SetOpaque(L"box"));

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        // Simple upper-to-upper move inside the opaque dir.
        const std::wstring oldUpper = resolver.GetUpperPath(L"box\\a.txt");
        const std::wstring newUpper = resolver.GetUpperPath(L"box\\b.txt");
        Assert::IsTrue(::MoveFileExW(oldUpper.c_str(), newUpper.c_str(), 0) != FALSE);
        cache.InvalidateWithAncestors(L"box\\a.txt");
        cache.InvalidateWithAncestors(L"box\\b.txt");

        Assert::IsTrue(env.FileExists(env.Upper(), L"box\\b.txt"));
        Assert::IsTrue(wm.IsOpaque(L"box"),
            L"Opacity of the containing directory must be unaffected by child rename");
    }

    // ---------------------------------------------------------------
    // R14 — redirect ADS that points at itself is bounded by the resolver depth guard
    // ---------------------------------------------------------------
    TEST_METHOD(R14_RedirectCycle_ResolverDepthGuardFires) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"loop");

        // Plant a self-referential redirect on the upper dir.
        LayerMountMetadata md;
        md.redirect = L"loop";
        Assert::IsTrue(MetadataADS::WriteLayerMountMetadata(
                           env.Upper() + L"\\loop", md));

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        // Should return without hanging or crashing — depth guard kicks in at 40.
        ResolvedPath r = resolver.ResolvePath(L"loop");
        Assert::IsFalse(r.Found(),
            L"Self-redirecting metadata must not resolve — depth guard should refuse");
    }

    // ---------------------------------------------------------------
    // R15 — directory rename with replaceIfExists=false into an existing
    //        destination must FAIL with STATUS_OBJECT_NAME_COLLISION.
    //
    // Previously ignored: HandleDirectoryRename silently merged (lower→
    // existing-dst) or overwrote (upper→existing-dst). Win32 rename
    // semantics demand a collision error when ReplaceIfExists is false.
    // ---------------------------------------------------------------
    TEST_METHOD(R15_DirRename_DestExistsInUpper_NoReplace_FailsWithCollision) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"src");
        env.WriteFile(env.Upper(), L"src\\content.txt", "src-payload");
        // Destination already has its own content.
        env.CreateDir(env.Upper(), L"dst");
        env.WriteFile(env.Upper(), L"dst\\other.txt", "dst-payload");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const NTSTATUS st = cu.HandleDirectoryRename(
            L"src", L"dst", /*sourceIsInLower=*/false, /*replaceIfExists=*/false);
        Assert::AreEqual(
            static_cast<long>(STATUS_OBJECT_NAME_COLLISION),
            static_cast<long>(st),
            L"Upper-source dir rename into existing dst without replace "
            L"must surface STATUS_OBJECT_NAME_COLLISION");

        // Source MUST remain intact — no side effects on collision.
        Assert::IsTrue(env.FileExists(env.Upper(), L"src\\content.txt"));
        // Destination MUST remain intact — not silently overwritten.
        Assert::IsTrue(env.FileExists(env.Upper(), L"dst\\other.txt"));
    }

    // Lower-source variant: destination exists in lower and is NOT whited
    // out. A rename-without-replace must still fail.
    TEST_METHOD(R15b_DirRename_DestExistsInLower_NoReplace_FailsWithCollision) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"src");
        env.WriteFile(env.Lower(0), L"src\\a.txt", "src");
        env.CreateDir(env.Lower(0), L"dst");
        env.WriteFile(env.Lower(0), L"dst\\b.txt", "dst");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const NTSTATUS st = cu.HandleDirectoryRename(
            L"src", L"dst", /*sourceIsInLower=*/true, /*replaceIfExists=*/false);
        Assert::AreEqual(
            static_cast<long>(STATUS_OBJECT_NAME_COLLISION),
            static_cast<long>(st));

        // No whiteout for src was created; no opaque marker for dst.
        Assert::IsFalse(wm.HasWhiteout(L"src", env.Upper()));
        Assert::IsFalse(wm.IsOpaque(L"dst"));
        Assert::IsFalse(env.FileExists(env.Upper(), L"dst\\b.txt"));
    }

    // Collision-guard edge: dst has a whiteout in upper AND an entry in
    // lower. Merged view does NOT show dst (whiteout hides it), so the
    // rename must succeed.
    TEST_METHOD(R15d_DirRename_DestLowerButWhitedOut_NoReplace_Succeeds) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"src");
        env.WriteFile(env.Upper(), L"src\\content.txt", "src-payload");
        env.WriteFile(env.Lower(0), L"dst\\old.txt", "old-lower");
        Assert::IsTrue(wm_CreateWhiteoutHelper(env, L"dst"));

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const NTSTATUS st = cu.HandleDirectoryRename(
            L"src", L"dst", /*sourceIsInLower=*/false, /*replaceIfExists=*/false);
        Assert::IsTrue(NT_SUCCESS(st),
            L"Whited-out destination is invisible in merged view — rename "
            L"without replace must succeed, not collision.");
    }

    // ---------------------------------------------------------------
    // R16 — CopyTreePreservingMetadata now propagates child failures.
    //
    // Previously child-copy return values were dropped, so a file-over-
    // directory type collision in a nested child left a partial tree at
    // the destination while the caller saw success. We construct exactly
    // that collision and verify the overall rename surfaces
    // STATUS_OBJECT_NAME_COLLISION from CopyDirectoryShell's type check,
    // and that the half-built destination is torn down by
    // HandleDirectoryRename.
    //
    // Setup: lower has `src\sub\inner.txt` (a directory). Upper already
    // has `dst\` (a directory, pre-existing user data) and `dst\sub`
    // (a FILE — the type conflict). Rename with replaceIfExists=true to
    // bypass the top-level collision guard and drive the recursive copy
    // into the file-over-directory conflict at `dst\sub`.
    // ---------------------------------------------------------------
    TEST_METHOD(R16_DirRename_ChildTypeConflict_FailsAndTearsDownDst) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"src\\sub\\inner.txt", "inner");
        env.CreateDir(env.Upper(), L"dst");
        env.WriteFile(env.Upper(), L"dst\\sub", "file-not-dir");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const NTSTATUS st = cu.HandleDirectoryRename(
            L"src", L"dst", /*sourceIsInLower=*/true, /*replaceIfExists=*/true);
        Assert::AreEqual(
            static_cast<long>(STATUS_OBJECT_NAME_COLLISION),
            static_cast<long>(st),
            L"File-over-directory type conflict during child copy must "
            L"surface STATUS_OBJECT_NAME_COLLISION — not a silent success "
            L"with a partial tree.");

        // Destination torn down — no inner.txt landed, no opaque marker.
        Assert::IsFalse(env.FileExists(env.Upper(), L"dst\\sub\\inner.txt"));
        Assert::IsFalse(wm.IsOpaque(L"dst"));
        // No whiteout at source was created (rename never committed).
        Assert::IsFalse(wm.HasWhiteout(L"src", env.Upper()));
    }

private:
    // Helper: creates a whiteout at `rel` by writing a .wh.<name> stub next
    // to the parent dir in upper. Intentionally lightweight — avoids
    // requiring a live LayerMount instance.
    static bool wm_CreateWhiteoutHelper(TempLayerEnvironment& env,
                                         const std::wstring& rel) {
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        return wm.CreateWhiteout(rel, WhiteoutType::Directory);
    }
};

} // namespace LayerMountTests
