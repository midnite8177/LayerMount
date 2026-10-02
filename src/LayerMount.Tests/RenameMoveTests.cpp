#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataStore.h"

#include "AclTestHelpers.h"

#include <cstdlib>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::AddDenyAce;
using LayerMountTestShared::BackupPrivilegeDisabledOnThread;
using LayerMountTestShared::DirectoryListingDenied;

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

namespace {

enum class DirectoryLayer {
    Upper,
    Lower,
};

const std::wstring& LayerRoot(const TempLayerEnvironment& env, DirectoryLayer layer) {
    return layer == DirectoryLayer::Upper ? env.Upper() : env.Lower(0);
}

void AssertEntryShownAs(const ::LayerMount::LayerMount& mount,
                        const std::wstring& dir,
                        const std::wstring& key,
                        const std::wstring& displayName) {
    const MergedDirectory listing = mount.MergeDirectoryEntries(dir);
    AssertStatus(STATUS_SUCCESS, listing.status,
        (L"The listing of '" + dir + L"' must succeed").c_str());
    Assert::IsTrue(listing.entries.count(key) == 1,
        (L"The listing of '" + dir + L"' must hold " + key).c_str());
    Assert::AreEqual(displayName, std::wstring(listing.entries.at(key).findData.cFileName),
        (L"The listing of '" + dir + L"' must show the entry as " + displayName).c_str());
}

std::wstring ListingKey(const std::wstring& displayName) {
    std::wstring key = displayName;
    ::CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    return key;
}

void AssertOnlyEntryShownAs(const ::LayerMount::LayerMount& mount,
                            const std::wstring& dir,
                            const std::wstring& displayName) {
    const MergedDirectory listing = mount.MergeDirectoryEntries(dir);
    AssertStatus(STATUS_SUCCESS, listing.status,
        (L"The listing of '" + dir + L"' must succeed").c_str());
    Assert::AreEqual(size_t{1}, listing.entries.size(),
        (L"The listing of '" + dir + L"' must hold one entry").c_str());
    AssertEntryShownAs(mount, dir, ListingKey(displayName), displayName);
}

void AssertListedDirectoryWithChild(const ::LayerMount::LayerMount& mount,
                                    const std::wstring& displayName,
                                    const std::wstring& childKey) {
    AssertOnlyEntryShownAs(mount, L"", displayName);

    const MergedDirectory children = mount.MergeDirectoryEntries(displayName);
    AssertStatus(STATUS_SUCCESS, children.status, L"The directory listing must succeed");
    Assert::IsTrue(children.entries.count(childKey) == 1,
        (L"The listing of '" + displayName + L"' must hold " + childKey).c_str());
}

void AssertUpperDirectoryCopiedUp(const TempLayerEnvironment& env, const std::wstring& dir) {
    const LayerMountMetadata metadata =
        MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\" + dir, nullptr);
    Assert::IsFalse(metadata.originLayer.empty(),
        (L"The upper '" + dir + L"' must carry copy-up metadata").c_str());
}

void AssertRootShowsOnlyFileNamed(const ::LayerMount::LayerMount& mount, const std::wstring& displayName) {
    AssertOnlyEntryShownAs(mount, L"", displayName);
    const MergedDirectory listing = mount.MergeDirectoryEntries(L"");
    Assert::AreEqual<DWORD>(0,
        listing.entries.at(ListingKey(displayName)).findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY,
        (L"The root must list " + displayName + L" as a file").c_str());
}

std::vector<std::wstring> EntriesUnder(const std::wstring& root) {
    std::vector<std::wstring> entries;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        entries.push_back(std::filesystem::relative(entry.path(), root).wstring());
    }
    return entries;
}

// mklink /J needs no symbolic-link privilege.
bool CreateDirectoryJunction(const std::wstring& junction, const std::wstring& target) {
    const std::wstring command =
        L"cmd.exe /c mklink /J \"" + junction + L"\" \"" + target + L"\" >nul 2>&1";
    return _wsystem(command.c_str()) == 0;
}

