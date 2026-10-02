#include "pch.h"
#include "TestFixture.h"

#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "CopyUp.h"
#include "MetadataStore.h"
#include "AclTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::BackupPrivilegeDisabledOnThread;
using LayerMountTestShared::DirectoryListingDenied;
using LayerMountTestShared::AssertListingDenied;

namespace LayerMountTests {

TEST_CLASS(DeleteTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

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

    TEST_METHOD(DeleteUpperOnlyFile_NoWhiteoutCreated) {
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

    TEST_METHOD(DeleteLowerOnlyFile_WhiteoutCreated) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"ghost.txt", "lower data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        SimulateFileDelete(resolver, wm, cache, L"ghost.txt");

        Assert::IsTrue(env.FileExists(env.Lower(0), L"ghost.txt"));
        Assert::IsTrue(wm.HasWhiteout(L"ghost.txt", env.Upper()));

        ResolvedPath r = resolver.ResolvePath(L"ghost.txt");
        Assert::IsFalse(r.Found());
        Assert::IsTrue(r.isWhiteout);
    }

    TEST_METHOD(DeleteShadowedFile_UpperRemovedAndWhiteoutCreated) {
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

    TEST_METHOD(DeleteUpperOnlyEmptyDir_NoWhiteoutCreated) {
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

    TEST_METHOD(DirEmptyInUpperButLowerChildren_MergedViewNotEmpty) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"mix");
        env.WriteFile(env.Lower(0), L"mix\\child.txt", "data");

        ::LayerMount::LayerMount mount(env.MakeConfig());
        auto merged = mount.MergeDirectoryEntries(L"mix").entries;

        Assert::IsFalse(merged.empty(),
            L"A directory empty in the upper must still list the lower's child.txt");
        Assert::IsTrue(merged.count(L"child.txt") == 1);
    }

    TEST_METHOD(AllLowerChildrenWhitedOut_MergedViewEmpty_AllowsDelete) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"box");
        env.WriteFile(env.Lower(0), L"box\\a.txt", "a");
        env.WriteFile(env.Lower(0), L"box\\b.txt", "b");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        Assert::IsTrue(wm.CreateWhiteout(L"box\\a.txt", WhiteoutType::File));
        Assert::IsTrue(wm.CreateWhiteout(L"box\\b.txt", WhiteoutType::File));

        ::LayerMount::LayerMount mount(config);
        const MergedDirectory merged = mount.MergeDirectoryEntries(L"box");

