#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataStore.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

TEST_CLASS(RenameMoveTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    static NTSTATUS SimulateLowerFileRename(CopyUp& cu,
                                            PathResolver& resolver,
                                            WhiteoutManager& wm,
                                            Cache& cache,
                                            const std::wstring& oldNorm,
                                            const std::wstring& newNorm,
                                            ReplaceExisting replace) {
        if (wm.HasWhiteout(newNorm, resolver.Config().upperPath)) {
            wm.RemoveWhiteout(newNorm);
        }

        NTSTATUS st = cu.CopyUpFile(oldNorm);
        if (!NT_SUCCESS(st)) return st;

        const std::wstring oldUpper = resolver.GetUpperPath(oldNorm);
        const std::wstring newUpper = resolver.GetUpperPath(newNorm);
        EnsureDirectoryExists(
            std::filesystem::path(newUpper).parent_path().wstring());

        const DWORD flags = replace == ReplaceExisting::Yes ? MOVEFILE_REPLACE_EXISTING : 0;
        if (!::MoveFileExW(oldUpper.c_str(), newUpper.c_str(), flags)) {
            return HRESULT_FROM_WIN32(::GetLastError());
        }

        wm.CreateWhiteout(oldNorm, WhiteoutType::File);

        cache.InvalidateWithAncestors(oldNorm);
        cache.InvalidateWithAncestors(newNorm);
        return STATUS_SUCCESS;
    }

    TEST_METHOD(UpperOnlyEmptyDirRename_SucceedsWithoutOpaque) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"src");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const NTSTATUS st = cu.RenameUpperDirectory(L"src", L"dst", ReplaceExisting::No);
        Assert::IsTrue(NT_SUCCESS(st));

        Assert::IsFalse(env.FileExists(env.Upper(), L"src"));
        Assert::IsTrue(env.FileExists(env.Upper(), L"dst"));
        Assert::IsFalse(wm.IsOpaque(L"dst"),
            L"Upper-to-upper move of a non-opaque dir must not fabricate opacity");
    }

    TEST_METHOD(UpperOnlyPopulatedDirRename_CarriesChildren) {
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

        Assert::IsTrue(NT_SUCCESS(cu.RenameUpperDirectory(L"src", L"dst", ReplaceExisting::No)));

        Assert::IsTrue(env.FileExists(env.Upper(), L"dst\\inner.txt"));
        Assert::IsTrue(env.FileExists(env.Upper(), L"dst\\deep\\more.txt"));
        Assert::AreEqual(std::string("alpha"),
                         env.ReadFile(env.Upper(), L"dst\\inner.txt"));
    }

    TEST_METHOD(UpperOnlyOpaqueDirRename_OpaqueMarkerTransfers) {
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

        Assert::IsTrue(NT_SUCCESS(cu.RenameUpperDirectory(L"src", L"dst", ReplaceExisting::No)));

        Assert::IsFalse(wm.IsOpaque(L"src"),
            L"Opacity should no longer be reported for the vanished source path");
        Assert::IsTrue(wm.IsOpaque(L"dst"),
            L"Opaque marker must travel with the directory");
    }

    TEST_METHOD(LowerOnlyFileRename_CopiesUpMovesAndWhiteouts) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"source.txt", "payload");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        resolver.ResolvePath(L"source.txt");
        Assert::IsTrue(cache.Get(L"source.txt").has_value());

        Assert::IsTrue(NT_SUCCESS(
            SimulateLowerFileRename(cu, resolver, wm, cache,
                                    L"source.txt", L"target.txt", ReplaceExisting::No)));

        Assert::IsTrue(env.FileExists(env.Upper(), L"target.txt"));
        Assert::AreEqual(std::string("payload"),
                         env.ReadFile(env.Upper(), L"target.txt"));
        Assert::IsTrue(env.FileExists(env.Lower(0), L"source.txt"),
            L"Rename must never mutate the lower layer");

        Assert::IsTrue(wm.HasWhiteout(L"source.txt", env.Upper()));
        Assert::IsFalse(wm.HasWhiteout(L"target.txt", env.Upper()));

        Assert::IsFalse(cache.Get(L"source.txt").has_value());
        Assert::IsFalse(cache.Get(L"target.txt").has_value());

        ResolvedPath rOld = resolver.ResolvePath(L"source.txt");
        Assert::IsFalse(rOld.Found());
        Assert::IsTrue(rOld.isWhiteout);

        ResolvedPath rNew = resolver.ResolvePath(L"target.txt");
        Assert::IsTrue(rNew.Found());
        Assert::IsTrue(rNew.source == LayerSource::Upper);
    }

    TEST_METHOD(LowerOnlyDirRename_RecursiveCopyOpaqueAndWhiteout) {
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

        Assert::IsTrue(NT_SUCCESS(cu.RenameLowerDirectory(L"ld", L"newdir", ReplaceExisting::No)));

        Assert::IsTrue(env.FileExists(env.Upper(), L"newdir\\top.txt"));
        Assert::IsTrue(env.FileExists(env.Upper(), L"newdir\\nested\\inner.txt"));
        Assert::AreEqual(std::string("inner"),
                         env.ReadFile(env.Upper(), L"newdir\\nested\\inner.txt"));

        Assert::IsTrue(wm.IsOpaque(L"newdir"));
        Assert::IsTrue(wm.HasWhiteout(L"ld", env.Upper()));

        Assert::IsTrue(env.FileExists(env.Lower(0), L"ld\\top.txt"));
        Assert::IsTrue(env.FileExists(env.Lower(0), L"ld\\nested\\inner.txt"));
    }

    TEST_METHOD(ShadowedFileRename_UpperIsSource_LowerIntact) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"shared.txt", "LOWER");
        env.WriteFile(env.Upper(),  L"shared.txt", "UPPER");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const std::wstring oldUpper = resolver.GetUpperPath(L"shared.txt");
        const std::wstring newUpper = resolver.GetUpperPath(L"renamed.txt");
        Assert::IsTrue(::MoveFileExW(oldUpper.c_str(), newUpper.c_str(), 0) != FALSE);
        cache.InvalidateWithAncestors(L"shared.txt");
        cache.InvalidateWithAncestors(L"renamed.txt");

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

    TEST_METHOD(RenameOntoWhitedOutTarget_WhiteoutIsCleared) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"src.txt",    "src-payload");
        env.WriteFile(env.Lower(0), L"target.txt", "target-lower");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(wm.CreateWhiteout(L"target.txt", WhiteoutType::File));
        Assert::IsTrue(wm.HasWhiteout(L"target.txt", env.Upper()));

        Assert::IsTrue(NT_SUCCESS(
            SimulateLowerFileRename(cu, resolver, wm, cache,
                                    L"src.txt", L"target.txt", ReplaceExisting::Yes)));

        Assert::IsFalse(wm.HasWhiteout(L"target.txt", env.Upper()));
        Assert::AreEqual(std::string("src-payload"),
                         env.ReadFile(env.Upper(), L"target.txt"));

        Assert::IsTrue(wm.HasWhiteout(L"src.txt", env.Upper()));
    }

    TEST_METHOD(RenameMetacopyFile_MetadataSurvivesMove) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"lazy.bin", std::string(1024, 'Q'));

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpMetadataOnly(L"lazy.bin")));
        Assert::IsTrue(MetadataStore::ReadLayerMountMetadata(
                           env.Upper() + L"\\lazy.bin", nullptr).metacopy);

        const std::wstring oldUpper = resolver.GetUpperPath(L"lazy.bin");
        const std::wstring newUpper = resolver.GetUpperPath(L"renamed.bin");
        Assert::IsTrue(::MoveFileExW(oldUpper.c_str(), newUpper.c_str(), 0) != FALSE);
        cache.InvalidateWithAncestors(L"lazy.bin");
        cache.InvalidateWithAncestors(L"renamed.bin");

        LayerMountMetadata md = MetadataStore::ReadLayerMountMetadata(
            env.Upper() + L"\\renamed.bin", nullptr);
        Assert::IsTrue(md.metacopy, L"metacopy flag must survive MoveFileExW");
        Assert::IsFalse(md.originLayer.empty(),
            L"originLayer must survive MoveFileExW");

        Assert::IsTrue(NT_SUCCESS(cu.CompleteLazyCopyUp(L"renamed.bin")));
        Assert::AreEqual(std::string(1024, 'Q'),
                         env.ReadFile(env.Upper(), L"renamed.bin"));
    }

    TEST_METHOD(RenameCrossDir_NewParentMissing_ParentIsCreated) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"foo.txt", "payload");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(
            SimulateLowerFileRename(cu, resolver, wm, cache,
                                    L"foo.txt", L"sub\\deeper\\moved.txt",
                                    ReplaceExisting::No)));

        Assert::IsTrue(env.FileExists(env.Upper(), L"sub\\deeper\\moved.txt"));
        Assert::AreEqual(std::string("payload"),
                         env.ReadFile(env.Upper(), L"sub\\deeper\\moved.txt"));
        Assert::IsTrue(wm.HasWhiteout(L"foo.txt", env.Upper()));
    }

    TEST_METHOD(RenameInsideOpaqueDir_SucceedsAndOpacityPreserved) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"box");
        env.WriteFile(env.Upper(), L"box\\a.txt", "data");
        auto config = env.MakeConfig();
        Assert::IsTrue(WhiteoutManager(config, nullptr).SetOpaque(L"box"));

        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        const std::wstring oldUpper = resolver.GetUpperPath(L"box\\a.txt");
        const std::wstring newUpper = resolver.GetUpperPath(L"box\\b.txt");
        Assert::IsTrue(::MoveFileExW(oldUpper.c_str(), newUpper.c_str(), 0) != FALSE);
        cache.InvalidateWithAncestors(L"box\\a.txt");
        cache.InvalidateWithAncestors(L"box\\b.txt");

        Assert::IsTrue(env.FileExists(env.Upper(), L"box\\b.txt"));
        Assert::IsTrue(wm.IsOpaque(L"box"),
            L"Opacity of the containing directory must be unaffected by child rename");
    }

    TEST_METHOD(RedirectCycle_ResolverDepthGuardFires) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"loop");

        LayerMountMetadata md;
        md.redirect = L"loop";
        Assert::IsTrue(MetadataStore::WriteLayerMountMetadata(
                           env.Upper() + L"\\loop", md, nullptr));

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath r = resolver.ResolvePath(L"loop");
        Assert::IsFalse(r.Found(),
            L"Self-redirecting metadata must not resolve — depth guard should refuse");
    }

    TEST_METHOD(DirRename_DestExistsInUpper_NoReplace_FailsWithCollision) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"src");
        env.WriteFile(env.Upper(), L"src\\content.txt", "src-payload");
        env.CreateDir(env.Upper(), L"dst");
        env.WriteFile(env.Upper(), L"dst\\other.txt", "dst-payload");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const NTSTATUS st = cu.RenameUpperDirectory(
            L"src", L"dst", ReplaceExisting::No);
        Assert::AreEqual(
            static_cast<long>(STATUS_OBJECT_NAME_COLLISION),
            static_cast<long>(st),
            L"Upper-source dir rename into existing dst without replace "
            L"must surface STATUS_OBJECT_NAME_COLLISION");

        Assert::IsTrue(env.FileExists(env.Upper(), L"src\\content.txt"));
        Assert::IsTrue(env.FileExists(env.Upper(), L"dst\\other.txt"));
    }

    TEST_METHOD(DirRename_DestExistsInLower_NoReplace_FailsWithCollision) {
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

        const NTSTATUS st = cu.RenameLowerDirectory(
            L"src", L"dst", ReplaceExisting::No);
        Assert::AreEqual(
            static_cast<long>(STATUS_OBJECT_NAME_COLLISION),
            static_cast<long>(st));

        Assert::IsFalse(wm.HasWhiteout(L"src", env.Upper()));
        Assert::IsFalse(wm.IsOpaque(L"dst"));
        Assert::IsFalse(env.FileExists(env.Upper(), L"dst\\b.txt"));
    }

    TEST_METHOD(DirRename_DestLowerButWhitedOut_NoReplace_Succeeds) {
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

        const NTSTATUS st = cu.RenameUpperDirectory(
            L"src", L"dst", ReplaceExisting::No);
        Assert::IsTrue(NT_SUCCESS(st),
            L"Whited-out destination is invisible in merged view — rename "
            L"without replace must succeed, not collision.");
    }

    TEST_METHOD(DirRename_ChildTypeConflict_FailsAndTearsDownDst) {
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

        // ReplaceExisting::Yes gets past the top-level collision check, so
        // the copy reaches the child conflict.
        const NTSTATUS st = cu.RenameLowerDirectory(
            L"src", L"dst", ReplaceExisting::Yes);
        Assert::AreEqual(
            static_cast<long>(STATUS_OBJECT_NAME_COLLISION),
            static_cast<long>(st),
            L"File-over-directory type conflict during child copy must "
            L"surface STATUS_OBJECT_NAME_COLLISION — not a silent success "
            L"with a partial tree.");

        Assert::IsFalse(env.FileExists(env.Upper(), L"dst\\sub\\inner.txt"));
        Assert::IsFalse(wm.IsOpaque(L"dst"));
        Assert::IsFalse(wm.HasWhiteout(L"src", env.Upper()));
    }

private:
    static bool wm_CreateWhiteoutHelper(TempLayerEnvironment& env,
                                         const std::wstring& rel) {
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        return wm.CreateWhiteout(rel, WhiteoutType::Directory);
    }
};

}