void AssertDirectoryRenameShowsNewName(DirectoryLayer layer,
                                       const std::wstring& from,
                                       const std::wstring& to,
                                       BOOLEAN replaceIfExists) {
    TempLayerEnvironment env(1);
    env.CreateDir(LayerRoot(env, layer), from);
    env.WriteFile(LayerRoot(env, layer), from + L"\\a.txt", "x");
    ::LayerMount::LayerMount mount(env.MakeConfig());

    AssertStatus(STATUS_SUCCESS, mount.Rename(from, to, replaceIfExists, kNoCallerPid),
        (L"The rename of " + from + L" to " + to + L" must succeed").c_str());
    AssertListedDirectoryWithChild(mount, to, L"a.txt");
}

}

TEST_CLASS(MountDirectoryRenameTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(CaseOnlyRename_UpperOnlyDirectory_ListsOnlyTheNewCase) {
        for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
            AssertDirectoryRenameShowsNewName(DirectoryLayer::Upper, L"Foo", L"foo", replace);
            AssertDirectoryRenameShowsNewName(DirectoryLayer::Upper, L"foo", L"FOO", replace);
        }
    }

    TEST_METHOD(CaseOnlyRename_LowerOnlyDirectory_ListsOnlyTheNewCase) {
        for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
            AssertDirectoryRenameShowsNewName(DirectoryLayer::Lower, L"Foo", L"foo", replace);
            AssertDirectoryRenameShowsNewName(DirectoryLayer::Lower, L"foo", L"FOO", replace);
        }
    }

    TEST_METHOD(Rename_UpperOnlyDirectory_ListsTheNewNameInTheCallersCase) {
        AssertDirectoryRenameShowsNewName(DirectoryLayer::Upper, L"a", L"NewDir", kFailIfExists);
    }

    TEST_METHOD(Rename_LowerOnlyDirectory_ListsTheNewNameInTheCallersCase) {
        AssertDirectoryRenameShowsNewName(DirectoryLayer::Lower, L"a", L"NewDir", kFailIfExists);
    }

    TEST_METHOD(CaseOnlyRename_DirectoryInBothLayers_ListsOneEntryWithBothChildren) {
        for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
            for (const std::wstring from : {L"Foo", L"foo"}) {
                TempLayerEnvironment env(1);
                env.WriteFile(env.Lower(0), L"Foo\\a.txt", "lower");
                env.WriteFile(env.Upper(), L"foo\\b.txt", "upper");
                ::LayerMount::LayerMount mount(env.MakeConfig());

                AssertStatus(STATUS_SUCCESS, mount.Rename(from, L"FOO", replace, kNoCallerPid),
                    (L"The rename of " + from + L" to FOO must succeed").c_str());
                AssertListedDirectoryWithChild(mount, L"FOO", L"a.txt");
                AssertListedDirectoryWithChild(mount, L"FOO", L"b.txt");
            }
        }
    }

    TEST_METHOD(Rename_LowerDirectoryIntoLowerOnlyParent_ListsTheParentInTheLowersCase) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"Dir");
        env.WriteFile(env.Lower(0), L"x\\a.txt", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"x", L"DIR\\y", kFailIfExists, kNoCallerPid),
            L"The rename of x to DIR\\y must succeed");
        AssertOnlyEntryShownAs(mount, L"", L"Dir");
        AssertOnlyEntryShownAs(mount, L"Dir", L"y");
        AssertUpperDirectoryCopiedUp(env, L"Dir");
    }

    TEST_METHOD(Rename_LowerFileIntoLowerOnlyParent_CopiesTheParentUpInTheLowersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo\\a.txt", "x");
        env.WriteFile(env.Lower(0), L"b.txt", "y");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"b.txt", L"foo\\b.txt", kFailIfExists, kNoCallerPid),
            L"The rename of b.txt to foo\\b.txt must succeed");
        AssertListedDirectoryWithChild(mount, L"Foo", L"a.txt");
        AssertListedDirectoryWithChild(mount, L"Foo", L"b.txt");
        AssertUpperDirectoryCopiedUp(env, L"Foo");
    }

    TEST_METHOD(Rename_LowerDirectoryIntoParentInNoLayer_ListsTheParentInTheCallersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"x\\a.txt", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"x", L"NewParent\\y", kFailIfExists, kNoCallerPid),
            L"The rename of x to NewParent\\y must succeed");
        AssertOnlyEntryShownAs(mount, L"", L"NewParent");
        AssertOnlyEntryShownAs(mount, L"NewParent", L"y");
    }

    TEST_METHOD(Rename_LowerFileIntoWhitedOutLowerDirectory_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo\\a.txt", "x");
        env.WriteFile(env.Lower(0), L"b.txt", "y");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"Foo"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const std::vector<std::wstring> upperBefore = EntriesUnder(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"b.txt", L"foo\\b.txt", kFailIfExists, kNoCallerPid),
            L"A rename into a whited-out directory must fail");
        Assert::IsTrue(upperBefore == EntriesUnder(env.Upper()),
            L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_LowerFileUnderLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        env.WriteFile(env.Lower(0), L"b.txt", "y");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const std::vector<std::wstring> upperBefore = EntriesUnder(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"b.txt", L"foo\\b.txt", kFailIfExists, kNoCallerPid),
            L"A rename to a path under a lower file must fail");
        Assert::IsTrue(upperBefore == EntriesUnder(env.Upper()),
            L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_LowerDirectoryUnderLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        env.WriteFile(env.Lower(0), L"x\\a.txt", "y");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const std::vector<std::wstring> upperBefore = EntriesUnder(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"x", L"foo\\x", kFailIfExists, kNoCallerPid),
            L"A rename of a directory to a path under a lower file must fail");
        Assert::IsTrue(upperBefore == EntriesUnder(env.Upper()),
            L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_LowerFileUnderWhitedOutLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        env.WriteFile(env.Lower(0), L"b.txt", "y");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"Foo"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const std::vector<std::wstring> upperBefore = EntriesUnder(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"b.txt", L"foo\\b.txt", kFailIfExists, kNoCallerPid),
            L"A rename to a path under a whited-out lower file must fail");
        Assert::IsTrue(upperBefore == EntriesUnder(env.Upper()),
            L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_LowerDirectoryIntoDirectoryHiddenByOpaqueAncestor_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\Sub\\a.txt", "x");
        env.WriteFile(env.Lower(0), L"x\\b.txt", "y");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const std::vector<std::wstring> upperBefore = EntriesUnder(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"x", L"d\\sub\\x", kFailIfExists, kNoCallerPid),
            L"A rename into a directory that an opaque ancestor hides must fail");
        Assert::IsTrue(upperBefore == EntriesUnder(env.Upper()),
            L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(CaseOnlyRename_LowerJunctionWithoutReparseSupport_CopiesTheTargetTreeUp) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"target\\inside.txt", "inside");
        if (!CreateDirectoryJunction(env.Lower(0) + L"\\Link", env.Lower(0) + L"\\target")) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the lower junction");
            return;
        }
        LayerConfig config = env.MakeConfig();
        config.hostCapabilities = LM_CAP_ADS | LM_CAP_SPARSE_FILES | LM_CAP_MULTIPLE_STREAMS |
                                  LM_CAP_NTFS_ACLS;
        ::LayerMount::LayerMount mount(config);

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"Link", L"LINK", kFailIfExists, kNoCallerPid),
            L"The rename of Link to LINK must succeed");

        const DWORD attrs = ::GetFileAttributesW((env.Upper() + L"\\LINK").c_str());
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES, attrs, L"The upper must hold LINK");
        Assert::AreEqual<DWORD>(0, attrs & FILE_ATTRIBUTE_REPARSE_POINT,
            L"An upper without reparse-point support must get a plain directory");
        Assert::AreEqual(std::string("inside"), env.ReadFile(env.Upper(), L"LINK\\inside.txt"),
            L"The upper directory must hold the junction target's files");
        Assert::IsFalse(env.FileExists(env.Lower(0), L"target\\.wh..wh..opq"),
            L"The rename must not write into the junction target in the lower");
        AssertEntryShownAs(mount, L"", L"link", L"LINK");
    }

    TEST_METHOD(Rename_LowerEntryToItsOwnName_LeavesTheUpperUntouched) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"Foo");
        env.WriteFile(env.Lower(0), L"Bar.txt", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const std::vector<std::wstring> upperBefore = EntriesUnder(env.Upper());

        for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
            AssertStatus(STATUS_SUCCESS, mount.Rename(L"Foo", L"Foo", replace, kNoCallerPid),
                L"The rename of Foo to Foo must succeed");
            AssertStatus(STATUS_SUCCESS, mount.Rename(L"Bar.txt", L"Bar.txt", replace, kNoCallerPid),
                L"The rename of Bar.txt to Bar.txt must succeed");
        }

        Assert::IsTrue(upperBefore == EntriesUnder(env.Upper()),
            L"A rename to the same name must not copy up or write a whiteout");
    }

    TEST_METHOD(RenameOpenFile_LowerFileToItsOwnName_KeepsReadingTheFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"bar.txt", "payload");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        AssertStatus(STATUS_SUCCESS, mount.Open(L"bar.txt", FILE_READ_DATA | DELETE,
                                                kNoCreateOptions, kNoCallerPid, &ctx, &info),
            L"The lower file must open");

        AssertStatus(STATUS_SUCCESS, mount.Rename(ctx.get(), L"bar.txt", kFailIfExists, kNoCallerPid),
            L"The rename of the open file to its own name must succeed");
        char buffer[16] = {};
        ULONG read = 0;
        const NTSTATUS readStatus = mount.Read(ctx.get(), buffer, 0, sizeof(buffer), &read);
        mount.Close(ctx.get());

        AssertStatus(STATUS_SUCCESS, readStatus, L"A read through the renamed handle must succeed");
        Assert::AreEqual(std::string("payload"), std::string(buffer, read));
    }

    TEST_METHOD(CaseOnlyRename_LowerDirectoryUnderDenyWriteParent_ListsTheNewCase) {
        UNIT_SKIP_IF_NOT_ADMIN();
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"secured\\Foo\\a.txt", "payload");
        AddDenyAce(env.Lower(0) + L"\\secured", FILE_GENERIC_WRITE,
                   OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"secured\\Foo", L"secured\\FOO", kFailIfExists, kNoCallerPid),
            L"The rename of secured\\Foo to secured\\FOO must succeed");
        AssertOnlyEntryShownAs(mount, L"secured", L"FOO");
    }
};