        AssertStatus(STATUS_SUCCESS, merged.status, L"The merge of a readable directory must succeed");
        Assert::IsTrue(merged.entries.empty(),
            L"With every lower child whited-out, the merged dir view is empty");
    }

    TEST_METHOD(DeleteShadowedDir_WhiteoutHidesLowerSubtree) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"sd");
        env.WriteFile(env.Lower(0), L"sd\\inner.txt", "inner-lower");
        env.CreateDir(env.Upper(),  L"sd");
        auto config = env.MakeConfig();
        Assert::IsTrue(WhiteoutManager(config, nullptr).SetOpaque(L"sd"));

        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ::LayerMount::LayerMount mount(config);
        const MergedDirectory merged = mount.MergeDirectoryEntries(L"sd");
        AssertStatus(STATUS_SUCCESS, merged.status, L"The merge of a readable directory must succeed");
        Assert::IsTrue(merged.entries.empty());

        SimulateDirDelete(resolver, wm, cache, L"sd");

        Assert::IsFalse(env.FileExists(env.Upper(), L"sd"));
        Assert::IsTrue(wm.HasWhiteout(L"sd", env.Upper()));

        Assert::IsFalse(resolver.ResolvePath(L"sd\\inner.txt").Found());
    }

    TEST_METHOD(DeleteLowerOnlyDir_WhiteoutCreatedOnly) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"lo_only");
        env.WriteFile(env.Lower(0), L"lo_only\\inner.txt", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        SimulateDirDelete(resolver, wm, cache, L"lo_only");

        Assert::IsTrue(env.FileExists(env.Lower(0), L"lo_only"));
        Assert::IsTrue(wm.HasWhiteout(L"lo_only", env.Upper()));
        Assert::IsFalse(resolver.ResolvePath(L"lo_only\\inner.txt").Found());
    }

    TEST_METHOD(DeleteLowerFile_InvalidatesCachedResolution) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"cached.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        Assert::IsTrue(resolver.ResolvePath(L"cached.txt").Found());
        Assert::IsTrue(cache.Get(L"cached.txt").has_value());

        SimulateFileDelete(resolver, wm, cache, L"cached.txt");
        Assert::IsFalse(cache.Get(L"cached.txt").has_value());
    }

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
        auto merged = mount.MergeDirectoryEntries(L"").entries;

        Assert::IsTrue(merged.count(L"visible.txt") == 1,
            L"Other lower files should remain visible");
        Assert::IsTrue(merged.count(L"hide.txt") == 0,
            L"Whited-out lower file must not appear in merged listing");
        Assert::IsTrue(merged.count(L".wh.hide.txt") == 0,
            L"Whiteout marker file must itself be filtered from the listing");
    }

    TEST_METHOD(CanDelete_DirOpaqueInLowerWithEntries_ReturnsDirectoryNotEmpty) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"sub"), "");
        env.WriteFile(env.Lower(0), L"sub\\own.txt", "lower0");

        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_DIRECTORY_NOT_EMPTY, mount.CanDelete(L"sub", 0),
            L"A directory that is opaque in a lower and has entries there is not empty");
    }

    TEST_METHOD(CanDelete_UpperDirUnreadable_ReturnsTheScanFailure) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        env.WriteFile(env.Lower(0), L"sub\\x.txt", "lower0");
        DirectoryListingDenied denied(env.Upper() + L"\\sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        BackupPrivilegeDisabledOnThread noBackupPrivilege;
        AssertListingDenied(env.Upper() + L"\\sub");

        AssertStatus(STATUS_ACCESS_DENIED, mount.CanDelete(L"sub", 0),
            L"A directory the upper cannot list must not count as empty");
    }

    TEST_METHOD(CanDelete_LowerDirUnreadable_ReturnsTheScanFailure) {
        TempLayerEnvironment env(2);
        env.CreateDir(env.Lower(0), L"sub");
        env.WriteFile(env.Lower(1), L"sub\\x.txt", "lower1");
        DirectoryListingDenied denied(env.Lower(0) + L"\\sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        BackupPrivilegeDisabledOnThread noBackupPrivilege;
        AssertListingDenied(env.Lower(0) + L"\\sub");

        AssertStatus(STATUS_ACCESS_DENIED, mount.CanDelete(L"sub", 0),
            L"A directory a lower cannot list must not count as empty");
    }

    TEST_METHOD(CanDeleteContext_UpperDirUnreadable_ReturnsTheScanFailure) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        env.WriteFile(env.Lower(0), L"sub\\x.txt", "lower0");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        Assert::IsTrue(NT_SUCCESS(mount.Open(L"sub", FILE_READ_ATTRIBUTES | DELETE,
                                             FILE_DIRECTORY_FILE, 0, &ctx, &info)),
            L"The directory opens before its listing is denied");

        NTSTATUS status = STATUS_SUCCESS;
        {
            DirectoryListingDenied denied(env.Upper() + L"\\sub");
            BackupPrivilegeDisabledOnThread noBackupPrivilege;
            AssertListingDenied(env.Upper() + L"\\sub");
            status = mount.CanDelete(ctx.get());
        }
        mount.Close(ctx.get());

        AssertStatus(STATUS_ACCESS_DENIED, status, L"An open directory the upper cannot list must not count as empty");
    }

    TEST_METHOD(CanDeleteContext_LowerDirUnreadable_ReturnsTheScanFailure) {
        TempLayerEnvironment env(2);
        env.CreateDir(env.Lower(0), L"sub");
        env.WriteFile(env.Lower(1), L"sub\\x.txt", "lower1");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        Assert::IsTrue(NT_SUCCESS(mount.Open(L"sub", FILE_READ_ATTRIBUTES | DELETE,
                                             FILE_DIRECTORY_FILE, 0, &ctx, &info)),
            L"The directory opens before its listing is denied");

        NTSTATUS status = STATUS_SUCCESS;
        {
            DirectoryListingDenied denied(env.Lower(0) + L"\\sub");
            BackupPrivilegeDisabledOnThread noBackupPrivilege;
            AssertListingDenied(env.Lower(0) + L"\\sub");
            status = mount.CanDelete(ctx.get());
        }
        mount.Close(ctx.get());

        AssertStatus(STATUS_ACCESS_DENIED, status, L"An open directory a lower cannot list must not count as empty");
    }

    TEST_METHOD(Delete_EmptyUpperDirUnreadable_FailsAndKeepsTheLowerEntry) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"sub");
        env.WriteFile(env.Lower(0), L"sub\\x.txt", "lower0");
        DirectoryListingDenied denied(env.Upper() + L"\\sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        NTSTATUS status = STATUS_SUCCESS;
        {
            BackupPrivilegeDisabledOnThread noBackupPrivilege;
            AssertListingDenied(env.Upper() + L"\\sub");
            status = mount.Delete(L"sub", 0);
        }

        AssertStatus(STATUS_ACCESS_DENIED, status,
            L"Deleting a directory the upper cannot list must fail with the scan's status");
        Assert::IsTrue(env.FileExists(env.Upper(), L"sub"),
            L"A failed delete must leave the upper directory");
        auto config = env.MakeConfig();
        Assert::IsFalse(WhiteoutManager(config, nullptr).HasWhiteout(L"sub", env.Upper()),
            L"A failed delete must write no whiteout");
        Assert::IsTrue(mount.MergeDirectoryEntries(L"sub").entries.count(L"x.txt") == 1,
            L"The lower's entry must stay in the listing");
    }
};

}