TEST_CLASS(MountDirectoryCopyUpTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(WriteOpen_UnderLowerDirectory_ListsTheDirectoryInTheLowersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo\\a.txt", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, OpenForWriteAndClose(mount, L"foo\\a.txt"),
            L"A write open of the lower file must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"foo\\a.txt"),
            L"The write open must copy the directory and the file up");

        AssertOnlyEntryShownAs(mount, L"", L"Foo");
        AssertOnlyEntryShownAs(mount, L"foo", L"a.txt");
    }

    TEST_METHOD(WriteOpen_UnderNestedLowerDirectories_ListsEachLevelInTheLowersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Outer\\Inner\\a.txt", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, OpenForWriteAndClose(mount, L"outer\\inner\\a.txt"),
            L"A write open of the nested lower file must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"outer\\inner\\a.txt"),
            L"The write open must copy both directories and the file up");

        AssertOnlyEntryShownAs(mount, L"", L"Outer");
        AssertOnlyEntryShownAs(mount, L"outer", L"Inner");
    }

    TEST_METHOD(Create_UnderLowerDirectory_CopiesTheDirectoryUpInTheLowersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo\\a.txt", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"foo\\b.txt", kNoCreateOptions),
            L"A create under the lower directory must succeed");

        AssertListedDirectoryWithChild(mount, L"Foo", L"a.txt");
        AssertListedDirectoryWithChild(mount, L"Foo", L"b.txt");
        AssertUpperDirectoryCopiedUp(env, L"Foo");
    }

    TEST_METHOD(Create_UnderWhitedOutLowerDirectory_FailsAndWritesNoParent) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo\\a.txt", "x");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"Foo"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            CreateThroughMount(mount, L"foo\\b.txt", kNoCreateOptions),
            L"A create under a whited-out directory must fail");
        Assert::IsFalse(env.FileExists(env.Upper(), L"foo"),
            L"The failed create must write no parent directory in the upper");
    }

    TEST_METHOD(Create_UnderLowerDirectoryHiddenByOpaqueAncestor_FailsAndWritesNoParent) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\Sub\\a.txt", "x");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            CreateThroughMount(mount, L"d\\sub\\b.txt", kNoCreateOptions),
            L"A create under a directory that an opaque ancestor hides must fail");
        Assert::IsFalse(env.FileExists(env.Upper(), L"d\\sub"),
            L"The failed create must write no parent directory in the upper");
    }

    TEST_METHOD(Create_UnderLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const std::vector<std::wstring> upperBefore = EntriesUnder(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            CreateThroughMount(mount, L"foo\\b.txt", kNoCreateOptions),
            L"A create under a lower file must fail");
        Assert::IsTrue(upperBefore == EntriesUnder(env.Upper()),
            L"The failed create must write nothing in the upper");
        AssertRootShowsOnlyFileNamed(mount, L"Foo");
    }

    TEST_METHOD(Create_TwoLevelsUnderLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const std::vector<std::wstring> upperBefore = EntriesUnder(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            CreateThroughMount(mount, L"foo\\bar\\b.txt", kNoCreateOptions),
            L"A create two levels under a lower file must fail");
        Assert::IsTrue(upperBefore == EntriesUnder(env.Upper()),
            L"The failed create must write nothing in the upper");
        AssertRootShowsOnlyFileNamed(mount, L"Foo");
    }

    TEST_METHOD(Create_UnderWhitedOutLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"Foo"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const std::vector<std::wstring> upperBefore = EntriesUnder(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            CreateThroughMount(mount, L"foo\\b.txt", kNoCreateOptions),
            L"A create under a whited-out lower file must fail");
        Assert::IsTrue(upperBefore == EntriesUnder(env.Upper()),
            L"The failed create must write nothing in the upper");
    }

    TEST_METHOD(Create_UnderParentInNoLayer_ListsTheParentInTheCallersCase) {
        TempLayerEnvironment env(1);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS,
            CreateThroughMount(mount, L"NewParent\\b.txt", kNoCreateOptions),
            L"A create under a parent in no layer must succeed");

        AssertOnlyEntryShownAs(mount, L"", L"NewParent");
    }

    TEST_METHOD(WriteOpen_UnderLowerDirectoryInAnUnlistableParent_CopiesUp) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"p");
        env.WriteFile(env.Lower(0), L"p\\Sub\\a.txt", "x");
        DirectoryListingDenied denied(env.Lower(0) + L"\\p");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        BackupPrivilegeDisabledOnThread noBackupPrivilege;
        LayerMountTestShared::AssertListingDenied(env.Lower(0) + L"\\p");

        AssertStatus(STATUS_SUCCESS, OpenForWriteAndClose(mount, L"p\\sub\\a.txt"),
            L"A write open must not fail when the lower parent cannot be listed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"p\\sub\\a.txt"),
            L"The write open must copy the directory and the file up");
    }

private:
    static NTSTATUS OpenForWriteAndClose(::LayerMount::LayerMount& mount,
                                         const std::wstring& path) {
        std::unique_ptr<::LayerMount::FileContext> ctx;
        ::LayerMount::InternalFileInfo info{};
        const NTSTATUS status = mount.Open(path, FILE_READ_DATA | FILE_WRITE_DATA,
                                           kNoCreateOptions, kNoCallerPid, &ctx, &info);
        if (ctx) mount.Close(ctx.get());
        return status;
    }
};

}
