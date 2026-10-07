#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataStore.h"
#include "WorkDirectory.h"

#include "AclTestHelpers.h"

#include <sddl.h>

#include <cstdlib>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::AccessDenied;
using LayerMountTestShared::AddDenyAce;
using LayerMountTestShared::AceSpec;
using LayerMountTestShared::DaclWithOneMoreAce;
using LayerMountTestShared::LocalAcl;
using LayerMountTestShared::WellKnownSid;
using LayerMountTestShared::BackupPrivilegeDisabledOnThread;
using LayerMountTestShared::DirectoryListingDenied;
using LayerMountTestShared::AssertListingDenied;
using LayerMountTestShared::ForEachAllowOrDenyAce;
using LayerMountTestShared::NewDirectoryError;
using LayerMountTestShared::NewFileError;

namespace LayerMountTests {

TEST_CLASS(RenameMoveTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
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
        abi::EventEmitter events;
        DirectoryRename dirRename(config, resolver, wm, cache, cu, events);

        const NTSTATUS st = dirRename.RenameUpperDirectory(
            {CallerPath(L"src"), CallerPath(L"dst")},
            EntryKind::Directory, ReplaceExisting::No);
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
        abi::EventEmitter events;
        DirectoryRename dirRename(config, resolver, wm, cache, cu, events);

        Assert::IsTrue(NT_SUCCESS(dirRename.RenameUpperDirectory(
            {CallerPath(L"src"), CallerPath(L"dst")},
            EntryKind::Directory, ReplaceExisting::No)));

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
        abi::EventEmitter events;
        DirectoryRename dirRename(config, resolver, wm, cache, cu, events);

        AssertStatus(STATUS_SUCCESS, wm.SetOpaque(L"src"), L"SetOpaque must mark the upper directory");
        Assert::IsTrue(wm.IsOpaque(L"src"));

        Assert::IsTrue(NT_SUCCESS(dirRename.RenameUpperDirectory(
            {CallerPath(L"src"), CallerPath(L"dst")},
            EntryKind::Directory, ReplaceExisting::No)));

        Assert::IsFalse(wm.IsOpaque(L"src"),
            L"Opacity should no longer be reported for the vanished source path");
        Assert::IsTrue(wm.IsOpaque(L"dst"),
            L"Opaque marker must travel with the directory");
    }

    TEST_METHOD(LowerOnlyDirRename_RecursiveCopyAndOpaque) {
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
        abi::EventEmitter events;
        DirectoryRename dirRename(config, resolver, wm, cache, cu, events);

        Assert::IsTrue(NT_SUCCESS(dirRename.RenameLowerDirectory(
            {CallerPath(L"ld"), CallerPath(L"newdir")},
            EntryKind::Directory, ReplaceExisting::No).status));

        Assert::IsTrue(env.FileExists(env.Upper(), L"newdir\\top.txt"));
        Assert::IsTrue(env.FileExists(env.Upper(), L"newdir\\nested\\inner.txt"));
        Assert::AreEqual(std::string("inner"),
                         env.ReadFile(env.Upper(), L"newdir\\nested\\inner.txt"));

        Assert::IsTrue(wm.IsOpaque(L"newdir"));

        Assert::IsTrue(env.FileExists(env.Lower(0), L"ld\\top.txt"));
        Assert::IsTrue(env.FileExists(env.Lower(0), L"ld\\nested\\inner.txt"));
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
        abi::EventEmitter events;
        DirectoryRename dirRename(config, resolver, wm, cache, cu, events);

        const NTSTATUS st = dirRename.RenameUpperDirectory(
            {CallerPath(L"src"), CallerPath(L"dst")},
            EntryKind::Directory, ReplaceExisting::No);
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
        abi::EventEmitter events;
        DirectoryRename dirRename(config, resolver, wm, cache, cu, events);

        const NTSTATUS st = dirRename.RenameLowerDirectory(
            {CallerPath(L"src"), CallerPath(L"dst")},
            EntryKind::Directory, ReplaceExisting::No).status;
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
        AssertStatus(STATUS_SUCCESS, wm_CreateWhiteoutHelper(env, L"dst"),
                     L"CreateWhiteout must succeed");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);
        abi::EventEmitter events;
        DirectoryRename dirRename(config, resolver, wm, cache, cu, events);

        const NTSTATUS st = dirRename.RenameUpperDirectory(
            {CallerPath(L"src"), CallerPath(L"dst")},
            EntryKind::Directory, ReplaceExisting::No);
        Assert::IsTrue(NT_SUCCESS(st),
            L"Whited-out destination is invisible in merged view — rename "
            L"without replace must succeed, not collision.");
    }

    TEST_METHOD(DirRename_EntryAtTheNewUpperName_FailsWithCollisionAndKeepsIt) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"src\\sub\\inner.txt", "inner");
        env.CreateDir(env.Upper(), L"dst");
        env.WriteFile(env.Upper(), L"dst\\sub", "upper dst file");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);
        abi::EventEmitter events;
        DirectoryRename dirRename(config, resolver, wm, cache, cu, events);

        // ReplaceExisting::Yes skips the merged-view collision check, so the
        // call reaches the move while dst is still in the upper.
        const NTSTATUS st = dirRename.RenameLowerDirectory(
            {CallerPath(L"src"), CallerPath(L"dst")},
            EntryKind::Directory, ReplaceExisting::Yes).status;
        Assert::AreEqual(
            static_cast<long>(STATUS_OBJECT_NAME_COLLISION),
            static_cast<long>(st),
            L"The rename fails when an upper entry holds the new name");

        Assert::AreEqual(std::string("upper dst file"), env.ReadFile(env.Upper(), L"dst\\sub"),
            L"The entry at the new name keeps its file");
        Assert::IsFalse(env.FileExists(env.Upper(), L"dst\\sub\\inner.txt"));
        Assert::IsFalse(wm.IsOpaque(L"dst"));
        Assert::IsFalse(wm.HasWhiteout(L"src", env.Upper()));
        Assert::IsTrue(EntriesUnder(env.Staging()).empty(),
            L"The failed rename leaves nothing in the work directory");
    }

private:
    static NTSTATUS wm_CreateWhiteoutHelper(TempLayerEnvironment& env,
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

// The size in a SetInfoRequest that leaves the size of the file unchanged.
constexpr UINT64 kUnchangedSize = UINT64_MAX;

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

bool RootListsAsDirectory(const MergedDirectory& rootListing, const std::wstring& displayName) {
    return (rootListing.entries.at(ListingKey(displayName)).findData.dwFileAttributes &
            FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void AssertRootShowsOnlyFileNamed(const ::LayerMount::LayerMount& mount, const std::wstring& displayName) {
    AssertOnlyEntryShownAs(mount, L"", displayName);
    Assert::IsFalse(RootListsAsDirectory(mount.MergeDirectoryEntries(L""), displayName),
        (L"The root must list " + displayName + L" as a file").c_str());
}

enum class ListedAs {
    File,
    Directory,
};

struct ListedEntry {
    std::wstring displayName;
    ListedAs kind;
};

void AssertRootShowsTwoEntries(const ::LayerMount::LayerMount& mount,
                               const ListedEntry& first,
                               const ListedEntry& second) {
    const MergedDirectory listing = mount.MergeDirectoryEntries(L"");
    AssertStatus(STATUS_SUCCESS, listing.status, L"The root listing must succeed");
    Assert::AreEqual(size_t{2}, listing.entries.size(),
        (L"The root must hold only " + first.displayName + L" and " + second.displayName).c_str());
    for (const ListedEntry& entry : {first, second}) {
        AssertEntryShownAs(mount, L"", ListingKey(entry.displayName), entry.displayName);
        const bool isDirectory = entry.kind == ListedAs::Directory;
        Assert::AreEqual(isDirectory, RootListsAsDirectory(listing, entry.displayName),
            (L"The root must list " + entry.displayName +
             (isDirectory ? L" as a directory" : L" as a file")).c_str());
    }
}

void AssertRootShowsFileAndDirectory(const ::LayerMount::LayerMount& mount,
                                     const std::wstring& fileName,
                                     const std::wstring& directoryName) {
    AssertRootShowsTwoEntries(mount, ListedEntry{fileName, ListedAs::File},
                              ListedEntry{directoryName, ListedAs::Directory});
}

enum class LowerChildHiding {
    Whiteout,
    OpaqueMarker,
};

// Writes a lower dst\old.txt and an upper dst that hides it, so dst shows
// no child.
void WriteDestinationHidingItsLowerChild(const TempLayerEnvironment& env,
                                         LowerChildHiding hiding) {
    env.WriteFile(env.Lower(0), L"dst\\old.txt", "old");
    const std::wstring marker = hiding == LowerChildHiding::Whiteout
        ? WhiteoutMarkerPath(L"dst\\old.txt")
        : OpaqueMarkerPath(L"dst");
    env.WriteFile(env.Upper(), marker, "");
}

void AssertDestinationStillHidesItsLowerChild(const TempLayerEnvironment& env,
                                              const ::LayerMount::LayerMount& mount) {
    Assert::IsTrue(env.FileExists(env.Upper(), L"dst"),
        L"The failed rename must leave the upper dst");
    const MergedDirectory listing = mount.MergeDirectoryEntries(L"dst");
    AssertStatus(STATUS_SUCCESS, listing.status, L"The listing of dst must succeed");
    Assert::IsTrue(listing.entries.empty(),
        L"The failed rename must keep the lower child of dst hidden");
}

void AssertNoOpaqueMarkerIn(const std::wstring& directory) {
    Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
        ::GetFileAttributesW(OpaqueMarkerPath(directory).c_str()),
        L"The rename must write no opaque marker file into the link target");
    Assert::IsFalse(MetadataStore::HasOpaqueMetadata(directory, nullptr),
        L"The rename must write no opaque stream onto the link target");
}

void AssertRootShowsOnlyNewNameOverWhiteout(const TempLayerEnvironment& env,
                                            const ::LayerMount::LayerMount& mount,
                                            const std::wstring& oldName,
                                            const std::wstring& newName) {
    Assert::IsTrue(env.FileExists(env.Upper(), WhiteoutMarkerPath(oldName)),
        L"The rename must leave a whiteout at the old name");
    AssertOnlyEntryShownAs(mount, L"", newName);
}

// Closes ctx after the rename, so a caller can compare the layers afterward.
void AssertReplaceRenameThroughHandleRefused(::LayerMount::LayerMount& mount,
                                             FileContext* ctx,
                                             const std::wstring& newPath,
                                             NTSTATUS expectedStatus,
                                             const wchar_t* message) {
    const NTSTATUS status = mount.Rename(ctx, newPath, kReplaceIfExists, kNoCallerPid);
    BY_HANDLE_FILE_INFORMATION handleInfo{};
    const bool handleWorks = ctx->handle != INVALID_HANDLE_VALUE &&
        ::GetFileInformationByHandle(ctx->handle, &handleInfo) != FALSE;
    const bool needsReopen = ctx->handleNeedsReopen;
    mount.Close(ctx);

    AssertStatus(expectedStatus, status, message);
    Assert::IsTrue(handleWorks, L"The refused rename must leave the handle open");
    Assert::IsFalse(needsReopen, L"The refused rename must not mark the handle for a reopen");
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

// The target that BuildLinkOverDirectory gives the link holds fromtarget.txt
// and sub.
void AssertListsOnlyTheLinkTarget(const ::LayerMount::LayerMount& mount, const std::wstring& dir) {
    const MergedDirectory listing = mount.MergeDirectoryEntries(dir);
    AssertStatus(STATUS_SUCCESS, listing.status,
        (L"The listing of '" + dir + L"' must succeed").c_str());
    Assert::AreEqual(size_t{2}, listing.entries.size(),
        (L"The listing of '" + dir + L"' must hold the two entries of the link target").c_str());
    Assert::IsTrue(listing.entries.count(L"fromtarget.txt") == 1,
        (L"The listing of '" + dir + L"' must hold the link target's file").c_str());
    Assert::IsTrue(listing.entries.count(L"sub") == 1,
        (L"The listing of '" + dir + L"' must hold the link target's directory").c_str());
}

constexpr UINT32 kCapabilitiesWithoutReparsePoints =
    LM_CAP_ADS | LM_CAP_SPARSE_FILES | LM_CAP_MULTIPLE_STREAMS | LM_CAP_NTFS_ACLS;

void AssertLowerLinkRenameCopiesTheLink(LinkCreator createLink, const std::wstring& oldName,
                                        const std::wstring& newName,
                                        UINT32 hostCapabilities) {
    TempLayerEnvironment env(1);
    env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
    const std::wstring target = env.Root() + L"\\target";
    if (!LinkCreatedOrSkipped(createLink, env.Lower(0) + L"\\" + oldName, target)) {
        return;
    }
    LayerConfig config = env.MakeConfig();
    config.hostCapabilities = hostCapabilities;
    ::LayerMount::LayerMount mount(config);
    const LayerSnapshot targetBefore(target);

    AssertStatus(STATUS_SUCCESS, mount.Rename(oldName, newName, kFailIfExists, kNoCallerPid),
        (L"The rename of " + oldName + L" to " + newName + L" must succeed").c_str());

    Assert::IsTrue(HasAttribute(env.Upper() + L"\\" + newName, FILE_ATTRIBUTE_REPARSE_POINT),
        (L"The upper " + newName + L" must be a link").c_str());
    AssertNoOpaqueMarkerIn(target);
    targetBefore.AssertUnchanged(L"The rename must not change the link target's entries");
    AssertListedDirectoryWithChild(mount, newName, L"inside.txt");
}

void AssertRenamedLowerLinkKeepsItsId(LinkCreator createLink, LinkTarget targetKind,
                                      const std::wstring& oldName, const std::wstring& newName,
                                      UINT32 hostCapabilities) {
    TempLayerEnvironment env(1);
    const std::wstring lowerLink = env.Lower(0) + L"\\" + oldName;
    if (!LinkToTargetCreatedOrSkipped(env, createLink, targetKind, lowerLink)) {
        return;
    }
    const UINT64 lowerLinkId = NtfsFileIdOf(lowerLink, LinkOpen::Itself);
    LayerConfig config = env.MakeConfig();
    config.hostCapabilities = hostCapabilities;
    ::LayerMount::LayerMount mount(config);

    AssertStatus(STATUS_SUCCESS, mount.Rename(oldName, newName, kFailIfExists, kNoCallerPid),
        (L"The rename of " + oldName + L" to " + newName + L" must succeed").c_str());

    Assert::AreEqual(lowerLinkId,
        IndexNumberThroughMount(mount, newName, FILE_OPEN_REPARSE_POINT),
        (L"An open of " + newName + L" as a link must report the lower link's file ID").c_str());
}

// A file symlink named flink and a junction named jlink in lowerDir. A host
// that cannot create a file symlink has only the junction.
struct LowerLinks {
    bool hasFileLink;
    bool hasJunction;
};

LowerLinks CreateLowerLinks(const TempLayerEnvironment& env, const std::wstring& lowerDir) {
    return {LinkToTargetCreatedOrSkipped(env, CreateFileSymlink, LinkTarget::File,
                                         lowerDir + L"\\flink"),
            LinkCreatedOrSkipped(CreateDirectoryJunction, lowerDir + L"\\jlink",
                                 LinkTargetPath(env, LinkTarget::Directory))};
}

// Denies FILE_ADD_FILE on the upper root and disables SE_BACKUP_NAME and
// SE_RESTORE_NAME on the thread. A whiteout marker then cannot go into the
// upper root, but a directory or a link can still move there. Declare it
// after the mount, because the mount enables the privileges on the process.
class UpperRootRefusesNewFiles {
public:
    explicit UpperRootRefusesNewFiles(const std::wstring& upper)
        : addFileDenied_(upper, FILE_ADD_FILE) {
        DisableRestorePrivilegeOnThread();
        const std::wstring probe = upper + L"\\probe";
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, NewDirectoryError(probe),
            L"The upper root must still take a new directory");
        Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, NewFileError(probe),
            L"The upper root must refuse a new file");
    }

    UpperRootRefusesNewFiles(const UpperRootRefusesNewFiles&) = delete;
    UpperRootRefusesNewFiles& operator=(const UpperRootRefusesNewFiles&) = delete;

private:
    AccessDenied addFileDenied_;
    BackupPrivilegeDisabledOnThread noBackupPrivilege_;
};

// Denies deniedAccess on the directory at path and disables SE_BACKUP_NAME
// and SE_RESTORE_NAME on the thread. deniedAccess must hold FILE_ADD_FILE,
// so the directory refuses a new file. When deniedAccess also holds
// FILE_ADD_SUBDIRECTORY, the directory refuses a new directory too. Declare
// it after the mount, because the mount enables the privileges on the
// process, and with them the deny does not apply.
class DirectoryRefusesNewEntries {
public:
    DirectoryRefusesNewEntries(const std::wstring& path, DWORD deniedAccess)
        : denied_(path, deniedAccess) {
        DisableRestorePrivilegeOnThread();
        const std::wstring probe = path + L"\\probe";
        Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, NewFileError(probe),
            (path + L" must refuse a new file").c_str());
        if ((deniedAccess & FILE_ADD_SUBDIRECTORY) != 0) {
            Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, NewDirectoryError(probe),
                (path + L" must refuse a new directory").c_str());
        }
    }

    DirectoryRefusesNewEntries(const DirectoryRefusesNewEntries&) = delete;
    DirectoryRefusesNewEntries& operator=(const DirectoryRefusesNewEntries&) = delete;

private:
    AccessDenied denied_;
    BackupPrivilegeDisabledOnThread noBackupPrivilege_;
};

// The LM_EVT_WARNING events a mount emits, each with its HRESULT and path.
struct EmittedWarnings {
    std::vector<HRESULT> results;
    std::vector<std::wstring> paths;
};

void LM_CALL CollectWarning(const LM_EVENT* evt, void* context) {
    if (evt->type != LM_EVT_WARNING) {
        return;
    }
    auto* warnings = static_cast<EmittedWarnings*>(context);
    warnings->results.push_back(evt->hr);
    warnings->paths.push_back(evt->relativePath != nullptr ? evt->relativePath : L"");
}

// The names of the files in directory that match pattern.
std::vector<std::wstring> FileNamesMatching(const std::wstring& directory,
                                            const std::wstring& pattern) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW((directory + L"\\" + pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return names;
    }
    do {
        names.push_back(fd.cFileName);
    } while (::FindNextFileW(find, &fd));
    ::FindClose(find);
    return names;
}

template <typename Predicate>
bool WorkHoldsEntryWhere(const TempLayerEnvironment& env, Predicate matches) {
    const fs::recursive_directory_iterator entries(env.Staging());
    return std::any_of(fs::begin(entries), fs::end(entries), matches);
}

bool WorkHoldsFileWithContent(const TempLayerEnvironment& env, const std::string& content) {
    return WorkHoldsEntryWhere(env, [&](const fs::directory_entry& entry) {
        return entry.is_regular_file() &&
               env.ReadFile(env.Staging(), fs::relative(entry.path(), env.Staging()).wstring()) ==
                   content;
    });
}

bool WorkHoldsFileNamed(const TempLayerEnvironment& env, const std::wstring& fileName) {
    return WorkHoldsEntryWhere(env, [&](const fs::directory_entry& entry) {
        return entry.is_regular_file() && entry.path().filename() == fileName;
    });
}

// Writes a merged src with a lower file, an upper file and an upper
// subdirectory.
void WriteMergedSrc(const TempLayerEnvironment& env) {
    env.WriteFile(env.Lower(0), L"src\\old.txt", "old");
    env.WriteFile(env.Upper(), L"src\\a.txt", "a");
    env.CreateDir(env.Upper(), L"src\\sub");
}

// Renames the src that WriteMergedSrc wrote to dst. An open child keeps the
// upper src from moving aside, and the deny ACEs that the merged copy takes
// from src keep the copy at dst from being removed.
NTSTATUS RenameSrcToDstWhenItsUpperCannotMoveAsideAndItsCopyCannotGo(
    const TempLayerEnvironment& env, ::LayerMount::LayerMount& mount, BOOLEAN replace) {
    const AccessDenied copyOfSubRefusesDelete(env.Upper() + L"\\src\\sub", DELETE);
    const AccessDenied copyOfSrcRefusesChildDelete(env.Upper() + L"\\src", FILE_DELETE_CHILD);
    const BackupPrivilegeDisabledOnThread noBackupPrivilege;
    DisableRestorePrivilegeOnThread();
    const ScopedHandle openChildBlocksMoveOfSrc =
        HoldOpen(env.Upper() + L"\\src\\a.txt", FILE_SHARE_READ | FILE_SHARE_WRITE);
    return mount.Rename(L"src", L"dst", replace, kNoCallerPid);
}

// Writes a record with stableIndexNumber for the upper entry at path.
void WriteStableIndexRecord(const std::wstring& path, uint64_t stableIndexNumber,
                            const LayerConfig& config) {
    LayerMountMetadata record;
    record.hasStableIndexNumber = true;
    record.stableIndexNumber = stableIndexNumber;
    Assert::IsTrue(MetadataStore::WriteLayerMountMetadata(path, record, &config),
        (L"WriteLayerMountMetadata must write the record of " + path).c_str());
}

// The sidecar record that HoldRecordAtFirstWhiteout holds open.
struct RecordHeldAtWhiteout {
    std::wstring sidecarDir;
    std::wstring heldName;
    ScopedHandle held;
    EmittedWarnings warnings;
};

// At the first LM_EVT_WHITEOUT_CREATED event, opens the first .meta.json
// file in hold->sidecarDir without FILE_SHARE_DELETE, so that record cannot
// move. Collects each LM_EVT_WARNING event in hold->warnings. Does not
// assert, because the engine calls it.
void LM_CALL HoldRecordAtFirstWhiteout(const LM_EVENT* evt, void* context) {
    auto* hold = static_cast<RecordHeldAtWhiteout*>(context);
    CollectWarning(evt, &hold->warnings);
    if (evt->type != LM_EVT_WHITEOUT_CREATED || !hold->heldName.empty()) {
        return;
    }
    hold->heldName = StoredLeafName(hold->sidecarDir + L"\\*.meta.json");
    hold->held.Reset(::CreateFileW((hold->sidecarDir + L"\\" + hold->heldName).c_str(),
                                   GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

// Returns one "(type;flags;mask;sid)" group for an ACE.
std::wstring AceText(BYTE type, BYTE flags, ACCESS_MASK mask, PSID sid) {
    LPWSTR sidString = nullptr;
    Assert::IsTrue(::ConvertSidToStringSidW(sid, &sidString) != FALSE,
        L"The test must convert the SID of an ACE");
    const std::wstring text = L"(" + std::to_wstring(type) + L";" + std::to_wstring(flags) +
                              L";" + std::to_wstring(mask) + L";" + sidString + L")";
    ::LocalFree(sidString);
    return text;
}

// Returns the explicit allow and deny ACEs in the DACL of path, in order,
// as one AceText group each.
std::wstring ExplicitAcesOf(const std::wstring& path) {
    std::wstring aces;
    ForEachAllowOrDenyAce(path, [&](const ACE_HEADER& header, ACCESS_MASK mask, PSID sid) {
        if ((header.AceFlags & INHERITED_ACE) != 0) {
            return;
        }
        aces += AceText(header.AceType, header.AceFlags, mask, sid);
    });
    return aces;
}

// The size is past what NTFS keeps in the file record, so a full copy
// allocates clusters and a metacopy shell has none.
std::string LowerFileData() {
    return "lower file data" + std::string(64 * 1024, 'R');
}

std::string LowerFileDataStart() {
    return LowerFileData().substr(0, kReadThroughMountBytes);
}

void AssertMetacopyShell(const std::wstring& upperPath, const LayerConfig& config) {
    Assert::IsTrue(MetadataStore::ReadLayerMountMetadata(upperPath, &config).metacopy,
        (L"The copy-up record of " + upperPath + L" must have the metacopy flag").c_str());
    Assert::AreEqual(0LL, AllocatedBytes(upperPath),
        (L"The volume must allocate no data for " + upperPath).c_str());
}

void AssertFullCopy(const TempLayerEnvironment& env, const std::wstring& name,
                    const LayerConfig& config) {
    const std::wstring upperPath = env.Upper() + L"\\" + name;
    Assert::IsFalse(MetadataStore::ReadLayerMountMetadata(upperPath, &config).metacopy,
        (L"The copy-up record of " + upperPath + L" must not have the metacopy flag").c_str());
    Assert::IsTrue(LowerFileData() == env.ReadFile(env.Upper(), name),
        (upperPath + L" must hold the lower file's data").c_str());
}

NTSTATUS SetDaclWithOneMoreAce(::LayerMount::LayerMount& mount, const std::wstring& path,
                               const std::wstring& templatePath, const AceSpec& ace) {
    const LocalAcl merged = DaclWithOneMoreAce(templatePath, ace);
    SECURITY_DESCRIPTOR sd{};
    Assert::IsTrue(::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) != FALSE &&
                   ::SetSecurityDescriptorDacl(&sd, TRUE, merged.get(), FALSE) != FALSE,
        L"The test must build the security descriptor");
    return mount.SetSecurity(path, DACL_SECURITY_INFORMATION, &sd, kNoCallerPid);
}

SetInfoRequest LastWriteTimeChange() {
    const FILETIME written = MakeFileTime(2020, 1, 10);
    return SetInfoRequest{INVALID_FILE_ATTRIBUTES, 0, 0,
                          ComposeUInt64(written.dwHighDateTime, written.dwLowDateTime), 0,
                          kUnchangedSize, kUnchangedSize};
}

// Sets the times of LastWriteTimeChange through a handle opened with access,
// and returns the status of the set.
NTSTATUS SetTimesThroughHandle(::LayerMount::LayerMount& mount, const std::wstring& path,
                               UINT32 access) {
    std::unique_ptr<FileContext> ctx;
    InternalFileInfo info{};
    AssertStatus(STATUS_SUCCESS,
        mount.Open(path, access, kNoCreateOptions, kNoCallerPid, &ctx, &info),
        (L"The open of " + path + L" must succeed").c_str());
    const NTSTATUS status = mount.SetInfo(ctx.get(), LastWriteTimeChange(), nullptr);
    mount.Close(ctx.get());
    return status;
}

void OpenForWriteThroughMount(::LayerMount::LayerMount& mount, const std::wstring& path) {
    std::unique_ptr<FileContext> ctx;
    InternalFileInfo info{};
    AssertStatus(STATUS_SUCCESS,
        mount.Open(path, FILE_WRITE_DATA, kNoCreateOptions, kNoCallerPid, &ctx, &info),
        (L"The open of " + path + L" for write must succeed").c_str());
    mount.Close(ctx.get());
}

const std::wstring kZoneStream = L"Zone.Identifier";
const std::string kZoneStreamData = "[ZoneTransfer]\r\nZoneId=3\r\n";

void WriteLowerFileWithZoneStream(const TempLayerEnvironment& env, const std::wstring& name,
                                  const std::string& data) {
    env.WriteFile(env.Lower(0), name, data);
    env.WriteFile(env.Lower(0), name + L":" + kZoneStream, kZoneStreamData);
}

void AssertZoneStreamShown(::LayerMount::LayerMount& mount, const std::wstring& name) {
    Assert::AreEqual(kZoneStreamData, ReadThroughMount(mount, name + L":" + kZoneStream),
        (L"A read of the stream of " + name + L" must return the stream's data").c_str());
    std::vector<InternalStreamInfo> streams;
    AssertStatus(STATUS_SUCCESS, mount.EnumerateStreams(name, streams),
        (L"The stream listing of " + name + L" must succeed").c_str());
    Assert::AreEqual(size_t{1}, streams.size(),
        (L"The stream listing of " + name + L" must hold one stream").c_str());
    Assert::AreEqual(L":" + kZoneStream + L":$DATA", streams[0].name,
        (L"The stream listing of " + name + L" must show the lower file's stream").c_str());
}

bool LinkToPrimedTargetCreatedOrSkipped(const TempLayerEnvironment& env,
                                        LayerSource linkSource,
                                        LinkCreator createLink) {
    env.WriteFile(env.Root(), L"target\\primer", "");
    return LinkCreatedOrSkipped(createLink, LinkLayerPath(env, linkSource) + L"\\link",
                                env.Root() + L"\\target");
}

void RenamePrimerToCopyLowerLinkUp(::LayerMount::LayerMount& mount, LayerSource linkSource) {
    if (linkSource == LayerSource::Upper) {
        return;
    }
    AssertStatus(STATUS_SUCCESS,
        mount.Rename(L"link\\primer", L"link\\primed", kFailIfExists, kNoCallerPid),
        L"Preconditions: a rename within the lower link must copy the link up");
}

std::vector<std::wstring> TargetEntriesBesidesPrimerAndPrimed(const TempLayerEnvironment& env) {
    std::vector<std::wstring> entries = EntriesUnder(env.Root() + L"\\target");
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const std::wstring& entry) {
                                     return entry == L"primer" || entry == L"primed";
                                 }),
                  entries.end());
    return entries;
}

void AssertTargetHolds(const TempLayerEnvironment& env,
                       const std::vector<std::wstring>& expected,
                       const wchar_t* message) {
    Assert::IsTrue(TargetEntriesBesidesPrimerAndPrimed(env) == expected, message);
}

// A replace-rename of from onto to while the work directory refuses new
// entries.
NTSTATUS RenameWhileWorkRefusesEntries(const TempLayerEnvironment& env,
                                       ::LayerMount::LayerMount& mount,
                                       const std::wstring& from,
                                       const std::wstring& to) {
    const DirectoryRefusesNewEntries workRefusesNewEntries(
        env.Staging(), FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
    return mount.Rename(from, to, kReplaceIfExists, kNoCallerPid);
}

// A replace-rename of the open file in ctx onto to while the work directory
// refuses new entries.
NTSTATUS RenameWhileWorkRefusesEntries(const TempLayerEnvironment& env,
                                       ::LayerMount::LayerMount& mount,
                                       FileContext* ctx,
                                       const std::wstring& to) {
    const DirectoryRefusesNewEntries workRefusesNewEntries(
        env.Staging(), FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
    return mount.Rename(ctx, to, kReplaceIfExists, kNoCallerPid);
}

// The attributes of the entry that handle holds open, or
// INVALID_FILE_ATTRIBUTES when the read fails.
DWORD AttributesOfHeld(const ScopedHandle& handle) {
    FILE_BASIC_INFO info{};
    if (!::GetFileInformationByHandleEx(handle.Get(), FileBasicInfo, &info, sizeof(info))) {
        return INVALID_FILE_ATTRIBUTES;
    }
    return info.FileAttributes;
}

// A junction or a directory symbolic link, and the name under the
// environment root that it points to.
struct LinkToNoDirectory {
    LinkCreator createLink;
    const wchar_t* target;
};

// Directory links that lead to no directory. The environment root holds no
// "missing", and "f.txt" is a file there.
constexpr LinkToNoDirectory kLinksToNoDirectory[] = {
    {CreateDirectorySymlink, L"missing"},
    {CreateDirectoryJunction, L"missing"},
    {CreateDirectorySymlink, L"f.txt"},
    {CreateDirectoryJunction, L"f.txt"},
};

// Creates link as the entry l at the overlay root, in the layer that
// linkSource selects. Renames x, a file in the lower and then in the upper, to l\b
// with each value of replaceIfExists. Asserts that each rename fails with
// STATUS_OBJECT_PATH_NOT_FOUND and changes nothing.
void AssertRenameUnderRootLinkFailsWithPathNotFound(LayerSource linkSource,
                                                    const LinkToNoDirectory& link) {
    for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
        for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Root(), L"f.txt", "f");
            env.WriteFile(LayerRoot(env, sourceLayer), L"x", "x");
            if (!LinkCreatedOrSkipped(link.createLink, LinkLayerPath(env, linkSource) + L"\\l",
                                      env.Root() + L"\\" + link.target)) {
                continue;
            }
            ::LayerMount::LayerMount mount(env.MakeConfig());
            const LayerSnapshot upperBefore(env.Upper());
            const LayerSnapshot lowerBefore(env.Lower(0));
            const std::wstring message =
                std::wstring(L"A rename of x to a path under the root link to ") + link.target +
                L" must fail";

            AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
                mount.Rename(L"x", L"l\\b", replace, kNoCallerPid),
                message.c_str());
            upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
            lowerBefore.AssertUnchanged(L"The failed rename must write nothing in the lower");
            Assert::AreEqual(std::string("x"), env.ReadFile(LayerRoot(env, sourceLayer), L"x"),
                L"The failed rename must keep x where it was");
        }
    }
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

    TEST_METHOD(Rename_LowerDirectoryIntoParentInNoLayer_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"x\\a.txt", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"x", L"NewParent\\y", kFailIfExists, kNoCallerPid),
            L"A rename into a parent in no layer must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
        AssertOnlyEntryShownAs(mount, L"", L"x");
    }

    TEST_METHOD(Rename_LowerFileIntoParentInNoLayer_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"b.txt", "y");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"b.txt", L"NewParent\\b.txt", kFailIfExists, kNoCallerPid),
            L"A rename of a file into a parent in no layer must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
        AssertOnlyEntryShownAs(mount, L"", L"b.txt");
    }

    TEST_METHOD(Rename_LowerFileIntoWhitedOutLowerDirectory_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo\\a.txt", "x");
        env.WriteFile(env.Lower(0), L"b.txt", "y");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"Foo"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"b.txt", L"foo\\b.txt", kFailIfExists, kNoCallerPid),
            L"A rename into a whited-out directory must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_LowerFileUnderLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        env.WriteFile(env.Lower(0), L"b.txt", "y");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"b.txt", L"foo\\b.txt", kFailIfExists, kNoCallerPid),
            L"A rename to a path under a lower file must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_LowerDirectoryUnderLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        env.WriteFile(env.Lower(0), L"x\\a.txt", "y");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"x", L"foo\\x", kFailIfExists, kNoCallerPid),
            L"A rename of a directory to a path under a lower file must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_LowerFileUnderWhitedOutLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        env.WriteFile(env.Lower(0), L"b.txt", "y");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"Foo"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"b.txt", L"foo\\b.txt", kFailIfExists, kNoCallerPid),
            L"A rename to a path under a whited-out lower file must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_LowerDirectoryIntoDirectoryHiddenByOpaqueAncestor_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\Sub\\a.txt", "x");
        env.WriteFile(env.Lower(0), L"x\\b.txt", "y");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"x", L"d\\sub\\x", kFailIfExists, kNoCallerPid),
            L"A rename into a directory that an opaque ancestor hides must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_UpperDirectoryIntoLowerOnlyParent_CopiesTheParentUpInTheLowersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo\\a.txt", "x");
        env.WriteFile(env.Upper(), L"bar\\c.txt", "z");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"bar", L"foo\\bar", kFailIfExists, kNoCallerPid),
            L"The rename of bar to foo\\bar must succeed");
        AssertListedDirectoryWithChild(mount, L"Foo", L"a.txt");
        AssertListedDirectoryWithChild(mount, L"Foo", L"bar");
        AssertUpperDirectoryCopiedUp(env, L"Foo");
    }

    TEST_METHOD(Rename_LowerDirectoryIntoLowerDirectoryThatAnUpperJunctionHides_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), WhiteoutMarkerPath(L"target\\sub"), "");
        env.WriteFile(env.Lower(0), L"link\\sub\\a.txt", "x");
        env.WriteFile(env.Lower(0), L"x\\b.txt", "y");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Upper() + L"\\link",
                                  env.Root() + L"\\target")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());
        const LayerSnapshot targetBefore(env.Root() + L"\\target");

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"x", L"link\\sub\\x", kFailIfExists, kNoCallerPid),
            L"A rename into a lower directory that an upper junction hides must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
        targetBefore.AssertUnchanged(L"The failed rename must write nothing in the junction target");
    }

    TEST_METHOD(Rename_UpperDirectoryIntoParentInNoLayer_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"bar\\c.txt", "z");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"bar", L"NewParent\\bar", kFailIfExists, kNoCallerPid),
            L"A rename into a parent in no layer must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
        AssertOnlyEntryShownAs(mount, L"", L"bar");
    }

    TEST_METHOD(Rename_UpperDirectoryIntoWhitedOutLowerDirectory_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo\\a.txt", "x");
        env.WriteFile(env.Upper(), L"bar\\c.txt", "z");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"Foo"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"bar", L"foo\\bar", kFailIfExists, kNoCallerPid),
            L"A rename into a whited-out directory must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_UpperDirectoryIntoDirectoryHiddenByOpaqueAncestor_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\Sub\\a.txt", "x");
        env.WriteFile(env.Upper(), L"bar\\c.txt", "z");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"bar", L"d\\sub\\bar", kFailIfExists, kNoCallerPid),
            L"A rename into a directory that an opaque ancestor hides must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_UpperDirectoryUnderLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        env.WriteFile(env.Upper(), L"bar\\c.txt", "z");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            mount.Rename(L"bar", L"foo\\bar", kFailIfExists, kNoCallerPid),
            L"A rename of a directory to a path under a lower file must fail");
        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
    }

    TEST_METHOD(Rename_LowerJunctionWithoutReparseSupport_CopiesTheLinkWithoutAnOpaqueMarker) {
        AssertLowerLinkRenameCopiesTheLink(
            CreateDirectoryJunction, L"link", L"moved", kCapabilitiesWithoutReparsePoints);
    }

    TEST_METHOD(Rename_LowerDirectorySymlinkWithoutReparseSupport_CopiesTheLinkWithoutAnOpaqueMarker) {
        AssertLowerLinkRenameCopiesTheLink(
            CreateDirectorySymlink, L"link", L"moved", kCapabilitiesWithoutReparsePoints);
    }

    TEST_METHOD(CaseOnlyRename_LowerJunctionWithoutReparseSupport_CopiesTheLinkWithoutAnOpaqueMarker) {
        AssertLowerLinkRenameCopiesTheLink(
            CreateDirectoryJunction, L"Link", L"LINK", kCapabilitiesWithoutReparsePoints);
    }

    TEST_METHOD(CaseOnlyRename_LowerDirectorySymlinkWithoutReparseSupport_CopiesTheLinkWithoutAnOpaqueMarker) {
        AssertLowerLinkRenameCopiesTheLink(
            CreateDirectorySymlink, L"Link", L"LINK", kCapabilitiesWithoutReparsePoints);
    }

    TEST_METHOD(CaseOnlyRename_LowerJunctionWithReparseSupport_CopiesTheLinkWithoutAnOpaqueMarker) {
        AssertLowerLinkRenameCopiesTheLink(
            CreateDirectoryJunction, L"Link", L"LINK", kDefaultHostCapabilities);
    }

    TEST_METHOD(Rename_LowerFileSymlink_KeepsTheLowerLinkId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            AssertRenamedLowerLinkKeepsItsId(
                CreateFileSymlink, LinkTarget::File, L"link", L"moved", capabilities);
        });
    }

    TEST_METHOD(Rename_LowerDirectorySymlink_KeepsTheLowerLinkId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            AssertRenamedLowerLinkKeepsItsId(
                CreateDirectorySymlink, LinkTarget::Directory, L"link", L"moved", capabilities);
        });
    }

    TEST_METHOD(Rename_LowerJunction_KeepsTheLowerLinkId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            AssertRenamedLowerLinkKeepsItsId(
                CreateDirectoryJunction, LinkTarget::Directory, L"link", L"moved", capabilities);
        });
    }

    TEST_METHOD(CaseOnlyRename_LowerJunction_KeepsTheLowerLinkId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            AssertRenamedLowerLinkKeepsItsId(
                CreateDirectoryJunction, LinkTarget::Directory, L"Link", L"LINK", capabilities);
        });
    }

    TEST_METHOD(Rename_LowerDirectoryHoldingLinks_KeepsEachEntrysId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
            const std::wstring lowerDir = env.Lower(0) + L"\\d";
            const LowerLinks links = CreateLowerLinks(env, lowerDir);
            if (!links.hasJunction) {
                return;
            }
            const UINT64 fileId = NtfsFileIdOf(lowerDir + L"\\f.txt", LinkOpen::Follow);
            const UINT64 fileLinkId =
                links.hasFileLink ? NtfsFileIdOf(lowerDir + L"\\flink", LinkOpen::Itself) : 0;
            const UINT64 junctionId = NtfsFileIdOf(lowerDir + L"\\jlink", LinkOpen::Itself);
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the lower directory must succeed");

            Assert::AreEqual(fileId, IndexNumberThroughMount(mount, L"e\\f.txt", kNoCreateOptions),
                L"The copied file must report the lower file's ID");
            Assert::AreEqual(junctionId,
                IndexNumberThroughMount(mount, L"e\\jlink", FILE_OPEN_REPARSE_POINT),
                L"The copied junction must report the lower link's file ID");
            if (links.hasFileLink) {
                Assert::AreEqual(fileLinkId,
                    IndexNumberThroughMount(mount, L"e\\flink", FILE_OPEN_REPARSE_POINT),
                    L"The copied file symlink must report the lower link's file ID");
            }
        });
    }

    TEST_METHOD(Rename_UpperDirectoryHoldingCopiedUpFile_KeepsTheFilesId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"x.txt", "lower");
            env.CreateDir(env.Upper(), L"d");
            const UINT64 lowerId = NtfsFileIdOf(env.Lower(0) + L"\\x.txt", LinkOpen::Follow);
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SUCCESS,
                mount.Rename(L"x.txt", L"d\\x.txt", kFailIfExists, kNoCallerPid),
                L"The rename of the lower file into the upper directory must succeed");
            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the upper directory must succeed");

            Assert::AreEqual(lowerId, IndexNumberThroughMount(mount, L"e\\x.txt", kNoCreateOptions),
                L"The file must keep the lower file's ID through both renames");
        });
    }

    TEST_METHOD(Rename_MergedDirectoryHoldingCopiedUpEntries_KeepsEachEntrysId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
            env.WriteFile(env.Lower(0), L"d\\sub\\g.txt", "nested");
            env.WriteFile(env.Lower(0), L"d\\lazy.bin", "lazy lower data");
            const std::wstring lowerDir = env.Lower(0) + L"\\d";
            const UINT64 fileId = NtfsFileIdOf(lowerDir + L"\\f.txt", LinkOpen::Follow);
            const UINT64 subId = NtfsFileIdOf(lowerDir + L"\\sub", LinkOpen::Follow);
            const UINT64 nestedId = NtfsFileIdOf(lowerDir + L"\\sub\\g.txt", LinkOpen::Follow);
            const UINT64 lazyId = NtfsFileIdOf(lowerDir + L"\\lazy.bin", LinkOpen::Follow);
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            {
                CopyUpAndRenameRig rig(config);
                AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(L"d\\f.txt"),
                    L"The copy-up of the lower file must succeed");
                AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpDirectory(L"d\\sub"),
                    L"The copy-up of the lower subdirectory must succeed");
                AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(L"d\\sub\\g.txt"),
                    L"The copy-up of the nested lower file must succeed");
                AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpMetadataOnly(L"d\\lazy.bin").status,
                    L"The metadata-only copy-up of the lower file must succeed");
            }
            env.WriteFile(env.Upper(), L"d\\new.txt", "upper");
            ::LayerMount::LayerMount mount(config);
            const UINT64 newFileId = IndexNumberThroughMount(mount, L"d\\new.txt", kNoCreateOptions);

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the merged directory must succeed");

            Assert::AreEqual(fileId, IndexNumberThroughMount(mount, L"e\\f.txt", kNoCreateOptions),
                L"The copied-up file must report the lower file's ID");
            Assert::AreEqual(subId, IndexNumberThroughMount(mount, L"e\\sub", kNoCreateOptions),
                L"The copied-up subdirectory must report the lower subdirectory's ID");
            Assert::AreEqual(nestedId,
                IndexNumberThroughMount(mount, L"e\\sub\\g.txt", kNoCreateOptions),
                L"The copied-up nested file must report the lower file's ID");
            Assert::AreEqual(lazyId, IndexNumberThroughMount(mount, L"e\\lazy.bin", kNoCreateOptions),
                L"The metacopy shell must report the lower file's ID");
            Assert::AreEqual(newFileId, IndexNumberThroughMount(mount, L"e\\new.txt", kNoCreateOptions),
                L"The file new in the upper must keep the ID it had before the rename");
            Assert::AreEqual(std::string("lazy lower data"), ReadThroughMount(mount, L"e\\lazy.bin"),
                L"The metacopy shell must read the lower file's data");
        });
    }

    TEST_METHOD(Rename_MergedDirectoryHoldingCopiedUpLinks_KeepsEachLinksId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
            const std::wstring lowerDir = env.Lower(0) + L"\\d";
            const LowerLinks links = CreateLowerLinks(env, lowerDir);
            if (!links.hasJunction) {
                return;
            }
            const UINT64 fileLinkId =
                links.hasFileLink ? NtfsFileIdOf(lowerDir + L"\\flink", LinkOpen::Itself) : 0;
            const UINT64 junctionId = NtfsFileIdOf(lowerDir + L"\\jlink", LinkOpen::Itself);
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            {
                CopyUpAndRenameRig rig(config);
                AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpDirectory(L"d\\jlink"),
                    L"The copy-up of the lower junction must succeed");
                if (links.hasFileLink) {
                    AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(L"d\\flink"),
                        L"The copy-up of the lower file symlink must succeed");
                }
            }
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the merged directory must succeed");

            Assert::AreEqual(junctionId,
                IndexNumberThroughMount(mount, L"e\\jlink", FILE_OPEN_REPARSE_POINT),
                L"The copied-up junction must report the lower link's file ID");
            if (links.hasFileLink) {
                Assert::AreEqual(fileLinkId,
                    IndexNumberThroughMount(mount, L"e\\flink", FILE_OPEN_REPARSE_POINT),
                    L"The copied-up file symlink must report the lower link's file ID");
            }
        });
    }

    TEST_METHOD(Rename_MergedDirectoryWithNestedDeletedLowerFile_KeepsTheFileHidden) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\sub\\x.txt", "deleted");
            env.WriteFile(env.Lower(0), L"d\\sub\\y.txt", "kept");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);
            AssertStatus(STATUS_SUCCESS, mount.Delete(L"d\\sub\\x.txt", kNoCallerPid),
                L"The delete of the nested lower file must succeed");

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the merged directory must succeed");

            AssertOnlyEntryShownAs(mount, L"e\\sub", L"y.txt");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e\\sub\\x.txt"),
                L"An open of the deleted file under the new name must find nothing");
        });
    }

    TEST_METHOD(Rename_MergedDirectoryWithNestedOpaqueDirectory_ShowsOnlyItsUpperChildren) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
            env.WriteFile(env.Lower(0), L"d\\sub\\x.txt", "lower");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);
            AssertStatus(STATUS_SUCCESS, mount.Delete(L"d\\sub\\x.txt", kNoCallerPid),
                L"The delete of the nested lower file must succeed");
            AssertStatus(STATUS_SUCCESS, mount.Delete(L"d\\sub", kNoCallerPid),
                L"The delete of the emptied lower subdirectory must succeed");
            AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d\\sub", FILE_DIRECTORY_FILE),
                L"The create of the subdirectory over its whiteout must succeed");
            AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d\\sub\\new.txt", kNoCreateOptions),
                L"The create of a file in the new subdirectory must succeed");

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the merged directory must succeed");

            AssertOnlyEntryShownAs(mount, L"e\\sub", L"new.txt");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e\\sub\\x.txt"),
                L"An open of the hidden lower file under the new name must find nothing");
        });
    }

    TEST_METHOD(Rename_MergedDirectoryWithNestedTypeConflicts_ShowsTheUpperEntries) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\sub\\lowerDir\\lower.txt", "lower");
            env.WriteFile(env.Lower(0), L"d\\sub\\lowerFile", "lower");
            env.WriteFile(env.Upper(), L"d\\sub\\lowerDir", "upper file");
            env.WriteFile(env.Upper(), L"d\\sub\\lowerFile\\upper.txt", "upper");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the merged directory must succeed");

            Assert::AreEqual(std::string("upper file"), ReadThroughMount(mount, L"e\\sub\\lowerDir"),
                L"The upper file over the lower directory must read the upper data");
            AssertOnlyEntryShownAs(mount, L"e\\sub\\lowerFile", L"upper.txt");
        });
    }

    TEST_METHOD(Rename_MergedDirectoryWithUpperDirectoryOverLowerJunction_WritesNothingThroughTheJunction) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
            env.WriteFile(env.Root(), L"target\\keep.txt", "target");
            const std::wstring target = env.Root() + L"\\target";
            if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Lower(0) + L"\\d\\j", target)) {
                return;
            }
            env.WriteFile(env.Upper(), L"d\\j\\upper.txt", "upper");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);
            const LayerSnapshot targetBefore(target);

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the merged directory must succeed");

            AssertOnlyEntryShownAs(mount, L"e\\j", L"upper.txt");
            targetBefore.AssertUnchanged(L"The rename must write nothing into the junction's target");
        });
    }

    TEST_METHOD(Rename_MergedDirectoryWithNestedOpaqueMetadataOnly_ShowsOnlyItsUpperChildren) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
            env.WriteFile(env.Lower(0), L"d\\sub\\x.txt", "lower");
            env.WriteFile(env.Upper(), L"d\\sub\\new.txt", "upper");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            Assert::IsTrue(MetadataStore::SetOpaqueMetadata(env.Upper() + L"\\d\\sub", &config),
                L"The opaque metadata must mark the upper subdirectory");
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the merged directory must succeed");

            AssertOnlyEntryShownAs(mount, L"e\\sub", L"new.txt");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e\\sub\\x.txt"),
                L"An open of the hidden lower file under the new name must find nothing");
        });
    }

    TEST_METHOD(Rename_MergedDirectoryWithWhitedOutUpperSubdirectory_ShowsOnlyItsUpperChildren) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
        env.WriteFile(env.Lower(0), L"d\\sub\\x.txt", "lower");
        env.WriteFile(env.Upper(), L"d\\sub\\new.txt", "upper");
        env.WriteFile(env.Upper(), L"d\\.wh.sub", "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\sub\\x.txt"),
            L"An open of the hidden lower file under the old name must find nothing");

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the merged directory must succeed");

        AssertOnlyEntryShownAs(mount, L"e\\sub", L"new.txt");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e\\sub\\x.txt"),
            L"An open of the hidden lower file under the new name must find nothing");
    }

    TEST_METHOD(Rename_LowerDirectoryWithFileWhitedOutInTheSameLower_KeepsTheFileHidden) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\sub\\x.txt", "hidden");
        env.WriteFile(env.Lower(0), L"d\\sub\\.wh.x.txt", "");
        env.WriteFile(env.Lower(0), L"d\\sub\\y.txt", "kept");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        AssertOnlyEntryShownAs(mount, L"d\\sub", L"y.txt");

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the lower directory must succeed");

        AssertOnlyEntryShownAs(mount, L"e\\sub", L"y.txt");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e\\sub\\x.txt"),
            L"An open of the whited-out file under the new name must find nothing");
    }

    TEST_METHOD(Rename_DirectoryInTwoLowers_ShowsTheEntriesOfBothLowers) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(2);
            env.WriteFile(env.Lower(0), L"d\\a.txt", "first");
            env.WriteFile(env.Lower(1), L"d\\B.txt", "second");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the directory in two lowers must succeed");

            const MergedDirectory listing = mount.MergeDirectoryEntries(L"e");
            AssertStatus(STATUS_SUCCESS, listing.status, L"The listing of e must succeed");
            Assert::AreEqual(size_t{2}, listing.entries.size(), L"The listing of e must hold two entries");
            AssertEntryShownAs(mount, L"e", L"a.txt", L"a.txt");
            AssertEntryShownAs(mount, L"e", L"b.txt", L"B.txt");
            Assert::AreEqual(std::string("second"), ReadThroughMount(mount, L"e\\B.txt"),
                L"The file from the second lower must read the second lower's data");
        });
    }

    TEST_METHOD(Rename_DirectoryInTwoLowers_MergesTheSubdirectoryOfBothLowers) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(2);
            env.WriteFile(env.Lower(0), L"d\\sub\\a.txt", "first");
            env.WriteFile(env.Lower(1), L"d\\sub\\b.txt", "second");
            env.WriteFile(env.Lower(1), L"d\\sub\\deep\\c.txt", "deep");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the directory in two lowers must succeed");

            const MergedDirectory listing = mount.MergeDirectoryEntries(L"e\\sub");
            AssertStatus(STATUS_SUCCESS, listing.status, L"The listing of e\\sub must succeed");
            Assert::AreEqual(size_t{3}, listing.entries.size(),
                L"The listing of e\\sub must hold the entries of both lowers");
            AssertEntryShownAs(mount, L"e\\sub", L"a.txt", L"a.txt");
            AssertEntryShownAs(mount, L"e\\sub", L"b.txt", L"b.txt");
            AssertOnlyEntryShownAs(mount, L"e\\sub\\deep", L"c.txt");
            Assert::AreEqual(std::string("deep"), ReadThroughMount(mount, L"e\\sub\\deep\\c.txt"),
                L"The nested file from the second lower must read the second lower's data");
        });
    }

    TEST_METHOD(Rename_DirectoryInTwoLowers_StillHidesTheEntryThatAWhiteoutInTheFirstLowerHides) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(2);
            env.WriteFile(env.Lower(0), L"d\\sub\\a.txt", "first");
            env.WriteFile(env.Lower(0), WhiteoutMarkerPath(L"d\\sub\\b.txt"), "");
            env.WriteFile(env.Lower(1), L"d\\sub\\b.txt", "second");
            env.WriteFile(env.Lower(1), L"d\\c.txt", "second");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);
            AssertOnlyEntryShownAs(mount, L"d\\sub", L"a.txt");

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the directory in two lowers must succeed");

            AssertEntryShownAs(mount, L"e", L"c.txt", L"c.txt");
            AssertOnlyEntryShownAs(mount, L"e\\sub", L"a.txt");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e\\sub\\b.txt"),
                L"An open of the whited-out file under the new name must find nothing");
        });
    }

    TEST_METHOD(Rename_DirectoryInTwoLowers_StillHidesTheSecondLowersChildrenOfAnOpaqueSubdirectory) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(2);
            env.WriteFile(env.Lower(0), L"d\\sub\\a.txt", "first");
            env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"d\\sub"), "");
            env.WriteFile(env.Lower(1), L"d\\sub\\b.txt", "second");
            env.WriteFile(env.Lower(1), L"d\\c.txt", "second");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);
            AssertOnlyEntryShownAs(mount, L"d\\sub", L"a.txt");

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the directory in two lowers must succeed");

            AssertEntryShownAs(mount, L"e", L"c.txt", L"c.txt");
            AssertOnlyEntryShownAs(mount, L"e\\sub", L"a.txt");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e\\sub\\b.txt"),
                L"An open of the hidden file under the new name must find nothing");
        });
    }

    TEST_METHOD(Rename_MergedDirectoryWithUpperSubdirectoryOverFirstLowerWhiteout_StillHidesTheSecondLowersChildren) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L"d\\a.txt", "first");
        env.WriteFile(env.Lower(0), WhiteoutMarkerPath(L"d\\sub"), "");
        env.WriteFile(env.Lower(1), L"d\\sub\\x.txt", "second");
        env.WriteFile(env.Lower(1), L"d\\sub\\deep\\y.txt", "second");
        env.WriteFile(env.Upper(), L"d\\sub\\u.txt", "upper");
        env.WriteFile(env.Upper(), L"d\\sub\\deep\\v.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\sub\\x.txt"),
            L"An open of the file under the whited-out subdirectory must find nothing");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\sub\\deep\\y.txt"),
            L"An open of the file under the whited-out ancestor must find nothing");

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the merged directory must succeed");

        const MergedDirectory listing = mount.MergeDirectoryEntries(L"e\\sub");
        AssertStatus(STATUS_SUCCESS, listing.status, L"The listing of e\\sub must succeed");
        Assert::AreEqual(size_t{2}, listing.entries.size(),
            L"The listing of e\\sub must hold only the upper entries");
        AssertEntryShownAs(mount, L"e\\sub", L"u.txt", L"u.txt");
        AssertEntryShownAs(mount, L"e\\sub", L"deep", L"deep");
        AssertOnlyEntryShownAs(mount, L"e\\sub\\deep", L"v.txt");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e\\sub\\x.txt"),
            L"An open of the second lower's file under the new name must find nothing");
    }

    TEST_METHOD(Rename_DirectoryWithFirstLowerJunctionOverSecondLowerDirectory_CopiesTheJunction) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Root(), L"target\\inside.txt", "target");
        env.WriteFile(env.Lower(0), L"d\\a.txt", "first");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Lower(0) + L"\\d\\sub",
                                  env.Root() + L"\\target")) {
            return;
        }
        env.WriteFile(env.Lower(1), L"d\\sub\\second.txt", "second");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        AssertOnlyEntryShownAs(mount, L"d\\sub", L"inside.txt");

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the directory in two lowers must succeed");

        Assert::IsTrue(HasAttribute(env.Upper() + L"\\e\\sub", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper e\\sub must be a link");
        AssertOnlyEntryShownAs(mount, L"e\\sub", L"inside.txt");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e\\sub\\second.txt"),
            L"An open of the second lower's file under the copied junction must find nothing");
    }

    TEST_METHOD(Rename_MergedDirectoryOverTwoLowers_StillHidesTheSecondLowerEntryThatAnUpperWhiteoutHides) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), L"d\\a.txt", "first");
        env.WriteFile(env.Lower(1), L"d\\b.txt", "second");
        env.WriteFile(env.Lower(1), L"d\\c.txt", "second");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        AssertStatus(STATUS_SUCCESS, mount.Delete(L"d\\b.txt", kNoCallerPid),
            L"The delete of the file in the second lower must succeed");

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the merged directory must succeed");

        const MergedDirectory listing = mount.MergeDirectoryEntries(L"e");
        AssertStatus(STATUS_SUCCESS, listing.status, L"The listing of e must succeed");
        Assert::AreEqual(size_t{2}, listing.entries.size(), L"The listing of e must hold two entries");
        AssertEntryShownAs(mount, L"e", L"a.txt", L"a.txt");
        AssertEntryShownAs(mount, L"e", L"c.txt", L"c.txt");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e\\b.txt"),
            L"An open of the deleted file under the new name must find nothing");
    }

    TEST_METHOD(Rename_MergedDirectoryWithUnlistableUpperSubdirectory_FailsAndKeepsTheOldTree) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
        env.WriteFile(env.Lower(0), L"d\\sub\\x.txt", "lower");
        env.WriteFile(env.Upper(), L"d\\sub\\u.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        {
            DirectoryListingDenied denied(env.Upper() + L"\\d\\sub");
            BackupPrivilegeDisabledOnThread noBackupPrivilege;
            AssertListingDenied(env.Upper() + L"\\d\\sub");

            AssertStatus(STATUS_ACCESS_DENIED,
                mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"A rename that cannot list an upper subdirectory must fail");
        }

        AssertEntryShownAs(mount, L"d", L"f.txt", L"f.txt");
        AssertEntryShownAs(mount, L"d\\sub", L"x.txt", L"x.txt");
        AssertEntryShownAs(mount, L"d\\sub", L"u.txt", L"u.txt");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e"),
            L"An open of the new name after the failed rename must find nothing");
        Assert::IsFalse(env.FileExists(env.Upper(), L"e"),
            L"The failed rename must leave no new name in the upper");
    }

    TEST_METHOD(Rename_LowerDirectoryWithUnlistableSubdirectory_FailsAndKeepsTheOldTree) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
        env.WriteFile(env.Lower(0), L"d\\sub\\x.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        {
            DirectoryListingDenied denied(env.Lower(0) + L"\\d\\sub");
            BackupPrivilegeDisabledOnThread noBackupPrivilege;
            AssertListingDenied(env.Lower(0) + L"\\d\\sub");

            AssertStatus(STATUS_ACCESS_DENIED,
                mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"A rename that cannot list a lower subdirectory must fail");
        }

        AssertEntryShownAs(mount, L"d", L"f.txt", L"f.txt");
        AssertOnlyEntryShownAs(mount, L"d\\sub", L"x.txt");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e"),
            L"An open of the new name after the failed rename must find nothing");
        Assert::IsFalse(env.FileExists(env.Upper(), L"e"),
            L"The failed rename must leave no new name in the upper");
    }

    TEST_METHOD(Rename_LowerDirectoryWhosePartialCopyRefusesDeleteWithoutPrivileges_LeavesTheStagingAreaEmpty) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\a\\x.txt", "lower");
        env.WriteFile(env.Lower(0), L"d\\b\\y.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        {
            const AccessDenied fileRefusesDelete(env.Lower(0) + L"\\d\\a\\x.txt", DELETE);
            const AccessDenied directoryRefusesChildDelete(env.Lower(0) + L"\\d\\a",
                                                           FILE_DELETE_CHILD);
            const DirectoryListingDenied laterChildRefusesListing(env.Lower(0) + L"\\d\\b");
            const BackupPrivilegeDisabledOnThread noBackupPrivilege;
            DisableRestorePrivilegeOnThread();

            AssertStatus(STATUS_ACCESS_DENIED,
                mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"A rename that cannot list a later child must fail");
            Assert::IsTrue(EntriesUnder(env.Staging()).empty(),
                L"The failed rename must leave nothing in the staging area");
            std::unique_ptr<WorkDirectory> workDirectory;
            std::wstring error;
            Assert::AreEqual<HRESULT>(S_OK,
                WorkDirectory::Open(env.MakeConfig(), &workDirectory, error),
                (L"An open of the work directory after the failed rename must succeed: " +
                 error).c_str());
        }

        AssertOnlyEntryShownAs(mount, L"d\\a", L"x.txt");
        AssertOnlyEntryShownAs(mount, L"d\\b", L"y.txt");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e"),
            L"An open of the new name after the failed rename must find nothing");
    }

    TEST_METHOD(Rename_LowerDirectoryWithInheritableAceOverSubdirectoryThatDeniesListing_CopiesTheTree) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\sub\\x.txt", "lower");
        AddDenyAce(env.Lower(0) + L"\\d", FILE_WRITE_EA,
                   OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        {
            DirectoryListingDenied denied(env.Lower(0) + L"\\d\\sub");

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename must succeed when the engine holds backup privilege");
        }

        AssertOnlyEntryShownAs(mount, L"e\\sub", L"x.txt");
    }

    TEST_METHOD(Rename_LowerDirectoryWithSubdirectory_KeepsTheLastWriteTimesOfBoth) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\sub\\x.txt", "lower");
        const FILETIME subWrite = MakeFileTime(2011, 3, 4);
        const FILETIME dWrite = MakeFileTime(2012, 5, 6);
        StampTimes(env.Lower(0) + L"\\d\\sub", MakeFileTime(2010, 1, 2),
                   MakeFileTime(2011, 3, 5), subWrite);
        StampTimes(env.Lower(0) + L"\\d", MakeFileTime(2010, 1, 3),
                   MakeFileTime(2012, 5, 7), dWrite);
        ::LayerMount::LayerMount mount(env.MakeConfig());
        Assert::AreEqual(ComposeUInt64(dWrite.dwHighDateTime, dWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"d"),
            L"The overlay must show the stamped last-write time of d before the rename");

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the lower directory must succeed");

        Assert::AreEqual(ComposeUInt64(dWrite.dwHighDateTime, dWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"e"),
            L"The new name must keep the last-write time of the renamed directory");
        Assert::AreEqual(ComposeUInt64(subWrite.dwHighDateTime, subWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"e\\sub"),
            L"The subdirectory must keep its last-write time under the new name");
    }

    TEST_METHOD(Rename_MergedDirectory_GivesTheNewNameTheLastWriteTimeOfTheUpperDirectory) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
        env.WriteFile(env.Upper(), L"d\\u.txt", "upper");
        const FILETIME upperWrite = MakeFileTime(2014, 7, 8);
        StampTimes(env.Lower(0) + L"\\d", MakeFileTime(2013, 1, 2),
                   MakeFileTime(2013, 3, 4), MakeFileTime(2013, 5, 6));
        StampTimes(env.Upper() + L"\\d", MakeFileTime(2014, 1, 2),
                   MakeFileTime(2014, 3, 4), upperWrite);
        ::LayerMount::LayerMount mount(env.MakeConfig());
        Assert::AreEqual(ComposeUInt64(upperWrite.dwHighDateTime, upperWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"d"),
            L"The overlay must show the last-write time of the upper d before the rename");

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the merged directory must succeed");

        Assert::AreEqual(ComposeUInt64(upperWrite.dwHighDateTime, upperWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"e"),
            L"The new name must show the last-write time of the upper d");
    }

    TEST_METHOD(Rename_MergedDirectoryWithMergedSubdirectory_GivesItTheUpperSubdirectorysLastWriteTime) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\sub\\x.txt", "lower");
        env.WriteFile(env.Upper(), L"d\\sub\\u.txt", "upper");
        const FILETIME upperSubWrite = MakeFileTime(2018, 6, 7);
        StampTimes(env.Lower(0) + L"\\d\\sub", MakeFileTime(2019, 1, 2),
                   MakeFileTime(2019, 3, 4), MakeFileTime(2019, 5, 6));
        StampTimes(env.Upper() + L"\\d\\sub", MakeFileTime(2018, 1, 2),
                   MakeFileTime(2018, 3, 4), upperSubWrite);
        ::LayerMount::LayerMount mount(env.MakeConfig());
        Assert::AreEqual(ComposeUInt64(upperSubWrite.dwHighDateTime, upperSubWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"d\\sub"),
            L"The overlay must show the last-write time of the upper d\\sub before the rename");

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the merged directory must succeed");

        Assert::AreEqual(ComposeUInt64(upperSubWrite.dwHighDateTime, upperSubWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"e\\sub"),
            L"The merged subdirectory must show the last-write time of the upper d\\sub");
        AssertEntryShownAs(mount, L"e\\sub", L"x.txt", L"x.txt");
        AssertEntryShownAs(mount, L"e\\sub", L"u.txt", L"u.txt");
    }

    TEST_METHOD(Rename_MergedDirectoryWhoseLowerSubdirectoryRefusesNewEntries_GivesItTheUpperSubdirectorysDacl) {
        UNIT_SKIP_IF_NOT_ADMIN();
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\sub\\x.txt", "lower");
        env.WriteFile(env.Upper(), L"d\\sub\\u.txt", "upper");
        const DWORD deniedOnlyOnTheUpperSub = FILE_WRITE_EA;
        AddDenyAce(env.Upper() + L"\\d\\sub", deniedOnlyOnTheUpperSub, NO_INHERITANCE);
        const std::wstring upperSubAces = ExplicitAcesOf(env.Upper() + L"\\d\\sub");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        {
            DirectoryRefusesNewEntries lowerSubRefuses(env.Lower(0) + L"\\d\\sub",
                FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD);
            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the merged directory must succeed");
        }

        AssertEntryShownAs(mount, L"e\\sub", L"x.txt", L"x.txt");
        AssertEntryShownAs(mount, L"e\\sub", L"u.txt", L"u.txt");
        Assert::AreEqual(std::string("lower"), ReadThroughMount(mount, L"e\\sub\\x.txt"),
            L"The lower file must read through the new name");
        Assert::AreEqual(std::string("upper"), ReadThroughMount(mount, L"e\\sub\\u.txt"),
            L"The upper file must read through the new name");
        Assert::AreEqual(upperSubAces, ExplicitAcesOf(env.Upper() + L"\\e\\sub"),
            L"The merged subdirectory must carry the explicit ACEs of the upper d\\sub");
    }

    TEST_METHOD(Rename_MergedDirectoryWithCompressedUpperDirectory_CompressesTheNewName) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", "lower");
        env.WriteFile(env.Upper(), L"d\\u.txt", "upper");
        if (!EnableCompression(env.Upper() + L"\\d")) {
            Logger::WriteMessage(
                L"[SKIP] The volume refused FSCTL_SET_COMPRESSION on the upper directory");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the merged directory must succeed");

        Assert::IsTrue((FileAttributesThroughMount(mount, L"e") & FILE_ATTRIBUTE_COMPRESSED) != 0,
            L"The new name must be compressed like the upper d");
    }

    TEST_METHOD(Rename_MergedDirectoryWithCompressedUpperDirectory_KeepsTheLowerFileUncompressed) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\f.txt", std::string(64 * 1024, 'f'));
        env.WriteFile(env.Upper(), L"d\\u.txt", "upper");
        if (!EnableCompression(env.Upper() + L"\\d")) {
            Logger::WriteMessage(
                L"[SKIP] The volume refused FSCTL_SET_COMPRESSION on the upper directory");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        Assert::IsFalse((FileAttributesThroughMount(mount, L"d\\f.txt") & FILE_ATTRIBUTE_COMPRESSED) != 0,
            L"Precondition: the lower d\\f.txt must be uncompressed");

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the merged directory must succeed");

        Assert::IsFalse((FileAttributesThroughMount(mount, L"e\\f.txt") & FILE_ATTRIBUTE_COMPRESSED) != 0,
            L"The lower file must stay uncompressed under the new name");
        Assert::IsTrue((FileAttributesThroughMount(mount, L"e") & FILE_ATTRIBUTE_COMPRESSED) != 0,
            L"The new name must be compressed like the upper d");
    }

    TEST_METHOD(Rename_CloudPlaceholderLowerDirectoryWithUpperShadow_ShowsAPlainDirectoryHoldingBothLayers) {
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            CloudPlaceholderLayers layers{SyncRootLayer::Lower};
            if (!layers.PlaceholderWithFileOrSkipped(L"d", L"d\\x.txt", "lower")) {
                return;
            }
            TempLayerEnvironment& env = layers.env;
            env.WriteFile(env.Upper(), L"d\\sub\\u.txt", "upper");
            auto config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
                L"The rename of the cloud placeholder directory must succeed");

            Assert::AreEqual<UINT32>(0u,
                FileAttributesThroughMount(mount, L"e") & FILE_ATTRIBUTE_REPARSE_POINT,
                L"The new name must open as a plain directory");
            Assert::AreEqual(std::string("lower"), ReadThroughMount(mount, L"e\\x.txt"),
                L"The lower file must read through the new name");
            Assert::AreEqual(std::string("upper"), ReadThroughMount(mount, L"e\\sub\\u.txt"),
                L"The upper file must read through the new name");
            Cache cache;
            WhiteoutManager whiteouts(config, &cache);
            Assert::IsTrue(whiteouts.IsOpaque(L"e"), L"The new name must be opaque");
            Assert::AreEqual(0, _wcsicmp((env.Lower(0) + L"\\d").c_str(),
                MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\e", &config)
                    .originLayer.c_str()),
                L"The copy-up record of the new name must name the lower d");
            Assert::IsTrue(EntriesUnder(env.Staging()).empty(),
                L"The rename must leave nothing in the work directory");
        });
    }

    TEST_METHOD(Rename_CloudPlaceholderLowerDirectoryWithUpperShadow_GivesTheNewNameTheUpperDirectorysLastWriteTime) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        if (!layers.PlaceholderWithFileOrSkipped(L"d", L"d\\x.txt", "lower")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        env.WriteFile(env.Upper(), L"d\\sub\\u.txt", "upper");
        const FILETIME upperWrite = MakeFileTime(2016, 4, 5);
        StampTimes(env.Upper() + L"\\d", MakeFileTime(2016, 1, 2),
                   MakeFileTime(2016, 3, 4), upperWrite);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the cloud placeholder directory must succeed");

        Assert::AreEqual(ComposeUInt64(upperWrite.dwHighDateTime, upperWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"e"),
            L"The new name must show the last-write time of the upper d");
    }

    TEST_METHOD(Rename_CloudPlaceholderLowerDirectoryWithUpperShadow_GivesTheNewNameTheUpperDirectorysDacl) {
        UNIT_SKIP_IF_NOT_ADMIN();
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        if (!layers.PlaceholderWithFileOrSkipped(L"d", L"d\\x.txt", "lower")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        env.WriteFile(env.Upper(), L"d\\sub\\u.txt", "upper");
        AddDenyAce(env.Upper() + L"\\d", FILE_WRITE_EA, NO_INHERITANCE);
        const std::wstring upperAces = ExplicitAcesOf(env.Upper() + L"\\d");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the cloud placeholder directory must succeed");

        Assert::AreEqual(upperAces, ExplicitAcesOf(env.Upper() + L"\\e"),
            L"The new name must carry the explicit ACEs of the upper d");
    }

    TEST_METHOD(Rename_CloudPlaceholderLowerDirectoryWithUpperShadow_KeepsTheCompressionOfAnUpperChild) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        if (!layers.PlaceholderWithFileOrSkipped(L"d", L"d\\x.txt", "lower")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        env.WriteFile(env.Upper(), L"d\\sub\\u.txt", "upper");
        if (!EnableCompression(env.Upper() + L"\\d\\sub\\u.txt")) {
            Logger::WriteMessage(
                L"[SKIP] The volume refused FSCTL_SET_COMPRESSION on the upper file");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the cloud placeholder directory must succeed");

        Assert::IsTrue(HasAttribute(env.Upper() + L"\\e\\sub\\u.txt", FILE_ATTRIBUTE_COMPRESSED),
            L"The upper file must stay compressed under the new name");
        Assert::AreEqual(std::string("upper"), ReadThroughMount(mount, L"e\\sub\\u.txt"),
            L"The upper file must read through the new name");
    }

    TEST_METHOD(Rename_CloudPlaceholderLowerDirectory_ShowsAPlainDirectoryHoldingItsChild) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        if (!layers.PlaceholderWithFileOrSkipped(L"d", L"d\\x.txt", "lower")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the cloud placeholder directory must succeed");

        Assert::AreEqual<UINT32>(0u,
            FileAttributesThroughMount(mount, L"e") & FILE_ATTRIBUTE_REPARSE_POINT,
            L"The new name must open as a plain directory");
        Assert::AreEqual(std::string("lower"), ReadThroughMount(mount, L"e\\x.txt"),
            L"The lower file must read through the new name");
    }

    TEST_METHOD(Rename_CloudPlaceholderLowerDirectory_KeepsItsLastWriteTime) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        if (!layers.PlaceholderWithFileOrSkipped(L"d", L"d\\x.txt", "lower")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        const FILETIME lowerWrite = MakeFileTime(2012, 8, 9);
        StampTimes(env.Lower(0) + L"\\d", MakeFileTime(2012, 1, 2),
                   MakeFileTime(2012, 3, 4), lowerWrite);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the cloud placeholder directory must succeed");

        Assert::AreEqual(ComposeUInt64(lowerWrite.dwHighDateTime, lowerWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"e"),
            L"The new name must keep the last-write time of the lower d");
    }

    TEST_METHOD(Rename_LowerDirectoryWithCloudPlaceholderSubdirectory_ShowsAPlainSubdirectoryHoldingItsChild) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        if (!layers.PlaceholderWithFileOrSkipped(L"d\\p", L"d\\p\\y.txt", "nested")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the lower directory must succeed");

        Assert::AreEqual<UINT32>(0u,
            FileAttributesThroughMount(mount, L"e\\p") & FILE_ATTRIBUTE_REPARSE_POINT,
            L"The subdirectory must open as a plain directory under the new name");
        Assert::AreEqual(std::string("nested"), ReadThroughMount(mount, L"e\\p\\y.txt"),
            L"The child of the subdirectory must read through the new name");
    }

    TEST_METHOD(Rename_PinnedCloudPlaceholderLowerDirectory_GivesTheNewNameNoPinnedAttribute) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        if (!layers.PlaceholderWithFileOrSkipped(L"d", L"d\\x.txt", "lower") ||
            !layers.syncRoot.PinnedOrSkipped(layers.env.Lower(0) + L"\\d")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the pinned cloud placeholder directory must succeed");

        Assert::IsFalse(HasAttribute(env.Upper() + L"\\e", FILE_ATTRIBUTE_PINNED),
            L"The new name must not carry the pin state of the lower d");
    }

    TEST_METHOD(Rename_LowerDirectoryHoldingCloudPlaceholderFile_ShowsAPlainFileWithTheLowersData) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        if (!layers.PlaceholderFileOrSkipped(L"d\\x.txt", "lower")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the lower directory must succeed");

        Assert::AreEqual<UINT32>(0u,
            FileAttributesThroughMount(mount, L"e\\x.txt") & FILE_ATTRIBUTE_REPARSE_POINT,
            L"The placeholder file must open as a plain file under the new name");
        Assert::AreEqual(std::string("lower"), ReadThroughMount(mount, L"e\\x.txt"),
            L"The data of the placeholder file must read through the new name");
    }

    TEST_METHOD(Rename_LowerDirectoryHoldingPartlyDehydratedCloudPlaceholderFile_CopiesTheProvidersData) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        constexpr size_t kSize = 256 * 1024;
        if (!layers.DehydratedPlaceholderFileOrSkipped(L"d\\x.bin", kSize, ByteRange{64 * 1024, 128 * 1024}, CloudFetch::Serve)) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the lower directory must succeed");

        Assert::IsTrue(CloudProviderData(0, kSize) == env.ReadFile(env.Upper(), L"e\\x.bin"),
            L"The file under the new name must hold the data the provider serves");
    }

    TEST_METHOD(Rename_LowerDirectoryHoldingUnixSocketFile_KeepsTheSocketTagUnderTheNewName) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\socket", "");
        if (!MicrosoftReparseTagSetOrSkipped(env.Lower(0) + L"\\d\\socket",
                                             IO_REPARSE_TAG_AF_UNIX)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of the lower directory must succeed");

        Assert::AreEqual(static_cast<DWORD>(IO_REPARSE_TAG_AF_UNIX),
            ReparseTagOf(env.Upper() + L"\\e\\socket"),
            L"The socket file under the new name must carry the reparse tag of the lower");
    }

    TEST_METHOD(Rename_LowerDirectoryWithUnhandledReparseTag_FailsAndLeavesBothNamesAsTheyWere) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"d");
        if (!NonLinkReparseTagSetOrSkipped(env.Lower(0) + L"\\d")) {
            return;
        }
        env.WriteFile(env.Upper(), L"d\\sub\\u.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_IO_REPARSE_TAG_NOT_HANDLED,
            mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename of a directory that cannot be listed must fail");

        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"e"),
            L"Nothing must show at the new name");
        Assert::IsFalse(env.FileExists(env.Upper(), L"e"),
            L"Nothing must be at the new name in the upper");
        AssertStatus(STATUS_SUCCESS, OpenThroughMount(mount, L"d"),
            L"The old name must still open");
        Assert::AreEqual(std::string("upper"), ReadThroughMount(mount, L"d\\sub\\u.txt"),
            L"The upper file must still read through the old name");
    }

    TEST_METHOD(Rename_LowerDirectoryUnderParentDenyingWriteAttributes_KeepsTheFilesLastWriteTime) {
        UNIT_SKIP_IF_NOT_ADMIN();
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"p\\d\\f.txt", "lower");
        const FILETIME fileWrite = MakeFileTime(2017, 2, 3);
        StampTimes(env.Lower(0) + L"\\p\\d\\f.txt", MakeFileTime(2017, 1, 2),
                   MakeFileTime(2017, 2, 4), fileWrite);
        AddDenyAce(env.Lower(0) + L"\\p", FILE_WRITE_ATTRIBUTES,
                   OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"p\\d", L"p\\e", kFailIfExists, kNoCallerPid),
            L"The rename of the lower directory must succeed");

        Assert::AreEqual(ComposeUInt64(fileWrite.dwHighDateTime, fileWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"p\\e\\f.txt"),
            L"The file must keep its last-write time under the new name");
    }

    TEST_METHOD(Rename_UpperDirectoryOntoWhitedOutLowerDirectory_KeepsItsLastWriteTime) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"e\\old.txt", "lower");
        env.WriteFile(env.Upper(), L".wh.e", "");
        env.WriteFile(env.Upper(), L"d\\u.txt", "upper");
        const FILETIME dWrite = MakeFileTime(2015, 9, 10);
        StampTimes(env.Upper() + L"\\d", MakeFileTime(2015, 1, 2),
                   MakeFileTime(2015, 3, 4), dWrite);
        ::LayerMount::LayerMount mount(env.MakeConfig());
        Assert::AreEqual(ComposeUInt64(dWrite.dwHighDateTime, dWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"d"),
            L"The overlay must show the stamped last-write time of d before the rename");

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"d", L"e", kFailIfExists, kNoCallerPid),
            L"The rename onto the whited-out name must succeed");

        AssertOnlyEntryShownAs(mount, L"e", L"u.txt");
        Assert::AreEqual(ComposeUInt64(dWrite.dwHighDateTime, dWrite.dwLowDateTime),
            LastWriteTimeThroughMount(mount, L"e"),
            L"Marking the moved directory opaque must keep its last-write time");
    }

    TEST_METHOD(ReplaceRename_UpperFileOntoCopiedUpFile_ReportsTheMovedFilesId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"a.txt", "lower");
            env.WriteFile(env.Upper(), L"b.txt", "upper");
            const UINT64 movedId = NtfsFileIdOf(env.Upper() + L"\\b.txt", LinkOpen::Follow);
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);
            AssertStatus(STATUS_SUCCESS, mount.EnsureInUpperLayer(L"a.txt"),
                L"The copy-up of the lower file must succeed");

            AssertStatus(STATUS_SUCCESS,
                mount.Rename(L"b.txt", L"a.txt", kReplaceIfExists, kNoCallerPid),
                L"The replacing rename must succeed");

            Assert::AreEqual(movedId, IndexNumberThroughMount(mount, L"a.txt", kNoCreateOptions),
                L"The moved file must report its own ID, not the replaced file's");
        });
    }

    TEST_METHOD(Delete_CopiedUpFileThenCreateAtItsPath_ReportsTheNewFilesId) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"a.txt", "lower");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);
            AssertStatus(STATUS_SUCCESS, mount.EnsureInUpperLayer(L"a.txt"),
                L"The copy-up of the lower file must succeed");

            AssertStatus(STATUS_SUCCESS, mount.Delete(L"a.txt", kNoCallerPid),
                L"The delete of the copied-up file must succeed");
            AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"a.txt", kNoCreateOptions),
                L"The create at the deleted file's path must succeed");

            Assert::AreEqual(NtfsFileIdOf(env.Upper() + L"\\a.txt", LinkOpen::Follow),
                IndexNumberThroughMount(mount, L"a.txt", kNoCreateOptions),
                L"The new file must report its own ID, not the deleted file's");
        });
    }

    TEST_METHOD(Rename_LowerEntryToItsOwnName_LeavesTheUpperUntouched) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"Foo");
        env.WriteFile(env.Lower(0), L"Bar.txt", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
            AssertStatus(STATUS_SUCCESS, mount.Rename(L"Foo", L"Foo", replace, kNoCallerPid),
                L"The rename of Foo to Foo must succeed");
            AssertStatus(STATUS_SUCCESS, mount.Rename(L"Bar.txt", L"Bar.txt", replace, kNoCallerPid),
                L"The rename of Bar.txt to Bar.txt must succeed");
        }

        upperBefore.AssertUnchanged(L"A rename to the same name must not copy up or write a whiteout");
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

    TEST_METHOD(ReplaceRename_DirectoryOntoDirectoryWithAnUpperChild_FailsAndChangesNothing) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            TempLayerEnvironment env(1);
            env.WriteFile(LayerRoot(env, sourceLayer), L"src\\a.txt", "a");
            env.WriteFile(env.Upper(), L"dst\\extra.txt", "extra");
            ::LayerMount::LayerMount mount(env.MakeConfig());
            const LayerSnapshot upperBefore(env.Upper());

            AssertStatus(STATUS_DIRECTORY_NOT_EMPTY,
                mount.Rename(L"src", L"dst", kReplaceIfExists, kNoCallerPid),
                L"A replace rename onto a directory with an upper child must fail");
            upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
            AssertOnlyEntryShownAs(mount, L"src", L"a.txt");
            AssertOnlyEntryShownAs(mount, L"dst", L"extra.txt");
        }
    }

    TEST_METHOD(ReplaceRename_DirectoryOntoDirectoryWithALowerChild_FailsAndChangesNothing) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            TempLayerEnvironment env(1);
            env.WriteFile(LayerRoot(env, sourceLayer), L"src\\a.txt", "a");
            env.WriteFile(env.Lower(0), L"dst\\extra.txt", "extra");
            ::LayerMount::LayerMount mount(env.MakeConfig());
            const LayerSnapshot upperBefore(env.Upper());

            AssertStatus(STATUS_DIRECTORY_NOT_EMPTY,
                mount.Rename(L"src", L"dst", kReplaceIfExists, kNoCallerPid),
                L"A replace rename onto a directory with a lower child must fail");
            upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
            AssertOnlyEntryShownAs(mount, L"src", L"a.txt");
            AssertOnlyEntryShownAs(mount, L"dst", L"extra.txt");
        }
    }

    TEST_METHOD(ReplaceRename_DirectoryOntoEmptyUpperDirectory_ListsOnlyTheSourceChildren) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            TempLayerEnvironment env(1);
            env.WriteFile(LayerRoot(env, sourceLayer), L"src\\a.txt", "a");
            env.CreateDir(env.Upper(), L"dst");
            ::LayerMount::LayerMount mount(env.MakeConfig());

            AssertStatus(STATUS_SUCCESS,
                mount.Rename(L"src", L"dst", kReplaceIfExists, kNoCallerPid),
                L"A replace rename onto an empty directory must succeed");
            AssertOnlyEntryShownAs(mount, L"", L"dst");
            AssertOnlyEntryShownAs(mount, L"dst", L"a.txt");
        }
    }

    TEST_METHOD(ReplaceRename_DirectoryOntoDirectoryWithOnlyWhitedOutChildren_ListsOnlyTheSourceChildren) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            TempLayerEnvironment env(1);
            env.WriteFile(LayerRoot(env, sourceLayer), L"src\\a.txt", "a");
            env.WriteFile(env.Lower(0), L"dst\\old.txt", "old");
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"dst\\old.txt"), "");
            ::LayerMount::LayerMount mount(env.MakeConfig());

            AssertStatus(STATUS_SUCCESS,
                mount.Rename(L"src", L"dst", kReplaceIfExists, kNoCallerPid),
                L"A replace rename onto a directory that shows no children must succeed");
            AssertOnlyEntryShownAs(mount, L"", L"dst");
            AssertOnlyEntryShownAs(mount, L"dst", L"a.txt");
        }
    }

    TEST_METHOD(ReplaceRename_DirectoryOntoEmptyLowerOnlyDirectory_ListsOnlyTheSourceChildren) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            TempLayerEnvironment env(1);
            env.WriteFile(LayerRoot(env, sourceLayer), L"src\\a.txt", "a");
            env.CreateDir(env.Lower(0), L"dst");
            ::LayerMount::LayerMount mount(env.MakeConfig());

            AssertStatus(STATUS_SUCCESS,
                mount.Rename(L"src", L"dst", kReplaceIfExists, kNoCallerPid),
                L"A replace rename onto an empty lower directory must succeed");
            AssertOnlyEntryShownAs(mount, L"", L"dst");
            AssertOnlyEntryShownAs(mount, L"dst", L"a.txt");
        }
    }

    TEST_METHOD(ReplaceRenameOpenDirectory_OntoDirectoryWithAChild_FailsAndKeepsTheHandleOpen) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"src\\a.txt", "a");
        env.WriteFile(env.Upper(), L"dst\\extra.txt", "extra");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        AssertStatus(STATUS_SUCCESS, mount.Open(L"src", FILE_LIST_DIRECTORY | DELETE,
                                                kNoCreateOptions, kNoCallerPid, &ctx, &info),
            L"The source directory must open");

        AssertReplaceRenameThroughHandleRefused(mount, ctx.get(), L"dst", STATUS_DIRECTORY_NOT_EMPTY,
            L"A replace rename of an open directory onto a directory with a child must fail");
    }

    TEST_METHOD(RenameDirectory_IntoItsOwnTree_FailsAndChangesNothing) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            for (const bool withUpperChild : {false, true}) {
                for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
                    for (const std::wstring destination : {L"a\\b", L"A\\new\\b"}) {
                        TempLayerEnvironment env(1);
                        env.WriteFile(LayerRoot(env, sourceLayer), L"a\\x.txt", "x");
                        if (withUpperChild) {
                            env.WriteFile(env.Upper(), L"a\\up.txt", "up");
                        }
                        ::LayerMount::LayerMount mount(env.MakeConfig());
                        const LayerSnapshot upperBefore(env.Upper());
                        const LayerSnapshot lowerBefore(env.Lower(0));

                        AssertStatus(STATUS_INVALID_PARAMETER,
                            mount.Rename(L"a", destination, replace, kNoCallerPid),
                            (L"The rename of a to " + destination + L" must fail").c_str());
                        upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
                        lowerBefore.AssertUnchanged(L"The refused rename must write nothing in the lower");
                        AssertOnlyEntryShownAs(mount, L"", L"a");
                        AssertEntryShownAs(mount, L"a", L"x.txt", L"x.txt");
                        if (withUpperChild) {
                            AssertEntryShownAs(mount, L"a", L"up.txt", L"up.txt");
                        }
                    }
                }
            }
        }
    }

    TEST_METHOD(ReplaceRenameDirectory_OntoADirectoryInItsOwnTree_FailsAndKeepsIt) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            for (const bool destinationHasChild : {false, true}) {
                TempLayerEnvironment env(1);
                env.WriteFile(LayerRoot(env, sourceLayer), L"a\\x.txt", "x");
                env.CreateDir(LayerRoot(env, sourceLayer), L"a\\b");
                if (destinationHasChild) {
                    env.WriteFile(LayerRoot(env, sourceLayer), L"a\\b\\c.txt", "c");
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());
                const LayerSnapshot upperBefore(env.Upper());

                AssertStatus(STATUS_INVALID_PARAMETER,
                    mount.Rename(L"a", L"a\\b", kReplaceIfExists, kNoCallerPid),
                    L"A replace rename of a onto a\\b must fail");
                upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
                AssertOnlyEntryShownAs(mount, L"", L"a");
                AssertEntryShownAs(mount, L"a", L"b", L"b");
                AssertEntryShownAs(mount, L"a", L"x.txt", L"x.txt");
            }
        }
    }

    TEST_METHOD(RenameDirectory_OntoAnEntryInItsOwnTreeWithoutReplace_ReportsACollision) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            TempLayerEnvironment env(1);
            env.CreateDir(LayerRoot(env, sourceLayer), L"a\\b");
            ::LayerMount::LayerMount mount(env.MakeConfig());
            const LayerSnapshot upperBefore(env.Upper());

            AssertStatus(STATUS_OBJECT_NAME_COLLISION,
                mount.Rename(L"a", L"a\\b", kFailIfExists, kNoCallerPid),
                L"A rename of a onto an existing a\\b without replace must report a collision");
            upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
        }
    }

    TEST_METHOD(RenameOpenDirectory_IntoItsOwnTree_FailsAndKeepsTheHandleOpen) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"a\\x.txt", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        AssertStatus(STATUS_SUCCESS, mount.Open(L"a", FILE_LIST_DIRECTORY | DELETE,
                                                kNoCreateOptions, kNoCallerPid, &ctx, &info),
            L"The source directory must open");
        const LayerSnapshot upperBefore(env.Upper());

        AssertReplaceRenameThroughHandleRefused(mount, ctx.get(), L"a\\b", STATUS_INVALID_PARAMETER,
            L"A rename of an open directory into its own tree must fail");
        upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
    }

    TEST_METHOD(RenameDirectory_ToANameThatStartsWithItsName_ListsTheNewName) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            AssertDirectoryRenameShowsNewName(sourceLayer, L"a", L"ab", kFailIfExists);
        }
    }

    TEST_METHOD(RenameFile_ToAPathUnderItself_FailsWithPathNotFound) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
                TempLayerEnvironment env(1);
                env.WriteFile(LayerRoot(env, sourceLayer), L"a", "x");
                ::LayerMount::LayerMount mount(env.MakeConfig());
                const LayerSnapshot upperBefore(env.Upper());

                AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
                    mount.Rename(L"a", L"a\\b", replace, kNoCallerPid),
                    L"A rename of a file to a path under itself must fail");
                upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
                AssertRootShowsOnlyFileNamed(mount, L"a");
            }
        }
    }

    TEST_METHOD(RenameLink_ToAPathUnderItselfWhenTheLinkLeadsToADirectory_FailsWithNotSameDevice) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createInnerLink : kDirectoryLinkCreators) {
                for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
                    TempLayerEnvironment env(1);
                    env.CreateDir(env.Root(), L"target");
                    env.WriteFile(env.Root(), L"innertarget\\c.txt", "c");
                    if (!LinkCreatedOrSkipped(CreateDirectoryJunction,
                                              LinkLayerPath(env, linkSource) + L"\\link",
                                              env.Root() + L"\\target") ||
                        !LinkCreatedOrSkipped(createInnerLink, env.Root() + L"\\target\\inner",
                                              env.Root() + L"\\innertarget")) {
                        continue;
                    }
                    ::LayerMount::LayerMount mount(env.MakeConfig());
                    const LayerSnapshot upperBefore(env.Upper());
                    const LayerSnapshot lowerBefore(env.Lower(0));
                    const LayerSnapshot targetBefore(env.Root() + L"\\target");
                    const LayerSnapshot innerTargetBefore(env.Root() + L"\\innertarget");

                    AssertStatus(STATUS_NOT_SAME_DEVICE,
                        mount.Rename(L"link\\inner", L"link\\inner\\b", replace, kNoCallerPid),
                        L"A rename of a link to a directory outside the mount to a path under itself must fail");
                    upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
                    lowerBefore.AssertUnchanged(L"The failed rename must write nothing in the lower");
                    targetBefore.AssertUnchanged(L"The failed rename must change nothing in the link target");
                    innerTargetBefore.AssertUnchanged(
                        L"The failed rename must change nothing in the target of the renamed link");
                }
            }
        }
    }

    TEST_METHOD(RenameLink_ToAnExistingPathUnderItselfWithoutReplace_FailsWithNotSameDevice) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createInnerLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                env.CreateDir(env.Root(), L"target");
                env.WriteFile(env.Root(), L"innertarget\\b", "b");
                if (!LinkCreatedOrSkipped(CreateDirectoryJunction,
                                          LinkLayerPath(env, linkSource) + L"\\link",
                                          env.Root() + L"\\target") ||
                    !LinkCreatedOrSkipped(createInnerLink, env.Root() + L"\\target\\inner",
                                          env.Root() + L"\\innertarget")) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());
                const LayerSnapshot upperBefore(env.Upper());
                const LayerSnapshot lowerBefore(env.Lower(0));
                const LayerSnapshot targetBefore(env.Root() + L"\\target");
                const LayerSnapshot innerTargetBefore(env.Root() + L"\\innertarget");

                AssertStatus(STATUS_NOT_SAME_DEVICE,
                    mount.Rename(L"link\\inner", L"link\\inner\\b", kFailIfExists, kNoCallerPid),
                    L"A rename of a link to an existing path under itself must fail as a move out of the mount");
                upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
                lowerBefore.AssertUnchanged(L"The failed rename must write nothing in the lower");
                targetBefore.AssertUnchanged(L"The failed rename must change nothing in the link target");
                innerTargetBefore.AssertUnchanged(
                    L"The failed rename must change nothing in the target of the renamed link");
            }
        }
    }

    TEST_METHOD(RenameLink_ToAPathUnderItselfWhenTheLinkLeadsToNoDirectory_FailsWithPathNotFound) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createInnerLink : kDirectoryLinkCreators) {
                for (const wchar_t* innerTarget : {L"missing", L"f.txt"}) {
                    for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
                        TempLayerEnvironment env(1);
                        env.CreateDir(env.Root(), L"target");
                        env.WriteFile(env.Root(), L"f.txt", "f");
                        if (!LinkCreatedOrSkipped(CreateDirectoryJunction,
                                                  LinkLayerPath(env, linkSource) + L"\\link",
                                                  env.Root() + L"\\target") ||
                            !LinkCreatedOrSkipped(createInnerLink,
                                                  env.Root() + L"\\target\\inner",
                                                  env.Root() + L"\\" + innerTarget)) {
                            continue;
                        }
                        ::LayerMount::LayerMount mount(env.MakeConfig());
                        const LayerSnapshot upperBefore(env.Upper());
                        const LayerSnapshot lowerBefore(env.Lower(0));
                        const LayerSnapshot targetBefore(env.Root() + L"\\target");
                        const std::wstring message =
                            std::wstring(L"A rename of a link to ") + innerTarget +
                            L" to a path under itself must fail";

                        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
                            mount.Rename(L"link\\inner", L"link\\inner\\b", replace, kNoCallerPid),
                            message.c_str());
                        upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
                        lowerBefore.AssertUnchanged(L"The failed rename must write nothing in the lower");
                        targetBefore.AssertUnchanged(L"The failed rename must change nothing in the link target");
                        Assert::AreEqual(std::string("f"), env.ReadFile(env.Root(), L"f.txt"),
                            L"The failed rename must keep the file that the link leads to");
                    }
                }
            }
        }
    }

    TEST_METHOD(Rename_ToAPathUnderAFile_FailsWithPathNotFound) {
        struct RenameSource {
            const wchar_t* upperEntry;
            const wchar_t* lowerEntry;
            ListedAs kind;
        };
        const RenameSource kSources[] = {
            {nullptr, L"x", ListedAs::File},
            {L"x", nullptr, ListedAs::File},
            {nullptr, L"x\\c.txt", ListedAs::Directory},
            {L"x\\c.txt", nullptr, ListedAs::Directory},
            {L"x\\c.txt", L"x\\d.txt", ListedAs::Directory},
        };
        for (const RenameSource& source : kSources) {
            for (const DirectoryLayer fileLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
                for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
                    TempLayerEnvironment env(1);
                    env.WriteFile(LayerRoot(env, fileLayer), L"a", "a");
                    if (source.upperEntry != nullptr) {
                        env.WriteFile(env.Upper(), source.upperEntry, "x");
                    }
                    if (source.lowerEntry != nullptr) {
                        env.WriteFile(env.Lower(0), source.lowerEntry, "x");
                    }
                    ::LayerMount::LayerMount mount(env.MakeConfig());
                    const LayerSnapshot upperBefore(env.Upper());
                    const LayerSnapshot lowerBefore(env.Lower(0));

                    AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
                        mount.Rename(L"x", L"a\\b", replace, kNoCallerPid),
                        L"A rename of x to a path under the file a must fail");
                    upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
                    lowerBefore.AssertUnchanged(L"The failed rename must write nothing in the lower");
                    AssertRootShowsTwoEntries(mount, ListedEntry{L"a", ListedAs::File},
                                              ListedEntry{L"x", source.kind});
                }
            }
        }
    }

    TEST_METHOD(Rename_ToAPathUnderADirectoryLinkThatLeadsToNoDirectory_FailsWithPathNotFound) {
        for (const LinkToNoDirectory& parentLink : kLinksToNoDirectory) {
            for (const LayerSource linkSource : kLinkLayerSources) {
                for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
                    TempLayerEnvironment env(1);
                    env.WriteFile(env.Root(), L"f.txt", "f");
                    env.WriteFile(env.Root(), L"target\\x", "x");
                    if (!LinkCreatedOrSkipped(CreateDirectoryJunction,
                                              LinkLayerPath(env, linkSource) + L"\\link",
                                              env.Root() + L"\\target") ||
                        !LinkCreatedOrSkipped(parentLink.createLink, env.Root() + L"\\target\\l",
                                              env.Root() + L"\\" + parentLink.target)) {
                        continue;
                    }
                    ::LayerMount::LayerMount mount(env.MakeConfig());
                    const LayerSnapshot upperBefore(env.Upper());
                    const LayerSnapshot lowerBefore(env.Lower(0));
                    const LayerSnapshot targetBefore(env.Root() + L"\\target");
                    const std::wstring message =
                        std::wstring(L"A rename of x to a path under a link to ") +
                        parentLink.target + L" must fail";

                    AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
                        mount.Rename(L"link\\x", L"link\\l\\b", replace, kNoCallerPid),
                        message.c_str());
                    upperBefore.AssertUnchanged(L"The failed rename must write nothing in the upper");
                    lowerBefore.AssertUnchanged(L"The failed rename must write nothing in the lower");
                    targetBefore.AssertUnchanged(
                        L"The failed rename must change nothing in the link target");
                    Assert::AreEqual(std::string("x"), env.ReadFile(env.Root(), L"target\\x"),
                        L"The failed rename must keep x in the link target");
                }
            }
        }
    }

    TEST_METHOD(Rename_ToAPathUnderARootDirectoryLinkThatLeadsToNoDirectory_FailsWithPathNotFound) {
        for (const LinkToNoDirectory& link : kLinksToNoDirectory) {
            for (const LayerSource linkSource : kLinkLayerSources) {
                AssertRenameUnderRootLinkFailsWithPathNotFound(linkSource, link);
            }
        }
    }

    TEST_METHOD(ReplaceRename_FileOntoDirectory_FailsAndChangesNothing) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            for (const DirectoryLayer destinationLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
                TempLayerEnvironment env(1);
                env.WriteFile(LayerRoot(env, sourceLayer), L"f.txt", "f");
                env.CreateDir(LayerRoot(env, destinationLayer), L"dir");
                ::LayerMount::LayerMount mount(env.MakeConfig());
                const LayerSnapshot upperBefore(env.Upper());
                const LayerSnapshot lowerBefore(env.Lower(0));

                AssertStatus(STATUS_FILE_IS_A_DIRECTORY,
                    mount.Rename(L"f.txt", L"dir", kReplaceIfExists, kNoCallerPid),
                    L"A replace rename of a file onto a directory must fail");
                upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
                lowerBefore.AssertUnchanged(L"The refused rename must write nothing in the lower");
                AssertRootShowsFileAndDirectory(mount, L"f.txt", L"dir");
            }
        }
    }

    TEST_METHOD(ReplaceRename_DirectoryOntoFile_FailsAndChangesNothing) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            for (const DirectoryLayer destinationLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
                TempLayerEnvironment env(1);
                env.WriteFile(LayerRoot(env, sourceLayer), L"dir\\a.txt", "a");
                env.WriteFile(LayerRoot(env, destinationLayer), L"f.txt", "f");
                ::LayerMount::LayerMount mount(env.MakeConfig());
                const LayerSnapshot upperBefore(env.Upper());
                const LayerSnapshot lowerBefore(env.Lower(0));

                AssertStatus(STATUS_NOT_A_DIRECTORY,
                    mount.Rename(L"dir", L"f.txt", kReplaceIfExists, kNoCallerPid),
                    L"A replace rename of a directory onto a file must fail");
                upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
                lowerBefore.AssertUnchanged(L"The refused rename must write nothing in the lower");
                AssertRootShowsFileAndDirectory(mount, L"f.txt", L"dir");
                AssertOnlyEntryShownAs(mount, L"dir", L"a.txt");
            }
        }
    }

    TEST_METHOD(ReplaceRename_FileOntoItsParentDirectory_FailsWithDirectoryNotEmpty) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            TempLayerEnvironment env(1);
            env.WriteFile(LayerRoot(env, sourceLayer), L"a\\f.txt", "f");
            ::LayerMount::LayerMount mount(env.MakeConfig());
            const LayerSnapshot upperBefore(env.Upper());
            const LayerSnapshot lowerBefore(env.Lower(0));

            AssertStatus(STATUS_DIRECTORY_NOT_EMPTY,
                mount.Rename(L"a\\f.txt", L"a", kReplaceIfExists, kNoCallerPid),
                L"A replace rename of a\\f.txt onto a must fail");
            upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
            lowerBefore.AssertUnchanged(L"The refused rename must write nothing in the lower");
            AssertOnlyEntryShownAs(mount, L"", L"a");
            AssertOnlyEntryShownAs(mount, L"a", L"f.txt");
        }
    }

    TEST_METHOD(Rename_OntoAnExistingEntryOfAnyTypeWithoutReplace_ReportsACollision) {
        for (const DirectoryLayer layer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            TempLayerEnvironment env(1);
            env.WriteFile(LayerRoot(env, layer), L"f.txt", "f");
            env.WriteFile(LayerRoot(env, layer), L"dir\\a.txt", "a");
            ::LayerMount::LayerMount mount(env.MakeConfig());
            const LayerSnapshot upperBefore(env.Upper());
            const LayerSnapshot lowerBefore(env.Lower(0));

            const std::pair<std::wstring, std::wstring> renames[] = {
                {L"f.txt", L"dir"},
                {L"dir", L"f.txt"},
                {L"dir\\a.txt", L"dir"},
            };
            for (const auto& [from, to] : renames) {
                AssertStatus(STATUS_OBJECT_NAME_COLLISION,
                    mount.Rename(from, to, kFailIfExists, kNoCallerPid),
                    (L"A rename of " + from + L" onto " + to + L" without replace must report a collision").c_str());
            }
            upperBefore.AssertUnchanged(L"The refused renames must write nothing in the upper");
            lowerBefore.AssertUnchanged(L"The refused renames must write nothing in the lower");
        }
    }

    TEST_METHOD(ReplaceRenameOpenFile_OntoDirectory_FailsAndKeepsTheHandleOpen) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            TempLayerEnvironment env(1);
            env.WriteFile(LayerRoot(env, sourceLayer), L"f.txt", "f");
            env.CreateDir(env.Upper(), L"dir");
            ::LayerMount::LayerMount mount(env.MakeConfig());
            std::unique_ptr<FileContext> ctx;
            InternalFileInfo info{};
            AssertStatus(STATUS_SUCCESS, mount.Open(L"f.txt", FILE_READ_DATA | DELETE,
                                                    kNoCreateOptions, kNoCallerPid, &ctx, &info),
                L"The source file must open");
            const LayerSnapshot upperBefore(env.Upper());
            const LayerSnapshot lowerBefore(env.Lower(0));

            AssertReplaceRenameThroughHandleRefused(mount, ctx.get(), L"dir", STATUS_FILE_IS_A_DIRECTORY,
                L"A replace rename of an open file onto a directory must fail");
            upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
            lowerBefore.AssertUnchanged(L"The refused rename must write nothing in the lower");
        }
    }

    TEST_METHOD(ReplaceRenameOpenDirectory_OntoFile_FailsAndKeepsTheHandleOpen) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            TempLayerEnvironment env(1);
            env.WriteFile(LayerRoot(env, sourceLayer), L"dir\\a.txt", "a");
            env.WriteFile(env.Lower(0), L"f.txt", "f");
            ::LayerMount::LayerMount mount(env.MakeConfig());
            std::unique_ptr<FileContext> ctx;
            InternalFileInfo info{};
            AssertStatus(STATUS_SUCCESS, mount.Open(L"dir", FILE_LIST_DIRECTORY | DELETE,
                                                    kNoCreateOptions, kNoCallerPid, &ctx, &info),
                L"The source directory must open");
            const LayerSnapshot upperBefore(env.Upper());
            const LayerSnapshot lowerBefore(env.Lower(0));

            AssertReplaceRenameThroughHandleRefused(mount, ctx.get(), L"f.txt", STATUS_NOT_A_DIRECTORY,
                L"A replace rename of an open directory onto a file must fail");
            upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
            lowerBefore.AssertUnchanged(L"The refused rename must write nothing in the lower");
        }
    }

    TEST_METHOD(ReplaceRename_FileOntoUpperJunction_ReplacesTheJunctionAndKeepsItsTarget) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        env.WriteFile(env.Lower(0), L"f.txt", "f");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Upper() + L"\\link",
                                  env.Root() + L"\\target")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot targetBefore(env.Root() + L"\\target");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"f.txt", L"link", kReplaceIfExists, kNoCallerPid),
            L"A replace rename of a file onto a directory junction must succeed");
        AssertRootShowsOnlyFileNamed(mount, L"link");
        Assert::AreEqual(static_cast<DWORD>(0), ReparseTagOf(env.Upper() + L"\\link"),
            L"The upper link must be the moved file, not the junction");
        targetBefore.AssertUnchanged(L"The rename must leave the junction target's entries");
        Assert::AreEqual(std::string("inside"), env.ReadFile(env.Root(), L"target\\inside.txt"),
            L"The rename must leave the junction target's file");
        Assert::AreEqual(std::string("f"), ReadThroughMount(mount, L"link"),
            L"The new name must show the moved file's data");
    }

    TEST_METHOD(Rename_UpperLinkToOpaqueMarkedTarget_KeepsTheTargetMarker) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Root(), OpaqueMarkerPath(L"target"), "");
            if (!LinkCreatedOrSkipped(createLink, env.Upper() + L"\\link", env.Root() + L"\\target")) {
                continue;
            }
            ::LayerMount::LayerMount mount(env.MakeConfig());

            AssertStatus(STATUS_SUCCESS,
                mount.Rename(L"link", L"moved", kFailIfExists, kNoCallerPid),
                L"A rename of an upper link must succeed");
            Assert::IsTrue(HasAttribute(env.Upper() + L"\\moved", FILE_ATTRIBUTE_REPARSE_POINT),
                L"The upper entry at moved must be the moved link");
            Assert::IsTrue(env.FileExists(env.Root(), OpaqueMarkerPath(L"target")),
                L"The rename of the link must keep the marker file in its target");
        }
    }

    TEST_METHOD(ReplaceRename_FileOntoUpperLinkToOpaqueMarkedTarget_KeepsTheTargetMarker) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Root(), OpaqueMarkerPath(L"target"), "");
            env.WriteFile(env.Lower(0), L"f.txt", "f");
            if (!LinkCreatedOrSkipped(createLink, env.Upper() + L"\\link", env.Root() + L"\\target")) {
                continue;
            }
            ::LayerMount::LayerMount mount(env.MakeConfig());

            AssertStatus(STATUS_SUCCESS,
                mount.Rename(L"f.txt", L"link", kReplaceIfExists, kNoCallerPid),
                L"A replace rename of a file onto an upper link must succeed");
            Assert::AreEqual(static_cast<DWORD>(0), ReparseTagOf(env.Upper() + L"\\link"),
                L"The upper link must be the moved file");
            Assert::IsTrue(env.FileExists(env.Root(), OpaqueMarkerPath(L"target")),
                L"The replace rename must keep the marker file in the link target");
        }
    }

    TEST_METHOD(Rename_FileOntoNameNextToWhiteoutNamedFileInLinkTarget_KeepsTheTargetFile) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
                    TempLayerEnvironment env(1);
                    env.WriteFile(env.Root(), L"target\\f.txt", "f");
                    if (!LinkToWhiteoutNamedFileCreatedOrSkipped(env, linkSource, createLink)) {
                        continue;
                    }
                    ::LayerMount::LayerMount mount(env.MakeConfig());

                    AssertStatus(STATUS_SUCCESS,
                        mount.Rename(L"link\\f.txt", L"link\\foo", replace, kNoCallerPid),
                        L"A rename of a file in a link target to link\\foo must succeed");
                    Assert::AreEqual(std::string("f"), env.ReadFile(env.Root(), L"target\\foo"),
                        L"The rename must move the file to foo in the link target");
                    Assert::AreEqual(std::string("target"),
                        env.ReadFile(env.Root(), WhiteoutMarkerPath(L"target\\foo")),
                        L"The rename must keep the .wh.foo file in the link target");
                    AssertUpperLinkListsWholeTarget(env, mount, L"link", env.Root() + L"\\target");
                }
            }
        }
    }

    TEST_METHOD(Rename_FileWithinLowerLink_RenamesTheFileInTheTarget) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
                TempLayerEnvironment env(1);
                env.WriteFile(env.Root(), L"target\\a", "a");
                env.WriteFile(env.Root(), L"target\\kept.txt", "kept");
                if (replace) {
                    env.WriteFile(env.Root(), L"target\\b", "replaced");
                }
                if (!LinkCreatedOrSkipped(createLink, env.Lower(0) + L"\\link",
                                          env.Root() + L"\\target")) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());

                AssertStatus(STATUS_SUCCESS, mount.Rename(L"link\\a", L"link\\b", replace, kNoCallerPid),
                    L"A rename of a file within a lower link must succeed");

                Assert::IsFalse(env.FileExists(env.Root(), L"target\\a"),
                    L"The rename must remove a from the link target");
                Assert::AreEqual(std::string("a"), env.ReadFile(env.Root(), L"target\\b"),
                    L"The rename must leave the file at b in the link target");
                Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"link\\a")),
                    L"The rename must write no whiteout through the link");
                AssertUpperLinkListsWholeTarget(env, mount, L"link", env.Root() + L"\\target");
            }
        }
    }

    TEST_METHOD(Rename_DirectoryWithinLowerJunction_RenamesTheDirectoryInTheTarget) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\sub\\inside.txt", "inside");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Lower(0) + L"\\link",
                                  env.Root() + L"\\target")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"link\\sub", L"link\\moved", kFailIfExists, kNoCallerPid),
            L"A rename of a directory within a lower junction must succeed");

        Assert::IsFalse(env.FileExists(env.Root(), L"target\\sub"),
            L"The rename must remove sub from the junction target");
        Assert::AreEqual(std::string("inside"), env.ReadFile(env.Root(), L"target\\moved\\inside.txt"),
            L"The rename must move the directory with its file in the junction target");
        AssertUpperLinkListsWholeTarget(env, mount, L"link", env.Root() + L"\\target");
    }

    TEST_METHOD(Rename_DirectoryInLinkTargetOntoNameALowerHoldsUnderTheLink_WritesNoOpaqueMarkerIntoTheTarget) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(2);
                env.WriteFile(env.Root(), L"target\\src\\a.txt", "a");
                env.WriteFile(env.Lower(1), L"link\\dst\\old.txt", "old");
                if (!LinkToPrimedTargetCreatedOrSkipped(env, linkSource, createLink)) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());
                RenamePrimerToCopyLowerLinkUp(mount, linkSource);

                AssertStatus(STATUS_SUCCESS,
                    mount.Rename(L"link\\src", L"link\\dst", kFailIfExists, kNoCallerPid),
                    L"A rename of a directory in a link target onto a name a lower holds under the link must succeed");
                AssertNoOpaqueMarkerIn(env.Root() + L"\\target\\dst");
                AssertTargetHolds(env, {L"dst", L"dst\\a.txt"},
                    L"The link target must hold only the moved directory and its file");
                AssertUpperLinkListsWholeTarget(env, mount, L"link", env.Root() + L"\\target");
            }
        }
    }

    TEST_METHOD(Rename_AcrossLinkBoundary_FailsWithNotSameDeviceAndChangesNothing) {
        struct CrossingRename {
            const wchar_t* from;
            const wchar_t* to;
        };
        constexpr CrossingRename kCrossings[] = {
            {L"f.txt", L"link\\foo"},
            {L"d", L"link\\d"},
            {L"link\\foo", L"bar"},
            {L"link\\sub", L"moved"},
            {L"link\\foo", L"other\\foo"},
        };
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                for (const CrossingRename& crossing : kCrossings) {
                    TempLayerEnvironment env(1);
                    env.WriteFile(env.Lower(0), L"f.txt", "f");
                    env.WriteFile(env.Lower(0), L"d\\inside.txt", "d");
                    env.WriteFile(env.Root(), L"target\\foo", "foo");
                    env.WriteFile(env.Root(), L"target\\sub\\inside.txt", "sub");
                    env.CreateDir(env.Root(), L"othertarget");
                    const std::wstring& linkLayer = LinkLayerPath(env, linkSource);
                    if (!LinkCreatedOrSkipped(createLink, linkLayer + L"\\link", env.Root() + L"\\target") ||
                        !LinkCreatedOrSkipped(createLink, linkLayer + L"\\other",
                                              env.Root() + L"\\othertarget")) {
                        continue;
                    }
                    ::LayerMount::LayerMount mount(env.MakeConfig());
                    const LayerSnapshot targetBefore(env.Root() + L"\\target");
                    const LayerSnapshot otherTargetBefore(env.Root() + L"\\othertarget");
                    const LayerSnapshot upperBefore(env.Upper());
                    const LayerSnapshot lowerBefore(env.Lower(0));
                    const std::wstring message =
                        std::wstring(L"A rename of ") + crossing.from + L" to " + crossing.to +
                        L" crosses a link boundary and must fail";

                    AssertStatus(STATUS_NOT_SAME_DEVICE,
                        mount.Rename(crossing.from, crossing.to, kReplaceIfExists, kNoCallerPid),
                        message.c_str());

                    targetBefore.AssertUnchanged(L"The refused rename must change nothing in the link target");
                    otherTargetBefore.AssertUnchanged(L"The refused rename must change nothing in the other link target");
                    upperBefore.AssertUnchanged(L"The refused rename must change nothing in the upper");
                    lowerBefore.AssertUnchanged(L"The refused rename must change nothing in the lower");
                }
            }
        }
    }

    TEST_METHOD(RenameContext_FileOutOfLowerJunction_FailsWithNotSameDeviceAndKeepsTheFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\foo", "foo");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Lower(0) + L"\\link",
                                  env.Root() + L"\\target")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        Assert::IsTrue(NT_SUCCESS(mount.Open(L"link\\foo", FILE_READ_ATTRIBUTES | DELETE,
                                             kNoCreateOptions, kNoCallerPid, &ctx, &info)),
            L"Preconditions: the file under the lower junction must open");

        const NTSTATUS status = mount.Rename(ctx.get(), L"bar", kFailIfExists, kNoCallerPid);
        mount.Close(ctx.get());

        AssertStatus(STATUS_NOT_SAME_DEVICE, status,
            L"A rename of an open file out of a lower junction must fail");
        Assert::AreEqual(std::string("foo"), env.ReadFile(env.Root(), L"target\\foo"),
            L"The refused rename must keep the file in the junction target");
        upperBefore.AssertUnchanged(L"The refused rename must change nothing in the upper");
    }

    TEST_METHOD(RenameContext_FileWithinLowerLink_RenamesTheFileInTheTarget) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Root(), L"target\\a", "a");
            if (!LinkCreatedOrSkipped(createLink, env.Lower(0) + L"\\link", env.Root() + L"\\target")) {
                continue;
            }
            ::LayerMount::LayerMount mount(env.MakeConfig());
            std::unique_ptr<FileContext> ctx;
            InternalFileInfo info{};
            Assert::IsTrue(NT_SUCCESS(mount.Open(L"link\\a", FILE_READ_ATTRIBUTES | DELETE,
                                                 kNoCreateOptions, kNoCallerPid, &ctx, &info)),
                L"Preconditions: the file under the lower link must open");

            const NTSTATUS status = mount.Rename(ctx.get(), L"link\\b", kFailIfExists, kNoCallerPid);
            const bool metacopyAfterRename = ctx->isMetacopyOnly;
            mount.Close(ctx.get());

            AssertStatus(STATUS_SUCCESS, status, L"A rename of an open file within a lower link must succeed");
            Assert::IsFalse(metacopyAfterRename,
                L"The renamed context must not count as a metacopy shell, as the target's file holds its data");
            Assert::IsFalse(env.FileExists(env.Root(), L"target\\a"),
                L"The rename must remove a from the link target");
            Assert::AreEqual(std::string("a"), env.ReadFile(env.Root(), L"target\\b"),
                L"The rename must leave the file at b in the link target");
            Assert::AreEqual(std::string("a"), ReadThroughMount(mount, L"link\\b"),
                L"The mount must show the renamed file");
            AssertUpperLinkListsWholeTarget(env, mount, L"link", env.Root() + L"\\target");
        }
    }

    TEST_METHOD(ReplaceRename_FileOntoFileInLinkTargetWhenTheWorkDirectoryRefusesEntries_ReplacesTheFile) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                env.WriteFile(env.Root(), L"target\\a", "a");
                env.WriteFile(env.Root(), L"target\\b", "replaced");
                if (!LinkToPrimedTargetCreatedOrSkipped(env, linkSource, createLink)) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());
                RenamePrimerToCopyLowerLinkUp(mount, linkSource);
                const LayerSnapshot workBefore(env.Staging());

                const NTSTATUS status =
                    RenameWhileWorkRefusesEntries(env, mount, L"link\\a", L"link\\b");

                AssertStatus(STATUS_SUCCESS, status,
                    L"A replace-rename of a file onto a file in a link target must succeed");
                AssertTargetHolds(env, {L"b"},
                    L"The link target must hold only b, and no copy of the replaced file");
                Assert::AreEqual(std::string("a"), env.ReadFile(env.Root(), L"target\\b"),
                    L"b in the link target must hold the moved file's data");
                workBefore.AssertUnchanged(L"The rename must put nothing in the work directory");
                Assert::AreEqual(std::string("a"), ReadThroughMount(mount, L"link\\b"),
                    L"The mount must show the moved file at link\\b");
            }
        }
    }

    TEST_METHOD(ReplaceRename_FileOntoFileSymlinkInLinkTargetWhenTheWorkDirectoryRefusesEntries_ReplacesTheSymlink) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\a", "a");
        env.WriteFile(env.Root(), L"other.txt", "other");
        if (!LinkCreatedOrSkipped(CreateFileSymlink, env.Root() + L"\\target\\b",
                                  env.Root() + L"\\other.txt") ||
            !LinkToPrimedTargetCreatedOrSkipped(env, LayerSource::Upper, CreateDirectoryJunction)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot workBefore(env.Staging());

        const NTSTATUS status = RenameWhileWorkRefusesEntries(env, mount, L"link\\a", L"link\\b");

        AssertStatus(STATUS_SUCCESS, status,
            L"A replace-rename of a file onto a file symlink in a link target must succeed");
        AssertTargetHolds(env, {L"b"}, L"The link target must hold only b");
        Assert::AreEqual(static_cast<DWORD>(0), ReparseTagOf(env.Root() + L"\\target\\b"),
            L"b in the link target must be the moved file, not the symlink");
        Assert::AreEqual(std::string("a"), env.ReadFile(env.Root(), L"target\\b"),
            L"b in the link target must hold the moved file's data");
        Assert::AreEqual(std::string("other"), env.ReadFile(env.Root(), L"other.txt"),
            L"The rename must leave the symlink's target");
        workBefore.AssertUnchanged(L"The rename must put nothing in the work directory");
    }

    TEST_METHOD(ReplaceRename_DirectoryOntoEmptyDirectoryInLinkTargetWhenTheWorkDirectoryRefusesEntries_ReplacesTheDirectory) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                env.WriteFile(env.Root(), L"target\\src\\inside.txt", "inside");
                env.CreateDir(env.Root(), L"target\\dst");
                if (!LinkToPrimedTargetCreatedOrSkipped(env, linkSource, createLink)) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());
                RenamePrimerToCopyLowerLinkUp(mount, linkSource);
                const LayerSnapshot workBefore(env.Staging());

                const NTSTATUS status =
                    RenameWhileWorkRefusesEntries(env, mount, L"link\\src", L"link\\dst");

                AssertStatus(STATUS_SUCCESS, status,
                    L"A replace-rename of a directory onto an empty directory in a link target must succeed");
                AssertTargetHolds(env, {L"dst", L"dst\\inside.txt"},
                    L"The link target must hold only the moved directory, and no copy of the replaced one");
                workBefore.AssertUnchanged(L"The rename must put nothing in the work directory");
                AssertUpperLinkListsWholeTarget(env, mount, L"link", env.Root() + L"\\target");
            }
        }
    }

    TEST_METHOD(ReplaceRename_FileOntoJunctionInLinkTargetWhenTheWorkDirectoryRefusesEntries_ReplacesTheJunction) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Root(), L"target\\a", "a");
            env.WriteFile(env.Root(), L"inner\\inside.txt", "inside");
            if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Root() + L"\\target\\j",
                                      env.Root() + L"\\inner") ||
                !LinkToPrimedTargetCreatedOrSkipped(env, linkSource, CreateDirectoryJunction)) {
                continue;
            }
            ::LayerMount::LayerMount mount(env.MakeConfig());
            RenamePrimerToCopyLowerLinkUp(mount, linkSource);
            const LayerSnapshot workBefore(env.Staging());
            const LayerSnapshot innerBefore(env.Root() + L"\\inner");

            const NTSTATUS status =
                RenameWhileWorkRefusesEntries(env, mount, L"link\\a", L"link\\j");

            AssertStatus(STATUS_SUCCESS, status,
                L"A replace-rename of a file onto a junction in a link target must succeed");
            AssertTargetHolds(env, {L"j"},
                L"The link target must hold only j, and no copy of the replaced junction");
            Assert::AreEqual(static_cast<DWORD>(0), ReparseTagOf(env.Root() + L"\\target\\j"),
                L"j in the link target must be the moved file, not the junction");
            Assert::AreEqual(std::string("a"), env.ReadFile(env.Root(), L"target\\j"),
                L"j in the link target must hold the moved file's data");
            innerBefore.AssertUnchanged(L"The rename must leave the replaced junction's target");
            workBefore.AssertUnchanged(L"The rename must put nothing in the work directory");
        }
    }

    TEST_METHOD(ReplaceRename_DirectoryOntoDirectoryInLinkTargetWhenTheMoveFails_KeepsTheDestination) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Root(), L"target\\src\\inside.txt", "inside");
            env.CreateDir(env.Root(), L"target\\dst");
            if (!LinkToPrimedTargetCreatedOrSkipped(env, linkSource, CreateDirectoryJunction)) {
                continue;
            }
            ::LayerMount::LayerMount mount(env.MakeConfig());
            RenamePrimerToCopyLowerLinkUp(mount, linkSource);
            const LayerSnapshot workBefore(env.Staging());

            NTSTATUS status = STATUS_SUCCESS;
            {
                // An open child without FILE_SHARE_DELETE blocks the move of src.
                const ScopedHandle heldChild = HoldOpen(
                    env.Root() + L"\\target\\src\\inside.txt", FILE_SHARE_READ | FILE_SHARE_WRITE);
                status = RenameWhileWorkRefusesEntries(env, mount, L"link\\src", L"link\\dst");
            }

            Assert::IsFalse(NT_SUCCESS(status), L"The rename must fail while a child of src is open");
            AssertTargetHolds(env, {L"dst", L"src", L"src\\inside.txt"},
                L"The failed rename must leave the link target as it was");
            Assert::IsFalse(HasAttribute(env.Root() + L"\\target\\dst", FILE_ATTRIBUTE_HIDDEN),
                L"dst must not be hidden once it is back");
            workBefore.AssertUnchanged(L"The failed rename must put nothing in the work directory");
            AssertUpperLinkListsWholeTarget(env, mount, L"link", env.Root() + L"\\target");
        }
    }

    TEST_METHOD(ReplaceRename_FileOntoJunctionInLinkTarget_HidesTheJunctionWhileItIsAside) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\a", "a");
        env.CreateDir(env.Root(), L"inner");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Root() + L"\\target\\j",
                                  env.Root() + L"\\inner") ||
            !LinkToPrimedTargetCreatedOrSkipped(env, LayerSource::Upper, CreateDirectoryJunction)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        // The handle stays on the junction itself when it moves aside and
        // after the rename removes it.
        const ScopedHandle heldJunction(::CreateFileW(
            (env.Root() + L"\\target\\j").c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        Assert::IsTrue(heldJunction.IsValid(), L"The test must hold the junction j open");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"link\\a", L"link\\j", kReplaceIfExists, kNoCallerPid),
            L"A replace-rename of a file onto a junction in a link target must succeed");

        const DWORD attributes = AttributesOfHeld(heldJunction);
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES, attributes,
            L"The test must read the attributes of the held junction");
        Assert::IsTrue((attributes & FILE_ATTRIBUTE_HIDDEN) != 0,
            L"The replaced junction must be hidden once it moved aside");
        Assert::IsFalse(HasAttribute(env.Root() + L"\\inner", FILE_ATTRIBUTE_HIDDEN),
            L"The junction's target must not be hidden");
    }

    TEST_METHOD(ReplaceRename_LinkOntoLinkInLinkTargetWhenTheWorkDirectoryRefusesEntries_ReplacesTheLink) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                env.CreateDir(env.Root(), L"target");
                env.WriteFile(env.Root(), L"moved\\moved.txt", "moved");
                env.WriteFile(env.Root(), L"replaced\\replaced.txt", "replaced");
                if (!LinkCreatedOrSkipped(createLink, env.Root() + L"\\target\\s",
                                          env.Root() + L"\\moved") ||
                    !LinkCreatedOrSkipped(createLink, env.Root() + L"\\target\\d",
                                          env.Root() + L"\\replaced") ||
                    !LinkToPrimedTargetCreatedOrSkipped(env, linkSource, CreateDirectoryJunction)) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());
                RenamePrimerToCopyLowerLinkUp(mount, linkSource);
                const LayerSnapshot workBefore(env.Staging());
                const LayerSnapshot replacedBefore(env.Root() + L"\\replaced");

                const NTSTATUS status =
                    RenameWhileWorkRefusesEntries(env, mount, L"link\\s", L"link\\d");

                AssertStatus(STATUS_SUCCESS, status,
                    L"A replace-rename of a link onto a link in a link target must succeed");
                Assert::IsFalse(env.FileExists(env.Root(), L"target\\s"),
                    L"The rename must remove s from the link target");
                Assert::AreNotEqual(static_cast<DWORD>(0), ReparseTagOf(env.Root() + L"\\target\\d"),
                    L"d in the link target must be a link");
                Assert::AreEqual(std::string("moved"), env.ReadFile(env.Root(), L"target\\d\\moved.txt"),
                    L"d in the link target must be the moved link");
                Assert::IsTrue(FileNamesMatching(env.Root() + L"\\target", L".layermount#*").empty(),
                    L"The rename must leave no copy of the replaced link in the link target");
                replacedBefore.AssertUnchanged(L"The rename must leave the replaced link's target");
                Assert::AreEqual(std::string("replaced"),
                    env.ReadFile(env.Root(), L"replaced\\replaced.txt"),
                    L"The replaced link's target must keep its file");
                workBefore.AssertUnchanged(L"The rename must put nothing in the work directory");
            }
        }
    }

    TEST_METHOD(ReplaceRename_FileOntoReadOnlyFileInLinkTarget_FailsWithAccessDeniedAndKeepsBoth) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Root(), L"target\\a", "a");
            env.WriteFile(env.Root(), L"target\\b", "keep");
            Assert::IsTrue(::SetFileAttributesW((env.Root() + L"\\target\\b").c_str(),
                                                FILE_ATTRIBUTE_READONLY) != FALSE,
                L"The test must make b in the link target read-only");
            if (!LinkToPrimedTargetCreatedOrSkipped(env, linkSource, CreateDirectoryJunction)) {
                continue;
            }
            ::LayerMount::LayerMount mount(env.MakeConfig());
            RenamePrimerToCopyLowerLinkUp(mount, linkSource);
            const LayerSnapshot workBefore(env.Staging());

            AssertStatus(STATUS_ACCESS_DENIED,
                mount.Rename(L"link\\a", L"link\\b", kReplaceIfExists, kNoCallerPid),
                L"A replace-rename onto a read-only file in a link target must fail");
            AssertTargetHolds(env, {L"a", L"b"}, L"The refused rename must keep a and b");
            Assert::AreEqual(std::string("a"), env.ReadFile(env.Root(), L"target\\a"),
                L"a in the link target must keep its data");
            Assert::AreEqual(std::string("keep"), env.ReadFile(env.Root(), L"target\\b"),
                L"b in the link target must keep its data");
            workBefore.AssertUnchanged(L"The refused rename must put nothing in the work directory");
        }
    }

    TEST_METHOD(ReplaceRename_FileOntoFileInLinkTargetWhoseRecordCannotMove_ReplacesTheFileAndWarns) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\a", "a");
        env.WriteFile(env.Root(), L"target\\b", "replaced");
        if (!LinkToPrimedTargetCreatedOrSkipped(env, LayerSource::Upper, CreateDirectoryJunction)) {
            return;
        }
        LayerConfig config = env.MakeConfig();
        config.hostCapabilities = kHostCapabilitiesWithoutAds;
        const std::wstring sidecar = env.Upper() + L"\\" + kSidecarDirName;
        WriteStableIndexRecord(env.Upper() + L"\\link\\a", 42, config);
        const std::wstring recordOfA = StoredLeafName(sidecar + L"\\*.meta.json");
        WriteStableIndexRecord(env.Upper() + L"\\link\\b", 7, config);
        EmittedWarnings warnings;
        ::LayerMount::LayerMount mount(config);
        mount.Events().Set(&CollectWarning, &warnings);

        NTSTATUS status = STATUS_SUCCESS;
        {
            const ScopedHandle recordHeldAgainstMove =
                HoldOpen(sidecar + L"\\" + recordOfA, FILE_SHARE_READ | FILE_SHARE_WRITE);
            status = mount.Rename(L"link\\a", L"link\\b", kReplaceIfExists, kNoCallerPid);
        }

        AssertStatus(STATUS_SUCCESS, status,
            L"The rename must stand once the move replaced b, though the record of a cannot move");
        AssertTargetHolds(env, {L"b"}, L"The link target must hold only b");
        Assert::AreEqual(std::string("a"), env.ReadFile(env.Root(), L"target\\b"),
            L"b in the link target must hold the moved file's data");
        Assert::AreEqual<size_t>(1, warnings.results.size(),
            L"The record that did not move with the file must emit one warning");
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_SHARING_VIOLATION), warnings.results[0],
            L"The warning must carry the failure of the record move");
        Assert::AreEqual(std::wstring(L"link\\b"), warnings.paths[0],
            L"The warning must name the path where the file stays");
        Assert::AreNotEqual(uint64_t{7},
            MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\link\\b", &config)
                .stableIndexNumber,
            L"b must not carry the record of the file it replaced");
    }

    TEST_METHOD(ReplaceRenameOpenFile_OntoFileInLinkTargetWhenTheWorkDirectoryRefusesEntries_ReplacesTheFile) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                env.WriteFile(env.Root(), L"target\\a", "a");
                env.WriteFile(env.Root(), L"target\\b", "replaced");
                if (!LinkToPrimedTargetCreatedOrSkipped(env, linkSource, createLink)) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());
                RenamePrimerToCopyLowerLinkUp(mount, linkSource);
                const LayerSnapshot workBefore(env.Staging());
                std::unique_ptr<FileContext> ctx;
                InternalFileInfo info{};
                Assert::IsTrue(NT_SUCCESS(mount.Open(L"link\\a", FILE_READ_ATTRIBUTES | DELETE,
                                                     kNoCreateOptions, kNoCallerPid, &ctx, &info)),
                    L"Preconditions: the file in the link target must open");

                const NTSTATUS status =
                    RenameWhileWorkRefusesEntries(env, mount, ctx.get(), L"link\\b");
                mount.Close(ctx.get());

                AssertStatus(STATUS_SUCCESS, status,
                    L"A replace-rename of an open file onto a file in a link target must succeed");
                AssertTargetHolds(env, {L"b"},
                    L"The link target must hold only b, and no copy of the replaced file");
                Assert::AreEqual(std::string("a"), env.ReadFile(env.Root(), L"target\\b"),
                    L"b in the link target must hold the moved file's data");
                workBefore.AssertUnchanged(L"The rename must put nothing in the work directory");
            }
        }
    }

    TEST_METHOD(Rename_IntoMissingDirectoryUnderLowerLink_FailsWithPathNotFoundAndCopiesNoLinkUp) {
        struct MissingParentRename {
            const wchar_t* from;
            const wchar_t* to;
        };
        constexpr MissingParentRename kRenames[] = {
            {L"f.txt", L"link\\missing\\x"},
            {L"d", L"link\\missing\\d"},
            {L"link\\a", L"link\\missing\\b"},
            {L"link\\sub", L"link\\missing\\sub"},
        };
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            for (const MissingParentRename& rename : kRenames) {
                TempLayerEnvironment env(1);
                env.WriteFile(env.Lower(0), L"f.txt", "f");
                env.WriteFile(env.Lower(0), L"d\\inside.txt", "d");
                env.WriteFile(env.Root(), L"target\\a", "a");
                env.WriteFile(env.Root(), L"target\\sub\\inside.txt", "sub");
                if (!LinkCreatedOrSkipped(createLink, env.Lower(0) + L"\\link", env.Root() + L"\\target")) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());
                const LayerSnapshot targetBefore(env.Root() + L"\\target");
                const LayerSnapshot upperBefore(env.Upper());
                const std::wstring message = std::wstring(L"A rename of ") + rename.from + L" to " +
                                             rename.to + L" into a missing directory must fail";

                AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
                    mount.Rename(rename.from, rename.to, kReplaceIfExists, kNoCallerPid),
                    message.c_str());

                targetBefore.AssertUnchanged(L"The failed rename must change nothing in the link target");
                upperBefore.AssertUnchanged(L"The failed rename must copy no link up into the upper");
            }
        }
    }

    TEST_METHOD(Rename_UnderJunctionWithUnreadableReparseTag_FailsWithAccessDeniedAndChangesNothing) {
        struct UnreadableRename {
            const wchar_t* from;
            const wchar_t* to;
        };
        constexpr UnreadableRename kRenames[] = {
            {L"f.txt", L"link\\bar"},
            {L"link\\foo", L"bar"},
            {L"link\\foo", L"link\\bar"},
        };
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const UnreadableRename& rename : kRenames) {
                TempLayerEnvironment env(1);
                env.WriteFile(env.Lower(0), L"f.txt", "f");
                env.WriteFile(env.Root(), L"target\\foo", "foo");
                const std::wstring junction = LinkLayerPath(env, linkSource) + L"\\link";
                if (!LinkCreatedOrSkipped(CreateDirectoryJunction, junction, env.Root() + L"\\target")) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());
                const LayerSnapshot targetBefore(env.Root() + L"\\target");
                const LayerSnapshot upperBefore(env.Upper());
                const LayerSnapshot lowerBefore(env.Lower(0));
                NTSTATUS status = STATUS_SUCCESS;
                {
                    LinkAccessDenied synchronizeDenied(junction, SYNCHRONIZE);
                    BackupPrivilegeDisabledOnThread noBackupPrivilege;
                    DisableRestorePrivilegeOnThread();
                    Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, LinkTagOpenError(junction),
                        L"The deny ACE must make the junction's reparse tag unreadable");
                    status = mount.Rename(rename.from, rename.to, kReplaceIfExists, kNoCallerPid);
                }
                const std::wstring message = std::wstring(L"A rename of ") + rename.from + L" to " +
                                             rename.to + L" with an unreadable junction on its path must fail";

                AssertStatus(STATUS_ACCESS_DENIED, status, message.c_str());

                targetBefore.AssertUnchanged(L"The refused rename must change nothing in the junction target");
                upperBefore.AssertUnchanged(L"The refused rename must change nothing in the upper");
                lowerBefore.AssertUnchanged(L"The refused rename must change nothing in the lower");
            }
        }
    }

    TEST_METHOD(Rename_WhiteoutNamedFileUnderLink_MovesTheTargetFile) {
        for (const LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                if (!LinkToWhiteoutNamedFileCreatedOrSkipped(env, linkSource, createLink)) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());

                AssertStatus(STATUS_SUCCESS,
                    mount.Rename(L"link\\.wh.foo", L"link\\.wh.bar", kFailIfExists, kNoCallerPid),
                    L"A rename of a .wh. file under a link to another .wh. name must succeed");
                AssertStatus(STATUS_SUCCESS,
                    mount.Rename(L"link\\.wh.bar", L"link\\plain", kFailIfExists, kNoCallerPid),
                    L"A rename of a .wh. file under a link to an ordinary name must succeed");
                AssertStatus(STATUS_SUCCESS,
                    mount.Rename(L"link\\plain", L"link\\.wh.back", kFailIfExists, kNoCallerPid),
                    L"A rename of a file under a link to a .wh. name must succeed");

                Assert::IsFalse(env.FileExists(env.Root(), WhiteoutMarkerPath(L"target\\foo")),
                    L"The renames must move the .wh.foo file away");
                Assert::AreEqual(std::string("target"), env.ReadFile(env.Root(), L"target\\.wh.back"),
                    L"The renames must leave the file at .wh.back in the link target");
                Assert::AreEqual(std::string("target"), ReadThroughMount(mount, L"link\\.wh.back"),
                    L"The mount must show the renamed file");
                AssertUpperLinkListsWholeTarget(env, mount, L"link", env.Root() + L"\\target");
            }
        }
    }

    TEST_METHOD(ReplaceRename_UpperDirectoryThatCannotMove_KeepsTheDestination) {
        for (const LowerChildHiding hiding : {LowerChildHiding::Whiteout, LowerChildHiding::OpaqueMarker}) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Upper(), L"src\\a.txt", "a");
            WriteDestinationHidingItsLowerChild(env, hiding);
            ::LayerMount::LayerMount mount(env.MakeConfig());

            NTSTATUS status = STATUS_SUCCESS;
            {
                // An open child without FILE_SHARE_DELETE blocks the move of src.
                const ScopedHandle heldChild =
                    HoldOpen(env.Upper() + L"\\src\\a.txt", FILE_SHARE_READ | FILE_SHARE_WRITE);
                status = mount.Rename(L"src", L"dst", kReplaceIfExists, kNoCallerPid);
            }

            Assert::IsFalse(NT_SUCCESS(status), L"The rename must fail while a child of src is open");
            AssertDestinationStillHidesItsLowerChild(env, mount);
            AssertOnlyEntryShownAs(mount, L"src", L"a.txt");
        }
    }

    TEST_METHOD(ReplaceRename_OntoOpaqueDirectoryThatRefusesNewMarkers_KeepsItsLowerChildHidden) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Upper(), L"src\\a.txt", "a");
            env.WriteFile(env.Lower(0), L"dst\\old.txt", "old");
            env.CreateDir(env.Upper(), L"dst");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            Assert::IsTrue(MetadataStore::SetOpaqueMetadata(env.Upper() + L"\\dst", &config),
                L"The opaque metadata must mark the upper dst");
            ::LayerMount::LayerMount mount(config);

            NTSTATUS status = STATUS_SUCCESS;
            {
                const std::wstring destination = env.Upper() + L"\\dst";
                const DirectoryRefusesNewEntries destinationRefusesMarkers(destination,
                                                                           FILE_ADD_FILE);
                Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED,
                    NewFileError(destination + L":probe"),
                    L"The upper dst must refuse a new stream");
                const ScopedHandle heldChild =
                    HoldOpen(env.Upper() + L"\\src\\a.txt", FILE_SHARE_READ | FILE_SHARE_WRITE);
                status = mount.Rename(L"src", L"dst", kReplaceIfExists, kNoCallerPid);
            }

            Assert::IsFalse(NT_SUCCESS(status),
                L"The rename must fail while a child of src is open");
            AssertDestinationStillHidesItsLowerChild(env, mount);
            AssertOnlyEntryShownAs(mount, L"src", L"a.txt");
        });
    }

    TEST_METHOD(ReplaceRename_DirectoryOntoOpaqueUpperDirectory_LeavesTheNewDirectoryNotOpaque) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Upper(), L"src\\a.txt", "a");
            env.CreateDir(env.Upper(), L"dst");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            WhiteoutManager markers(config, nullptr);
            AssertStatus(STATUS_SUCCESS, markers.SetOpaque(L"dst"),
                L"SetOpaque must mark the upper dst");
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SUCCESS,
                mount.Rename(L"src", L"dst", kReplaceIfExists, kNoCallerPid),
                L"A replace rename onto an empty opaque directory must succeed");

            AssertOnlyEntryShownAs(mount, L"", L"dst");
            AssertOnlyEntryShownAs(mount, L"dst", L"a.txt");
            Assert::IsFalse(markers.IsOpaque(L"dst"),
                L"The renamed directory must not take the opaque marker of the replaced dst");
        });
    }

    TEST_METHOD(ReplaceRename_LowerDirectoryThatCannotCopy_KeepsTheDestination) {
        for (const LowerChildHiding hiding : {LowerChildHiding::Whiteout, LowerChildHiding::OpaqueMarker}) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"src\\a.txt", "a");
            WriteDestinationHidingItsLowerChild(env, hiding);
            ::LayerMount::LayerMount mount(env.MakeConfig());

            NTSTATUS status = STATUS_SUCCESS;
            {
                // Without SE_BACKUP_NAME, the deny ACE stops the copy of src\a.txt.
                AccessDenied unreadableChild(env.Lower(0) + L"\\src\\a.txt", FILE_READ_DATA);
                BackupPrivilegeDisabledOnThread noBackupPrivilege;
                status = mount.Rename(L"src", L"dst", kReplaceIfExists, kNoCallerPid);
            }

            Assert::IsFalse(NT_SUCCESS(status), L"The rename must fail when src\\a.txt cannot be read");
            AssertDestinationStillHidesItsLowerChild(env, mount);
            AssertOnlyEntryShownAs(mount, L"src", L"a.txt");
        }
    }

    TEST_METHOD(ReplaceRename_FileOntoLowerJunction_ShowsOnlyTheFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        env.WriteFile(env.Lower(0), L"f.txt", "f");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Lower(0) + L"\\link",
                                  env.Root() + L"\\target")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));
        const LayerSnapshot targetBefore(env.Root() + L"\\target");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"f.txt", L"link", kReplaceIfExists, kNoCallerPid),
            L"A replace rename of a file onto a lower junction must succeed");
        AssertRootShowsOnlyFileNamed(mount, L"link");
        Assert::IsTrue(mount.MergeDirectoryEntries(L"link").entries.empty(),
            L"The file must hide the lower junction's entries");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
        targetBefore.AssertUnchanged(L"The rename must leave the junction target's entries");
    }

    TEST_METHOD(ReplaceRename_FileOntoDirectorySymlink_ReplacesTheSymlinkAndKeepsItsTarget) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        env.WriteFile(env.Upper(), L"f.txt", "f");
        if (!CreateDirectorySymlink(env.Upper() + L"\\link", env.Root() + L"\\target")) {
            Logger::WriteMessage(L"[SKIP] the upper directory symlink could not be created");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot targetBefore(env.Root() + L"\\target");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"f.txt", L"link", kReplaceIfExists, kNoCallerPid),
            L"A replace rename of a file onto a directory symlink must succeed");
        AssertRootShowsOnlyFileNamed(mount, L"link");
        Assert::AreEqual(std::string("f"), env.ReadFile(env.Upper(), L"link"),
            L"The upper link must be the moved file");
        targetBefore.AssertUnchanged(L"The rename must leave the symlink target's entries");
    }

    TEST_METHOD(ReplaceRename_FileOntoJunctionWhenTheMoveFails_RestoresTheJunction) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        env.WriteFile(env.Upper(), L"f.txt", "f");
        if (!CreateDirectoryJunction(env.Upper() + L"\\link", env.Root() + L"\\target")) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the upper junction");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());
        const LayerSnapshot targetBefore(env.Root() + L"\\target");

        NTSTATUS status = STATUS_SUCCESS;
        {
            // An open handle without FILE_SHARE_DELETE blocks the move of f.txt.
            const ScopedHandle heldSource =
                HoldOpen(env.Upper() + L"\\f.txt", FILE_SHARE_READ | FILE_SHARE_WRITE);
            status = mount.Rename(L"f.txt", L"link", kReplaceIfExists, kNoCallerPid);
        }

        Assert::IsFalse(NT_SUCCESS(status), L"The rename must fail while f.txt is open");
        upperBefore.AssertUnchanged(L"The failed rename must leave the upper as it was");
        targetBefore.AssertUnchanged(L"The failed rename must leave the junction target's entries");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\link", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper link must be a junction again");
    }

    TEST_METHOD(ReplaceRename_DirectoryOntoJunction_FailsWithNotADirectory) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            for (const DirectoryLayer junctionLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
                TempLayerEnvironment env(1);
                env.CreateDir(env.Root(), L"target");
                env.WriteFile(LayerRoot(env, sourceLayer), L"dir\\a.txt", "a");
                if (!CreateDirectoryJunction(LayerRoot(env, junctionLayer) + L"\\link",
                                             env.Root() + L"\\target")) {
                    Logger::WriteMessage(L"[SKIP] mklink /J could not create the junction");
                    return;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());
                const LayerSnapshot upperBefore(env.Upper());
                const LayerSnapshot lowerBefore(env.Lower(0));
                const LayerSnapshot targetBefore(env.Root() + L"\\target");

                AssertStatus(STATUS_NOT_A_DIRECTORY,
                    mount.Rename(L"dir", L"link", kReplaceIfExists, kNoCallerPid),
                    L"A replace rename of a directory onto a junction to an empty directory must fail");
                upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
                lowerBefore.AssertUnchanged(L"The refused rename must write nothing in the lower");
                targetBefore.AssertUnchanged(
                    L"The refused rename must write nothing in the junction target");
            }
        }
    }

    TEST_METHOD(ReplaceRenameOpenDirectory_OntoJunction_FailsAndKeepsTheHandleOpen) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Root(), L"target");
        env.WriteFile(env.Lower(0), L"dir\\a.txt", "a");
        if (!CreateDirectoryJunction(env.Upper() + L"\\link", env.Root() + L"\\target")) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the upper junction");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        AssertStatus(STATUS_SUCCESS, mount.Open(L"dir", FILE_LIST_DIRECTORY | DELETE,
                                                kNoCreateOptions, kNoCallerPid, &ctx, &info),
            L"The source directory must open");
        const LayerSnapshot upperBefore(env.Upper());
        const LayerSnapshot targetBefore(env.Root() + L"\\target");

        AssertReplaceRenameThroughHandleRefused(mount, ctx.get(), L"link", STATUS_NOT_A_DIRECTORY,
            L"A replace rename of an open directory onto a junction must fail");
        upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
        targetBefore.AssertUnchanged(L"The refused rename must write nothing in the junction target");
    }

    TEST_METHOD(ReplaceRenameOpenJunction_OntoDirectory_FailsAndKeepsTheHandleOpen) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Root(), L"target");
        env.CreateDir(env.Lower(0), L"dir");
        if (!CreateDirectoryJunction(env.Upper() + L"\\link", env.Root() + L"\\target")) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the upper junction");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        AssertStatus(STATUS_SUCCESS, mount.Open(L"link", FILE_LIST_DIRECTORY | DELETE,
                                                kNoCreateOptions, kNoCallerPid, &ctx, &info),
            L"The junction must open");
        const LayerSnapshot upperBefore(env.Upper());
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertReplaceRenameThroughHandleRefused(mount, ctx.get(), L"dir", STATUS_FILE_IS_A_DIRECTORY,
            L"A replace rename of an open junction onto an empty directory must fail");
        upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
        lowerBefore.AssertUnchanged(L"The refused rename must write nothing in the lower");
    }

    TEST_METHOD(ReplaceRename_JunctionOntoDirectory_FailsWithFileIsADirectory) {
        for (const DirectoryLayer destinationLayer : {DirectoryLayer::Lower, DirectoryLayer::Upper}) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
            env.CreateDir(LayerRoot(env, destinationLayer), L"dir");
            if (!CreateDirectoryJunction(env.Upper() + L"\\link", env.Root() + L"\\target")) {
                Logger::WriteMessage(L"[SKIP] mklink /J could not create the upper junction");
                return;
            }
            ::LayerMount::LayerMount mount(env.MakeConfig());
            const LayerSnapshot upperBefore(env.Upper());
            const LayerSnapshot lowerBefore(env.Lower(0));

            AssertStatus(STATUS_FILE_IS_A_DIRECTORY,
                mount.Rename(L"link", L"dir", kReplaceIfExists, kNoCallerPid),
                L"A replace rename of a junction onto an empty directory must fail");
            upperBefore.AssertUnchanged(L"The refused rename must write nothing in the upper");
            lowerBefore.AssertUnchanged(L"The refused rename must write nothing in the lower");
        }
    }

    TEST_METHOD(ReplaceRename_UpperJunctionOntoUpperFile_ReplacesTheFileWithTheJunction) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        env.WriteFile(env.Upper(), L"f.txt", "f");
        if (!CreateDirectoryJunction(env.Upper() + L"\\link", env.Root() + L"\\target")) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the upper junction");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot targetBefore(env.Root() + L"\\target");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"link", L"f.txt", kReplaceIfExists, kNoCallerPid),
            L"A replace rename of an upper junction onto an upper file must succeed");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\f.txt", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper f.txt must be the moved junction");
        AssertListedDirectoryWithChild(mount, L"f.txt", L"inside.txt");
        targetBefore.AssertUnchanged(L"The rename must leave the junction target's entries");
    }

    TEST_METHOD(ReplaceRename_UpperJunctionOntoLowerFile_WritesNoOpaqueMarkerIntoTheTarget) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        env.WriteFile(env.Lower(0), L"f.txt", "f");
        if (!CreateDirectoryJunction(env.Upper() + L"\\link", env.Root() + L"\\target")) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the upper junction");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"link", L"f.txt", kReplaceIfExists, kNoCallerPid),
            L"A replace rename of an upper junction onto a lower file must succeed");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\f.txt", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper f.txt must be the moved junction");
        AssertListedDirectoryWithChild(mount, L"f.txt", L"inside.txt");
        AssertNoOpaqueMarkerIn(env.Root() + L"\\target");
    }

    TEST_METHOD(ReplaceRename_LowerJunctionOntoLowerFile_CopiesTheJunctionUp) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        env.WriteFile(env.Lower(0), L"f.txt", "f");
        if (!CreateDirectoryJunction(env.Lower(0) + L"\\link", env.Root() + L"\\target")) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the lower junction");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));
        const LayerSnapshot targetBefore(env.Root() + L"\\target");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"link", L"f.txt", kReplaceIfExists, kNoCallerPid),
            L"A replace rename of a lower junction onto a lower file must succeed");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\f.txt", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper f.txt must be a junction");
        Assert::IsTrue(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"link")),
            L"The rename must leave a whiteout at the old path");
        AssertListedDirectoryWithChild(mount, L"f.txt", L"inside.txt");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
        targetBefore.AssertUnchanged(L"The rename must leave the junction target's entries");
        AssertNoOpaqueMarkerIn(env.Root() + L"\\target");
    }

    TEST_METHOD(ReplaceRename_JunctionOntoJunction_ReplacesTheDestinationLink) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"first\\a.txt", "a");
        env.WriteFile(env.Root(), L"second\\b.txt", "b");
        if (!CreateDirectoryJunction(env.Upper() + L"\\one", env.Root() + L"\\first") ||
            !CreateDirectoryJunction(env.Upper() + L"\\two", env.Root() + L"\\second")) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the upper junctions");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot firstBefore(env.Root() + L"\\first");
        const LayerSnapshot secondBefore(env.Root() + L"\\second");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"one", L"two", kReplaceIfExists, kNoCallerPid),
            L"A replace rename of a junction onto a junction must succeed");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\two", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper two must be the moved junction");
        AssertListedDirectoryWithChild(mount, L"two", L"a.txt");
        firstBefore.AssertUnchanged(L"The rename must leave the source junction target's entries");
        secondBefore.AssertUnchanged(L"The rename must leave the replaced junction target's entries");
    }

    TEST_METHOD(ReplaceRename_JunctionOntoLowerJunction_ShowsOnlyTheMovedLinkTarget) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"first\\a.txt", "a");
        env.WriteFile(env.Root(), L"second\\b.txt", "b");
        if (!CreateDirectoryJunction(env.Upper() + L"\\one", env.Root() + L"\\first") ||
            !CreateDirectoryJunction(env.Lower(0) + L"\\two", env.Root() + L"\\second")) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the junctions");
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));
        const LayerSnapshot secondBefore(env.Root() + L"\\second");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"one", L"two", kReplaceIfExists, kNoCallerPid),
            L"A replace rename of an upper junction onto a lower junction must succeed");
        AssertListedDirectoryWithChild(mount, L"two", L"a.txt");
        Assert::IsTrue(mount.MergeDirectoryEntries(L"two").entries.count(L"b.txt") == 0,
            L"The moved junction must hide the lower junction target's entries");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
        secondBefore.AssertUnchanged(L"The rename must leave the replaced junction target's entries");
    }

    TEST_METHOD(Rename_UpperDirectoryOverLowerJunction_MovesOnlyTheUpperDirectory) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Lower(0) + L"\\link",
                                  env.Root() + L"\\target")) {
            return;
        }
        env.WriteFile(env.Upper(), L"link\\upper.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));
        const LayerSnapshot targetBefore(env.Root() + L"\\target");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"link", L"moved", kFailIfExists, kNoCallerPid),
            L"A rename of an upper directory over a lower junction must succeed");
        Assert::IsFalse(HasAttribute(env.Upper() + L"\\moved", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper entry at moved must be a directory, not a link");
        AssertOnlyEntryShownAs(mount, L"moved", L"upper.txt");
        AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"link", L"moved");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
        targetBefore.AssertUnchanged(L"The rename must leave the junction target's entries");
        AssertNoOpaqueMarkerIn(env.Root() + L"\\target");
    }

    TEST_METHOD(Rename_UpperJunctionOverLowerDirectory_ListsTheJunctionTarget) {
        TempLayerEnvironment env(1);
        if (!BuildLinkOverDirectory(env, LayerSource::Upper, CreateDirectoryJunction)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));
        const LayerSnapshot targetBefore(env.Root() + L"\\target");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"link", L"moved", kFailIfExists, kNoCallerPid),
            L"A rename of an upper junction over a lower directory must succeed");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\moved", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper entry at moved must be the junction");
        AssertListsOnlyTheLinkTarget(mount, L"moved");
        AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"link", L"moved");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
        targetBefore.AssertUnchanged(L"The rename must leave the junction target's entries");
        AssertNoOpaqueMarkerIn(env.Root() + L"\\target");
    }

    TEST_METHOD(Rename_UpperJunctionOverLowerJunction_ListsTheUpperJunctionTarget) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"first\\a.txt", "a");
        env.WriteFile(env.Root(), L"second\\b.txt", "b");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Upper() + L"\\link",
                                  env.Root() + L"\\first") ||
            !LinkCreatedOrSkipped(CreateDirectoryJunction, env.Lower(0) + L"\\link",
                                  env.Root() + L"\\second")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));
        const LayerSnapshot secondBefore(env.Root() + L"\\second");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"link", L"moved", kFailIfExists, kNoCallerPid),
            L"A rename of an upper junction over a lower junction must succeed");
        AssertOnlyEntryShownAs(mount, L"moved", L"a.txt");
        AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"link", L"moved");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
        secondBefore.AssertUnchanged(L"The rename must leave the lower junction target's entries");
    }

    TEST_METHOD(Rename_UpperJunctionOverLowerFile_MovesTheJunction) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        env.WriteFile(env.Lower(0), L"link", "lower");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Upper() + L"\\link",
                                  env.Root() + L"\\target")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"link", L"moved", kFailIfExists, kNoCallerPid),
            L"A rename of an upper junction over a lower file must succeed");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\moved", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper entry at moved must be the junction");
        AssertOnlyEntryShownAs(mount, L"moved", L"inside.txt");
        AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"link", L"moved");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
        AssertNoOpaqueMarkerIn(env.Root() + L"\\target");
    }

    TEST_METHOD(Rename_UpperDirectoryOverLowerFile_MovesOnlyTheUpperDirectory) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d", "lower");
        env.WriteFile(env.Upper(), L"d\\upper.txt", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"d", L"moved", kFailIfExists, kNoCallerPid),
            L"A rename of an upper directory over a lower file must succeed");
        AssertOnlyEntryShownAs(mount, L"moved", L"upper.txt");
        AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"d", L"moved");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
    }

    TEST_METHOD(Rename_OpaqueUpperDirectoryOverLowerDirectory_KeepsTheLowerChildrenHidden) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\lower.txt", "lower");
            env.WriteFile(env.Lower(0), L"moved\\other.txt", "lower");
            env.WriteFile(env.Upper(), L"d\\upper.txt", "upper");
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"moved"), "");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            Assert::IsTrue(MetadataStore::SetOpaqueMetadata(env.Upper() + L"\\d", &config),
                L"The test must mark the upper d opaque in the metadata store");
            ::LayerMount::LayerMount mount(config);
            const LayerSnapshot lowerBefore(env.Lower(0));

            AssertStatus(STATUS_SUCCESS,
                mount.Rename(L"d", L"moved", kFailIfExists, kNoCallerPid),
                L"A rename of an opaque upper directory over a lower directory must succeed");
            AssertOnlyEntryShownAs(mount, L"moved", L"upper.txt");
            AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"d", L"moved");
            Assert::IsFalse(MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\d", &config),
                L"The rename must leave no opaque metadata at the old name");
            lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
        });
    }

    TEST_METHOD(Rename_UpperDirectoryUnderOpaqueParentOverHiddenLowerDirectory_MovesOnlyTheUpperChildren) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"p\\d\\lower.txt", "lower");
        env.WriteFile(env.Upper(), L"p\\d\\upper.txt", "upper");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"p"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"p\\d", L"p\\e", kFailIfExists, kNoCallerPid),
            L"A rename of an upper directory under an opaque parent must succeed");
        AssertOnlyEntryShownAs(mount, L"p\\e", L"upper.txt");
        AssertOnlyEntryShownAs(mount, L"p", L"e");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"p\\d")),
            L"The rename must write no whiteout for a name whose lower entry the opaque parent hides");
        Assert::IsFalse(env.FileExists(env.Upper(), OpaqueMarkerPath(L"p\\e")),
            L"The rename must write no opaque marker file into the new name");
        Assert::IsFalse(MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\p\\e", nullptr),
            L"The rename must write no opaque stream onto the new name");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
    }

    TEST_METHOD(Rename_UpperFileUnderOpaqueParentOverHiddenLowerFile_WritesNoWhiteout) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"p\\f.txt", "lower");
        env.WriteFile(env.Upper(), L"p\\f.txt", "upper");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"p"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"p\\f.txt", L"p\\g.txt", kFailIfExists, kNoCallerPid),
            L"A rename of an upper file under an opaque parent must succeed");
        AssertOnlyEntryShownAs(mount, L"p", L"g.txt");
        Assert::AreEqual(std::string("upper"), env.ReadFile(env.Upper(), L"p\\g.txt"),
            L"The new name must hold the upper file's data");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"p\\f.txt")),
            L"The rename must write no whiteout for a name whose lower entry the opaque parent hides");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
    }

    TEST_METHOD(Rename_UpperFileInOpaqueDirectory_KeepsTheDirectoryOpaque) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"box\\hidden.txt", "lower");
        env.WriteFile(env.Upper(), L"box\\a.txt", "data");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"box"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"box\\a.txt", L"box\\b.txt", kFailIfExists, kNoCallerPid),
            L"A rename of an upper file inside an opaque directory must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), OpaqueMarkerPath(L"box")),
            L"The directory must keep its opaque marker");
        AssertOnlyEntryShownAs(mount, L"box", L"b.txt");
        Assert::AreEqual(std::string("data"), ReadThroughMount(mount, L"box\\b.txt"),
            L"The new name must show the upper file's data");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"box\\a.txt")),
            L"The rename must write no whiteout for a name that no lower holds");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
    }

    TEST_METHOD(Rename_LowerOnlyFile_CopiesItUpAndWhitesOutTheOldName) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"source.txt", "payload");
        const LayerConfig config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);
        const LayerSnapshot lowerBefore(env.Lower(0));
        AssertOnlyEntryShownAs(mount, L"", L"source.txt");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"source.txt", L"target.txt", kFailIfExists, kNoCallerPid),
            L"A rename of a lower-only file must succeed");
        Assert::IsTrue(
            MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\target.txt", &config).metacopy,
            L"The upper target.txt must be a metacopy shell of the lower file");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"target.txt")),
            L"The rename must write no whiteout at the new name");
        AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"source.txt", L"target.txt");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"source.txt"),
            L"An open of the old name must find nothing");
        Assert::AreEqual(std::string("payload"), ReadThroughMount(mount, L"target.txt"),
            L"The new name must show the lower file's data");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
    }

    TEST_METHOD(Rename_FileInUpperAndLower_KeepsTheLowerFileHiddenAtTheOldName) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"shared.txt", "LOWER");
        env.WriteFile(env.Upper(), L"shared.txt", "UPPER");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"shared.txt", L"renamed.txt", kFailIfExists, kNoCallerPid),
            L"A rename of a file in the upper and the lower must succeed");
        AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"shared.txt", L"renamed.txt");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"shared.txt"),
            L"An open of the old name must not find the lower file");
        Assert::AreEqual(std::string("UPPER"), ReadThroughMount(mount, L"renamed.txt"),
            L"The new name must show the upper file's data");
        Assert::AreEqual(std::string("LOWER"), env.ReadFile(env.Lower(0), L"shared.txt"),
            L"The lower file must keep its data");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
    }

    TEST_METHOD(ReplaceRename_LowerFileOntoWhitedOutName_RemovesTheWhiteoutAndShowsTheFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"src.txt", "src-payload");
        env.WriteFile(env.Lower(0), L"target.txt", "target-lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"target.txt"), "");
        const LayerConfig config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"src.txt", L"target.txt", kReplaceIfExists, kNoCallerPid),
            L"A rename onto a whited-out name must succeed");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"target.txt")),
            L"The rename must remove the whiteout at the new name");
        Assert::IsTrue(
            MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\target.txt", &config).metacopy,
            L"The upper target.txt must be a metacopy shell of the renamed file");
        AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"src.txt", L"target.txt");
        Assert::AreEqual(std::string("src-payload"), ReadThroughMount(mount, L"target.txt"),
            L"The new name must show the renamed file's data");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
    }

    TEST_METHOD(Rename_LowerFileOntoWhitedOutNameWithoutReplace_RemovesTheWhiteoutAndShowsTheFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"src.txt", "src-payload");
        env.WriteFile(env.Lower(0), L"target.txt", "target-lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"target.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"src.txt", L"target.txt", kFailIfExists, kNoCallerPid),
            L"A rename onto a whited-out name must succeed without replace");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"target.txt")),
            L"The rename must remove the whiteout at the new name");
        AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"src.txt", L"target.txt");
        Assert::AreEqual(std::string("src-payload"), ReadThroughMount(mount, L"target.txt"),
            L"The new name must show the renamed file's data");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
    }

    TEST_METHOD(Rename_MetacopyShell_KeepsItsMetadataAndReadsTheLowerData) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"lazy.bin", "lazy lower data");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            {
                CopyUpAndRenameRig rig(config);
                AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpMetadataOnly(L"lazy.bin").status,
                    L"The metadata-only copy-up of the lower file must succeed");
            }
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SUCCESS,
                mount.Rename(L"lazy.bin", L"renamed.bin", kFailIfExists, kNoCallerPid),
                L"A rename of a metacopy shell must succeed");

            const LayerMountMetadata metadata =
                MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\renamed.bin", &config);
            Assert::IsTrue(metadata.metacopy,
                L"The upper renamed.bin must keep the metacopy flag");
            Assert::IsFalse(metadata.originLayer.empty(),
                L"The upper renamed.bin must keep its origin layer");
            AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"lazy.bin", L"renamed.bin");
            Assert::AreEqual(std::string("lazy lower data"), ReadThroughMount(mount, L"renamed.bin"),
                L"The new name must show the lower file's data");
        });
    }

    TEST_METHOD(Rename_LowerFile_LeavesAMetacopyShellThatAWriteOpenFills) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"a.bin", LowerFileData());
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SUCCESS, mount.Rename(L"a.bin", L"b.bin", kFailIfExists, kNoCallerPid),
                L"The rename of the lower file must succeed");

            AssertMetacopyShell(env.Upper() + L"\\b.bin", config);
            AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"a.bin", L"b.bin");
            OpenForWriteThroughMount(mount, L"b.bin");
            AssertFullCopy(env, L"b.bin", config);
            Assert::AreEqual(LowerFileDataStart(), ReadThroughMount(mount, L"b.bin"),
                L"The new name must show the lower file's data");
        });
    }

    TEST_METHOD(Rename_LowerFileWithoutSparseSupport_CopiesTheData) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a.bin", LowerFileData());
        LayerConfig config = env.MakeConfig();
        config.hostCapabilities &= ~LM_CAP_SPARSE_FILES;
        ::LayerMount::LayerMount mount(config);

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"a.bin", L"b.bin", kFailIfExists, kNoCallerPid),
            L"The rename of the lower file must succeed");

        AssertFullCopy(env, L"b.bin", config);
    }

    TEST_METHOD(Rename_LowerFileSymlink_MovesTheLinkItself) {
        TempLayerEnvironment env(1);
        if (!LinkToTargetCreatedOrSkipped(env, CreateFileSymlink, LinkTarget::File,
                                          env.Lower(0) + L"\\link")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"link", L"moved", kFailIfExists, kNoCallerPid),
            L"The rename of the lower file symlink must succeed");

        Assert::AreEqual(static_cast<DWORD>(IO_REPARSE_TAG_SYMLINK),
            ReparseTagOf(env.Upper() + L"\\moved"),
            L"The upper entry at moved must be a symbolic link");
    }

    TEST_METHOD(RenameOpenFile_LowerFileByAHandleWithDataAccess_CopiesTheData) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a.bin", LowerFileData());
        const LayerConfig config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        AssertStatus(STATUS_SUCCESS, mount.Open(L"a.bin", FILE_READ_DATA | DELETE,
                                                kNoCreateOptions, kNoCallerPid, &ctx, &info),
            L"The lower file must open");

        AssertStatus(STATUS_SUCCESS, mount.Rename(ctx.get(), L"b.bin", kFailIfExists, kNoCallerPid),
            L"The rename of the open file must succeed");
        char buffer[kReadThroughMountBytes] = {};
        ULONG read = 0;
        const NTSTATUS readStatus = mount.Read(ctx.get(), buffer, 0, sizeof(buffer), &read);
        mount.Close(ctx.get());

        AssertFullCopy(env, L"b.bin", config);
        AssertStatus(STATUS_SUCCESS, readStatus, L"A read through the renamed handle must succeed");
        Assert::AreEqual(LowerFileDataStart(), std::string(buffer, read),
            L"A read through the renamed handle must return the lower file's data");
    }

    TEST_METHOD(RenameOpenFile_LowerFileByAHandleWithoutDataAccess_LeavesAMetacopyShell) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a.bin", LowerFileData());
        const LayerConfig config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        AssertStatus(STATUS_SUCCESS, mount.Open(L"a.bin", FILE_READ_ATTRIBUTES | DELETE,
                                                kNoCreateOptions, kNoCallerPid, &ctx, &info),
            L"The lower file must open");

        AssertStatus(STATUS_SUCCESS, mount.Rename(ctx.get(), L"b.bin", kFailIfExists, kNoCallerPid),
            L"The rename of the open file must succeed");
        mount.Close(ctx.get());

        AssertMetacopyShell(env.Upper() + L"\\b.bin", config);
        OpenForWriteThroughMount(mount, L"b.bin");
        AssertFullCopy(env, L"b.bin", config);
    }

    TEST_METHOD(Rename_LowerFileWithAStream_ShowsTheStreamAtTheNewName) {
        TempLayerEnvironment env(1);
        WriteLowerFileWithZoneStream(env, L"a.bin", LowerFileData());
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"a.bin", L"b.bin", kFailIfExists, kNoCallerPid),
            L"The rename of the lower file must succeed");

        AssertZoneStreamShown(mount, L"b.bin");
    }

    TEST_METHOD(WriteOpen_LargeLowerFileWithAStreamWithoutDataAccess_ShowsTheStream) {
        TempLayerEnvironment env(1);
        WriteLowerFileWithZoneStream(env, L"big.bin", std::string(2 * 1024 * 1024, 'B'));
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};

        AssertStatus(STATUS_SUCCESS, mount.Open(L"big.bin", FILE_WRITE_ATTRIBUTES,
                                                kNoCreateOptions, kNoCallerPid, &ctx, &info),
            L"The open of the lower file for its attributes must succeed");
        mount.Close(ctx.get());

        AssertZoneStreamShown(mount, L"big.bin");
    }

    TEST_METHOD(SetInfo_TimesOfLargePartlyDehydratedCloudPlaceholderFileWithoutProvider_ChangeOnAMetacopyShell) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        constexpr size_t kSize = 2 * 1024 * 1024;
        constexpr ByteRange kDropped{512 * 1024, 1024 * 1024};
        if (!layers.DehydratedPlaceholderFileWithoutProviderOrSkipped(L"big.bin", kSize, kDropped)) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        const LayerConfig config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);

        const NTSTATUS status = SetTimesThroughHandle(mount, L"big.bin", FILE_WRITE_ATTRIBUTES);

        AssertStatus(STATUS_SUCCESS, status,
            L"The set of the last-write time of the placeholder file must succeed");
        Assert::AreEqual(LastWriteTimeChange().lastWriteTime,
                         LastWriteTimeThroughMount(mount, L"big.bin"),
            L"The file must show the last-write time that the set gave it");
        AssertMetacopyShell(env.Upper() + L"\\big.bin", config);
        Assert::IsTrue(
            HasAttribute(env.Lower(0) + L"\\big.bin", FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS),
            L"The lower placeholder file must stay dehydrated");
    }

    TEST_METHOD(SetSecurity_DaclOfLargePartlyDehydratedCloudPlaceholderFileWithoutProvider_ChangesOnAMetacopyShell) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        constexpr size_t kSize = 2 * 1024 * 1024;
        constexpr ByteRange kDropped{512 * 1024, 1024 * 1024};
        if (!layers.DehydratedPlaceholderFileWithoutProviderOrSkipped(L"big.bin", kSize, kDropped)) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        const LayerConfig config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);
        const std::wstring lowerPath = env.Lower(0) + L"\\big.bin";
        const WellKnownSid batch(WinBatchSid);

        const NTSTATUS status = SetDaclWithOneMoreAce(
            mount, L"big.bin", lowerPath,
            AceSpec{GRANT_ACCESS, FILE_READ_ATTRIBUTES, NO_INHERITANCE, batch.Get()});

        AssertStatus(STATUS_SUCCESS, status,
            L"The set of the DACL of the placeholder file must succeed");
        const std::wstring upperPath = env.Upper() + L"\\big.bin";
        AssertMetacopyShell(upperPath, config);
        const std::wstring addedAce =
            AceText(ACCESS_ALLOWED_ACE_TYPE, 0, FILE_READ_ATTRIBUTES, batch.Get());
        Assert::IsTrue(ExplicitAcesOf(upperPath).find(addedAce) != std::wstring::npos,
            L"The upper file must carry the ACE that the set added");
        Assert::IsTrue(HasAttribute(lowerPath, FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS),
            L"The lower placeholder file must stay dehydrated");
    }

    TEST_METHOD(SetSecurity_DaclThatDeniesWriteToEveryoneOnLargeLowerFile_LaterReadReturnsTheLowerData) {
        TempLayerEnvironment env(1);
        const std::string lowerData = "lower file data" + std::string(2 * 1024 * 1024, 'D');
        env.WriteFile(env.Lower(0), L"big.bin", lowerData);
        const LayerConfig config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);
        constexpr DWORD kWriteRights =
            FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
        const WellKnownSid everyone(WinWorldSid);
        AssertStatus(STATUS_SUCCESS,
            SetDaclWithOneMoreAce(mount, L"big.bin", env.Lower(0) + L"\\big.bin",
                                  AceSpec{DENY_ACCESS, kWriteRights, NO_INHERITANCE,
                                          everyone.Get()}),
            L"The set of a DACL that denies write to everyone must succeed");
        const std::wstring upperPath = env.Upper() + L"\\big.bin";
        Assert::IsTrue(MetadataStore::ReadLayerMountMetadata(upperPath, &config).metacopy,
            L"The set of the DACL must leave a metacopy shell in the upper");
        Assert::IsTrue(
            ExplicitAcesOf(upperPath).find(
                AceText(ACCESS_DENIED_ACE_TYPE, 0, kWriteRights, everyone.Get())) !=
                std::wstring::npos,
            L"The upper file must carry the ACE that denies write to everyone");

        Assert::AreEqual(lowerData.substr(0, kReadThroughMountBytes),
                         ReadThroughMount(mount, L"big.bin"),
            L"A read through the mount must return the lower file's data");
    }

    TEST_METHOD(SetInfo_TimesOfLowerFileByAHandleWithoutDataAccess_LeavesAMetacopyShell) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a.bin", LowerFileData());
        const LayerConfig config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);

        const NTSTATUS status = SetTimesThroughHandle(mount, L"a.bin", FILE_READ_ATTRIBUTES);

        AssertStatus(STATUS_SUCCESS, status, L"The set of the last-write time must succeed");
        AssertMetacopyShell(env.Upper() + L"\\a.bin", config);
        Assert::AreEqual(LowerFileDataStart(), ReadThroughMount(mount, L"a.bin"),
            L"A read through the mount must return the lower file's data");
    }

    TEST_METHOD(SetInfo_TimesOfLowerFileByAHandleWithDataAccess_CopiesTheData) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a.bin", LowerFileData());
        const LayerConfig config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);

        const NTSTATUS status = SetTimesThroughHandle(mount, L"a.bin", FILE_READ_DATA);

        AssertStatus(STATUS_SUCCESS, status, L"The set of the last-write time must succeed");
        AssertFullCopy(env, L"a.bin", config);
    }

    TEST_METHOD(SetInfo_TimesOfLowerFileByAStreamHandleWithoutDataAccess_CopiesTheData) {
        TempLayerEnvironment env(1);
        WriteLowerFileWithZoneStream(env, L"a.bin", LowerFileData());
        const LayerConfig config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);

        const NTSTATUS status =
            SetTimesThroughHandle(mount, L"a.bin:" + kZoneStream, FILE_READ_ATTRIBUTES);

        AssertStatus(STATUS_SUCCESS, status,
            L"The set of the last-write time through the stream handle must succeed");
        AssertFullCopy(env, L"a.bin", config);
    }

    TEST_METHOD(Overwrite_LowerFileByAHandleWithoutDataAccess_LeavesAnEmptyFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a.bin", LowerFileData());
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        AssertStatus(STATUS_SUCCESS, mount.Open(L"a.bin", FILE_READ_ATTRIBUTES,
                                                kNoCreateOptions, kNoCallerPid, &ctx, &info),
            L"The lower file must open");

        const NTSTATUS status = mount.Overwrite(ctx.get(), 0, FALSE, 0, nullptr);
        mount.Close(ctx.get());

        AssertStatus(STATUS_SUCCESS, status, L"The overwrite must succeed");
        Assert::AreEqual(0ULL, FileSizeThroughMount(mount, L"a.bin"),
            L"A data open after the overwrite must show an empty file");
    }

    TEST_METHOD(SetInfo_TimesByALowerHandleWithDataAccessAfterAnotherHandleLeftAMetacopyShell_ReadsTheLowerData) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a.bin", LowerFileData());
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        AssertStatus(STATUS_SUCCESS, mount.Open(L"a.bin", FILE_READ_DATA,
                                                kNoCreateOptions, kNoCallerPid, &ctx, &info),
            L"The lower file must open for its data");
        AssertStatus(STATUS_SUCCESS, SetTimesThroughHandle(mount, L"a.bin", FILE_READ_ATTRIBUTES),
            L"The set of the last-write time through a second handle must succeed");

        const NTSTATUS setStatus = mount.SetInfo(ctx.get(), LastWriteTimeChange(), nullptr);
        char buffer[kReadThroughMountBytes] = {};
        ULONG read = 0;
        const NTSTATUS readStatus = mount.Read(ctx.get(), buffer, 0, sizeof(buffer), &read);
        mount.Close(ctx.get());

        AssertStatus(STATUS_SUCCESS, setStatus,
            L"The set of the last-write time through the first handle must succeed");
        AssertStatus(STATUS_SUCCESS, readStatus, L"A read through the first handle must succeed");
        Assert::AreEqual(LowerFileDataStart(), std::string(buffer, read),
            L"A read through the first handle must return the lower file's data");
    }

    TEST_METHOD(CaseOnlyRename_LowerFile_LeavesAMetacopyShellListedInTheNewCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a.bin", LowerFileData());
        const LayerConfig config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"a.bin", L"A.bin", kFailIfExists, kNoCallerPid),
            L"The case-only rename of the lower file must succeed");

        AssertMetacopyShell(env.Upper() + L"\\A.bin", config);
        Assert::AreEqual(LowerFileDataStart(), ReadThroughMount(mount, L"A.bin"),
            L"The new name must show the lower file's data");
        AssertOnlyEntryShownAs(mount, L"", L"A.bin");
    }

    TEST_METHOD(Rename_UpperDirectoryOntoNameWhoseLowerEntryAnOpaqueParentHides_Succeeds) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"p\\x\\lower.txt", "lower");
        env.WriteFile(env.Upper(), L"p\\d\\upper.txt", "upper");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"p"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"p\\d", L"p\\x", kFailIfExists, kNoCallerPid),
            L"A rename onto a name that the overlay does not show must succeed");
        AssertOnlyEntryShownAs(mount, L"p", L"x");
        AssertOnlyEntryShownAs(mount, L"p\\x", L"upper.txt");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
    }

    TEST_METHOD(Rename_LowerOnlyDirectory_CopiesTheTreeUpOpaqueAndWhitesOutTheOldName) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"ld\\top.txt", "top");
        env.WriteFile(env.Lower(0), L"ld\\nested\\inner.txt", "inner");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"ld", L"newdir", kFailIfExists, kNoCallerPid),
            L"A rename of a lower-only directory must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"newdir\\top.txt"),
            L"The upper newdir must hold the lower directory's file");
        Assert::AreEqual(std::string("inner"), env.ReadFile(env.Upper(), L"newdir\\nested\\inner.txt"),
            L"The upper newdir must hold the lower directory's nested file");
        Assert::IsTrue(env.FileExists(env.Upper(), OpaqueMarkerPath(L"newdir")),
            L"The upper newdir must be opaque");
        AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"ld", L"newdir");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
    }

    TEST_METHOD(ReplaceRename_UpperDirectoryOverLowerJunctionWithUnreadableTag_FailsAndChangesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        const std::wstring junction = env.Lower(0) + L"\\link";
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, junction, env.Root() + L"\\target")) {
            return;
        }
        env.WriteFile(env.Upper(), L"link\\upper.txt", "upper");
        env.CreateDir(env.Upper(), L"dst");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());
        const LayerSnapshot lowerBefore(env.Lower(0));

        NTSTATUS status = STATUS_SUCCESS;
        {
            LinkAccessDenied synchronizeDenied(junction, SYNCHRONIZE);
            BackupPrivilegeDisabledOnThread noBackupPrivilege;
            DisableRestorePrivilegeOnThread();
            Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, LinkTagOpenError(junction),
                L"The deny ACE must make the junction's reparse tag unreadable");
            status = mount.Rename(L"link", L"dst", kReplaceIfExists, kNoCallerPid);
        }

        AssertStatus(STATUS_ACCESS_DENIED, status,
            L"The rename must fail when the reparse tag of the lower entry cannot be read");
        upperBefore.AssertUnchanged(L"The failed rename must leave the upper as it was");
        lowerBefore.AssertUnchanged(L"The failed rename must write nothing in the lower");
        Assert::IsTrue(mount.MergeDirectoryEntries(L"dst").entries.empty(),
            L"The failed rename must leave dst empty");
        AssertOnlyEntryShownAs(mount, L"link", L"upper.txt");
    }

    TEST_METHOD(ReplaceRename_UpperJunctionOverLowerDirectoryOntoUpperFile_ListsTheJunctionTarget) {
        TempLayerEnvironment env(1);
        if (!BuildLinkOverDirectory(env, LayerSource::Upper, CreateDirectoryJunction)) {
            return;
        }
        env.WriteFile(env.Upper(), L"f.txt", "f");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));
        const LayerSnapshot targetBefore(env.Root() + L"\\target");

        AssertStatus(STATUS_SUCCESS,
            mount.Rename(L"link", L"f.txt", kReplaceIfExists, kNoCallerPid),
            L"A replace rename of an upper junction over a lower directory onto an upper file must succeed");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\f.txt", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper f.txt must be the moved junction, not the replaced file");
        AssertListsOnlyTheLinkTarget(mount, L"f.txt");
        AssertRootShowsOnlyNewNameOverWhiteout(env, mount, L"link", L"f.txt");
        lowerBefore.AssertUnchanged(L"The rename must write nothing in the lower");
        targetBefore.AssertUnchanged(L"The rename must leave the junction target's entries");
    }

    TEST_METHOD(Rename_UpperJunctionOverLowerDirectoryWhenTheWhiteoutFails_KeepsTheOldName) {
        TempLayerEnvironment env(1);
        if (!BuildLinkOverDirectory(env, LayerSource::Upper, CreateDirectoryJunction)) {
            return;
        }
        env.WriteFile(env.Lower(0), L"moved\\old.txt", "old");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"moved"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());
        const LayerSnapshot lowerBefore(env.Lower(0));

        NTSTATUS status = STATUS_SUCCESS;
        {
            const UpperRootRefusesNewFiles noNewFiles(env.Upper());
            status = mount.Rename(L"link", L"moved", kFailIfExists, kNoCallerPid);
        }

        AssertStatus(STATUS_ACCESS_DENIED, status,
            L"The rename must fail when the engine cannot write the whiteout at the old name");
        upperBefore.AssertUnchanged(L"The failed rename must leave the upper as it was");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\link", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The upper link must be the junction again");
        AssertOnlyEntryShownAs(mount, L"", L"link");
        AssertListsOnlyTheLinkTarget(mount, L"link");
        lowerBefore.AssertUnchanged(L"The failed rename must write nothing in the lower");
    }

    TEST_METHOD(Rename_UpperDirectoryOntoWhitedOutLowerDirectoryWhenTheOpaqueMarkerFails_KeepsTheOldName) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Upper(), L"src\\a.txt", "upper");
            env.WriteFile(env.Lower(0), L"dst\\old.txt", "lower");
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"dst"), "");
            env.CreateDir(env.Upper(), kSidecarDirName);
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);
            const LayerSnapshot upperBefore(env.Upper());
            const LayerSnapshot lowerBefore(env.Lower(0));

            NTSTATUS status = STATUS_SUCCESS;
            {
                const std::wstring source = env.Upper() + L"\\src";
                const std::wstring sidecar = env.Upper() + L"\\" + kSidecarDirName;
                const DirectoryRefusesNewEntries sourceRefusesNewFiles(source, FILE_ADD_FILE);
                const DirectoryRefusesNewEntries sidecarRefusesNewFiles(sidecar, FILE_ADD_FILE);
                Assert::AreEqual<DWORD>(ERROR_ACCESS_DENIED, NewFileError(source + L":probe"),
                    L"The upper src must refuse a new stream");
                status = mount.Rename(L"src", L"dst", kFailIfExists, kNoCallerPid);
            }

            AssertStatus(STATUS_ACCESS_DENIED, status,
                L"The rename must fail when the engine cannot mark the directory opaque");
            upperBefore.AssertUnchanged(L"The failed rename must leave the upper as it was");
            lowerBefore.AssertUnchanged(L"The failed rename must write nothing in the lower");
            AssertListedDirectoryWithChild(mount, L"src", L"a.txt");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"dst"),
                L"An open of dst after the failed rename must find nothing");
        });
    }

    TEST_METHOD(Rename_OpaqueUpperDirectoryOntoWhitedOutLowerDirectoryWhenTheMarkerCannotMove_KeepsTheOldName) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Upper(), L"src\\a.txt", "upper");
            env.WriteFile(env.Lower(0), L"dst\\old.txt", "lower");
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"dst"), "");
            env.CreateDir(env.Upper(), kSidecarDirName);
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            Assert::IsTrue(MetadataStore::SetOpaqueMetadata(env.Upper() + L"\\src", &config),
                L"The opaque metadata must mark the upper src");
            ::LayerMount::LayerMount mount(config);
            const LayerSnapshot upperBefore(env.Upper());

            NTSTATUS status = STATUS_SUCCESS;
            {
                const DirectoryRefusesNewEntries sidecarRefusesNewFiles(
                    env.Upper() + L"\\" + kSidecarDirName, FILE_ADD_FILE);
                status = mount.Rename(L"src", L"dst", kFailIfExists, kNoCallerPid);
            }

            if (capabilities != kHostCapabilitiesWithoutAds) {
                AssertStatus(STATUS_SUCCESS, status,
                    L"The rename must succeed, because the opaque stream moves with the directory");
                AssertOnlyEntryShownAs(mount, L"dst", L"a.txt");
                return;
            }
            AssertStatus(STATUS_ACCESS_DENIED, status,
                L"The rename must fail when the opaque marker cannot move");
            upperBefore.AssertUnchanged(L"The failed rename must leave the upper as it was");
            Assert::IsTrue(MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\src", &config),
                L"The upper src must keep its opaque marker");
            AssertListedDirectoryWithChild(mount, L"src", L"a.txt");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"dst"),
                L"The whiteout at dst must still hide the lower dst");
        });
    }

    TEST_METHOD(Rename_UpperDirectoryWhoseChildRecordCannotMove_LeavesTheUpperAsItWas) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"src\\a.txt", "a");
        LayerConfig config = env.MakeConfig();
        config.hostCapabilities = kHostCapabilitiesWithoutAds;
        Assert::IsTrue(MetadataStore::SetOpaqueMetadata(env.Upper() + L"\\src", &config),
            L"The opaque metadata must mark the upper src");
        LayerMountMetadata childRecord;
        childRecord.hasStableIndexNumber = true;
        childRecord.stableIndexNumber = 42;
        Assert::IsTrue(MetadataStore::WriteLayerMountMetadata(env.Upper() + L"\\src\\a.txt",
                                                              childRecord, &config),
            L"WriteLayerMountMetadata must write the record of src\\a.txt");
        ::LayerMount::LayerMount mount(config);
        const LayerSnapshot upperBefore(env.Upper());

        NTSTATUS status = STATUS_SUCCESS;
        {
            const std::wstring sidecar = env.Upper() + L"\\" + kSidecarDirName;
            Assert::AreEqual(size_t{1}, FileNamesMatching(sidecar, L"*.meta.json").size(),
                L"The sidecar directory must hold only the record of src\\a.txt");
            const ScopedHandle childRecordHeldAgainstMove = HoldOpen(
                sidecar + L"\\" + StoredLeafName(sidecar + L"\\*.meta.json"),
                FILE_SHARE_READ | FILE_SHARE_WRITE);
            status = mount.Rename(L"src", L"dst", kFailIfExists, kNoCallerPid);
        }

        AssertStatus(STATUS_SHARING_VIOLATION, status,
            L"The rename must fail when the record of a child cannot move");
        upperBefore.AssertUnchanged(L"The failed rename must leave the upper as it was");
        Assert::IsTrue(MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\src", &config),
            L"The upper src must keep its opaque marker");
        Assert::AreEqual(uint64_t{42},
            MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\src\\a.txt", &config)
                .stableIndexNumber,
            L"The upper src\\a.txt must keep its record");
        AssertListedDirectoryWithChild(mount, L"src", L"a.txt");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"dst"),
            L"An open of dst after the failed rename must find nothing");
    }

    TEST_METHOD(Rename_CopiedUpFileWhoseRecordCannotMove_KeepsItsRecordAtTheOldName) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"a.txt", "lower");
            env.CreateDir(env.Upper(), kSidecarDirName);
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);
            AssertStatus(STATUS_SUCCESS, mount.EnsureInUpperLayer(L"a.txt"),
                L"The copy-up of the lower file must succeed");
            const UINT64 copiedUpId = IndexNumberThroughMount(mount, L"a.txt", kNoCreateOptions);
            const LayerSnapshot upperBefore(env.Upper());

            NTSTATUS status = STATUS_SUCCESS;
            {
                const DirectoryRefusesNewEntries sidecarRefusesNewFiles(
                    env.Upper() + L"\\" + kSidecarDirName, FILE_ADD_FILE);
                status = mount.Rename(L"a.txt", L"b.txt", kFailIfExists, kNoCallerPid);
            }

            if (capabilities != kHostCapabilitiesWithoutAds) {
                AssertStatus(STATUS_SUCCESS, status,
                    L"The rename must succeed, because the record stream moves with the file");
                Assert::AreEqual(copiedUpId,
                    IndexNumberThroughMount(mount, L"b.txt", kNoCreateOptions),
                    L"The renamed file must keep its ID");
                return;
            }
            AssertStatus(STATUS_ACCESS_DENIED, status,
                L"The rename must fail when the record cannot move");
            upperBefore.AssertUnchanged(L"The failed rename must leave the upper as it was");
            Assert::AreEqual(copiedUpId, IndexNumberThroughMount(mount, L"a.txt", kNoCreateOptions),
                L"The file must keep its ID at the old name");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"b.txt"),
                L"An open of b.txt after the failed rename must find nothing");
        });
    }

    TEST_METHOD(Rename_UpperDirectoryWhoseOpaqueMarkerCannotMove_KeepsItsRecordAtTheOldName) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"src\\a.txt", "a");
        LayerConfig config = env.MakeConfig();
        config.hostCapabilities = kHostCapabilitiesWithoutAds;
        Assert::IsTrue(MetadataStore::SetOpaqueMetadata(env.Upper() + L"\\src", &config),
            L"The opaque metadata must mark the upper src");
        WriteStableIndexRecord(env.Upper() + L"\\src", 42, config);
        ::LayerMount::LayerMount mount(config);
        const LayerSnapshot upperBefore(env.Upper());

        NTSTATUS status = STATUS_SUCCESS;
        {
            const std::wstring sidecar = env.Upper() + L"\\" + kSidecarDirName;
            Assert::AreEqual(size_t{1}, FileNamesMatching(sidecar, L"*.opaque").size(),
                L"The sidecar directory must hold only the opaque marker of src");
            const ScopedHandle markerHeldAgainstMove = HoldOpen(
                sidecar + L"\\" + StoredLeafName(sidecar + L"\\*.opaque"),
                FILE_SHARE_READ | FILE_SHARE_WRITE);
            status = mount.Rename(L"src", L"dst", kFailIfExists, kNoCallerPid);
        }

        AssertStatus(STATUS_SHARING_VIOLATION, status,
            L"The rename must fail when the opaque marker of src cannot move");
        upperBefore.AssertUnchanged(L"The failed rename must leave the upper as it was");
        Assert::AreEqual(uint64_t{42},
            MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\src", &config)
                .stableIndexNumber,
            L"The upper src must keep its record");
        Assert::IsTrue(MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\src", &config),
            L"The upper src must keep its opaque marker");
        AssertListedDirectoryWithChild(mount, L"src", L"a.txt");
    }

    TEST_METHOD(Rename_UpperDirectoryThatCannotMoveBackAfterAChildRecordStuck_MovesTheOtherRecords) {
        TempLayerEnvironment env(1);
        for (const wchar_t* child : {L"a.txt", L"b.txt", L"c.txt"}) {
            env.WriteFile(env.Upper(), std::wstring(L"d\\s\\") + child, "child");
        }
        LayerConfig config = env.MakeConfig();
        config.hostCapabilities = kHostCapabilitiesWithoutAds;
        const std::wstring sidecar = env.Upper() + L"\\" + kSidecarDirName;
        WriteStableIndexRecord(env.Upper() + L"\\d\\s\\b.txt", 2, config);
        const std::wstring stuckRecord = StoredLeafName(sidecar + L"\\*.meta.json");
        WriteStableIndexRecord(env.Upper() + L"\\d\\s\\a.txt", 1, config);
        WriteStableIndexRecord(env.Upper() + L"\\d\\s\\c.txt", 3, config);
        ::LayerMount::LayerMount mount(config);

        NTSTATUS status = STATUS_SUCCESS;
        {
            const DirectoryRefusesNewEntries oldParentRefusesNewEntries(
                env.Upper() + L"\\d", FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
            const ScopedHandle recordHeldAgainstMove =
                HoldOpen(sidecar + L"\\" + stuckRecord, FILE_SHARE_READ | FILE_SHARE_WRITE);
            status = mount.Rename(L"d\\s", L"s2", kFailIfExists, kNoCallerPid);
        }

        AssertStatus(STATUS_SUCCESS, status,
            L"The rename must stand when the directory cannot move back");
        const MergedDirectory children = mount.MergeDirectoryEntries(L"s2");
        AssertStatus(STATUS_SUCCESS, children.status, L"The listing of s2 must succeed");
        Assert::AreEqual(size_t{3}, children.entries.size(), L"s2 must show its three children");
        Assert::AreEqual(uint64_t{1},
            MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\s2\\a.txt", &config)
                .stableIndexNumber,
            L"The record of a.txt, which moved before the stuck record, must stay with it");
        Assert::AreEqual(uint64_t{3},
            MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\s2\\c.txt", &config)
                .stableIndexNumber,
            L"The record of c.txt, which comes after the stuck record, must move with it");
        Assert::IsTrue(env.FileExists(sidecar, stuckRecord),
            L"The record that cannot move must stay keyed to the old path of b.txt");
    }

    TEST_METHOD(Rename_UpperDirectoryThatCannotMoveBackAfterAChildRecordStuck_EmitsAWarningAtTheNewName) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"d\\s\\a.txt", "a");
        LayerConfig config = env.MakeConfig();
        config.hostCapabilities = kHostCapabilitiesWithoutAds;
        const std::wstring sidecar = env.Upper() + L"\\" + kSidecarDirName;
        WriteStableIndexRecord(env.Upper() + L"\\d\\s\\a.txt", 1, config);
        EmittedWarnings warnings;
        ::LayerMount::LayerMount mount(config);
        mount.Events().Set(&CollectWarning, &warnings);

        {
            const DirectoryRefusesNewEntries oldParentRefusesNewEntries(
                env.Upper() + L"\\d", FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
            const ScopedHandle recordHeldAgainstMove = HoldOpen(
                sidecar + L"\\" + StoredLeafName(sidecar + L"\\*.meta.json"),
                FILE_SHARE_READ | FILE_SHARE_WRITE);
            AssertStatus(STATUS_SUCCESS,
                mount.Rename(L"d\\s", L"s2", kFailIfExists, kNoCallerPid),
                L"The rename must stand when the directory cannot move back");
        }

        Assert::AreEqual<size_t>(1, warnings.results.size(),
            L"The record that did not move with the directory must emit one warning");
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_SHARING_VIOLATION), warnings.results[0],
            L"The warning must carry the failure of the record move");
        Assert::AreEqual(std::wstring(L"s2"), warnings.paths[0],
            L"The warning must name the path where the directory stays");
    }

    TEST_METHOD(Rename_UpperDirectoryWithAChildDirectoryThatCannotBeListed_LeavesTheUpperAsItWas) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"src\\a\\x.txt", "x");
        env.WriteFile(env.Upper(), L"src\\b.txt", "b");
        LayerConfig config = env.MakeConfig();
        config.hostCapabilities = kHostCapabilitiesWithoutAds;
        WriteStableIndexRecord(env.Upper() + L"\\src\\b.txt", 42, config);
        ::LayerMount::LayerMount mount(config);
        const LayerSnapshot upperBefore(env.Upper());

        NTSTATUS status = STATUS_SUCCESS;
        {
            const DirectoryListingDenied listingDenied(env.Upper() + L"\\src\\a");
            const BackupPrivilegeDisabledOnThread noBackupPrivilege;
            AssertListingDenied(env.Upper() + L"\\src\\a");
            status = mount.Rename(L"src", L"dst", kFailIfExists, kNoCallerPid);
        }

        AssertStatus(STATUS_ACCESS_DENIED, status,
            L"The rename must fail when the walk cannot list a directory below src");
        upperBefore.AssertUnchanged(L"The failed rename must leave the upper as it was");
        Assert::AreEqual(uint64_t{42},
            MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\src\\b.txt", &config)
                .stableIndexNumber,
            L"The upper src\\b.txt must keep its record");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"dst"),
            L"An open of dst after the failed rename must find nothing");
    }

    TEST_METHOD(ReplaceRename_WhenARecordOfTheDestinationCannotComeBack_BringsBackTheDestinationWithoutIt) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"src\\a.txt", "a");
        env.WriteFile(env.Lower(0), L"src", "lower file");
        env.CreateDir(env.Upper(), L"dst");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"dst"), "");
        LayerConfig config = env.MakeConfig();
        config.hostCapabilities = kHostCapabilitiesWithoutAds;
        Assert::IsTrue(MetadataStore::SetOpaqueMetadata(env.Upper() + L"\\dst", &config),
            L"The opaque metadata must mark the upper dst");
        WriteStableIndexRecord(env.Upper() + L"\\dst", 42, config);
        const std::wstring sidecar = env.Upper() + L"\\" + kSidecarDirName;
        const std::wstring recordAtDestination = StoredLeafName(sidecar + L"\\*.meta.json");
        RecordHeldAtWhiteout hold{sidecar, {}, ScopedHandle(), {}};
        ::LayerMount::LayerMount mount(config);
        mount.Events().Set(&HoldRecordAtFirstWhiteout, &hold);

        NTSTATUS status = STATUS_SUCCESS;
        {
            // The whiteout at the old name is the last step that succeeds,
            // and the held whiteout at the new name then fails the rename.
            const ScopedHandle heldDestinationWhiteout =
                HoldOpen(env.Upper() + L"\\" + WhiteoutMarkerPath(L"dst"),
                         FILE_SHARE_READ | FILE_SHARE_WRITE);
            status = mount.Rename(L"src", L"dst", kReplaceIfExists, kNoCallerPid);
            hold.held.Reset();
        }
        mount.Events().Clear();

        AssertStatus(STATUS_SHARING_VIOLATION, status,
            L"The rename must fail when the whiteout at the new name cannot go");
        Assert::IsFalse(hold.heldName.empty(),
            L"The test must hold the record of dst after the engine moved it aside");
        Assert::AreNotEqual(recordAtDestination, hold.heldName,
            L"The held record must be keyed to the path of dst in the work directory");
        Assert::IsTrue(env.FileExists(env.Upper(), L"dst"),
            L"The upper dst must come back");
        const MergedDirectory listing = mount.MergeDirectoryEntries(L"dst");
        AssertStatus(STATUS_SUCCESS, listing.status, L"The listing of dst must succeed");
        Assert::IsTrue(env.FileExists(sidecar, hold.heldName),
            L"The record that cannot come back must stay keyed to the path in the work directory");
        Assert::IsFalse(MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\dst", &config),
            L"dst must come back without the marker that stayed with the stuck record");
        AssertOnlyEntryShownAs(mount, L"src", L"a.txt");
        Assert::AreEqual(size_t{1}, hold.warnings.paths.size(),
            L"The record that cannot come back must emit one warning");
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_SHARING_VIOLATION),
            hold.warnings.results[0], L"The warning must carry the error of the record move");
        Assert::AreEqual(std::wstring(L"dst"), hold.warnings.paths[0],
            L"The warning must name dst");
    }

    TEST_METHOD(Rename_LowerDirectoryWhenTheWhiteoutFails_KeepsTheOldName) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\a.txt", "a");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));

        NTSTATUS status = STATUS_SUCCESS;
        {
            const UpperRootRefusesNewFiles noNewFiles(env.Upper());
            status = mount.Rename(L"d", L"moved", kFailIfExists, kNoCallerPid);
        }

        AssertStatus(STATUS_ACCESS_DENIED, status,
            L"The rename must fail when the engine cannot write the whiteout at the old name");
        Assert::IsFalse(env.FileExists(env.Upper(), L"moved"),
            L"The failed rename must leave no upper entry at the new name");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"d")),
            L"The failed rename must leave no whiteout at the old name");
        AssertListedDirectoryWithChild(mount, L"d", L"a.txt");
        lowerBefore.AssertUnchanged(L"The failed rename must write nothing in the lower");
    }

    TEST_METHOD(ReplaceRename_UpperFileOntoUpperFileWhenTheWhiteoutFails_KeepsTheReplacedFile) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"a.txt", "lower");
            env.WriteFile(env.Upper(), L"a.txt", "upper");
            env.WriteFile(env.Upper(), L"b.txt", "keep");
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"a.txt"), "");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);
            const LayerSnapshot upperBefore(env.Upper());
            const LayerSnapshot lowerBefore(env.Lower(0));

            NTSTATUS status = STATUS_SUCCESS;
            {
                const ScopedHandle heldMarker =
                    HoldOpen(env.Upper() + L"\\" + WhiteoutMarkerPath(L"a.txt"), 0);
                status = mount.Rename(L"a.txt", L"b.txt", kReplaceIfExists, kNoCallerPid);
            }

            AssertStatus(STATUS_SHARING_VIOLATION, status,
                L"The rename must fail when the engine cannot write the whiteout at the old name");
            upperBefore.AssertUnchanged(L"The failed rename must leave the upper as it was");
            lowerBefore.AssertUnchanged(L"The failed rename must write nothing in the lower");
            Assert::AreEqual(std::string("keep"), ReadThroughMount(mount, L"b.txt"),
                L"b.txt must still show the replaced file");
            Assert::AreEqual(std::string("upper"), ReadThroughMount(mount, L"a.txt"),
                L"a.txt must show the upper file again");
        });
    }

    TEST_METHOD(Rename_FileOntoWhitedOutNameWhenTheWhiteoutCannotGo_KeepsTheOldName) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Upper, DirectoryLayer::Lower}) {
            TempLayerEnvironment env(1);
            env.WriteFile(LayerRoot(env, sourceLayer), L"a.txt", "source");
            env.WriteFile(env.Lower(0), L"b.txt", "lower");
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"b.txt"), "");
            ::LayerMount::LayerMount mount(env.MakeConfig());

            NTSTATUS status = STATUS_SUCCESS;
            {
                const ScopedHandle heldMarker =
                    HoldOpen(env.Upper() + L"\\" + WhiteoutMarkerPath(L"b.txt"),
                             FILE_SHARE_READ | FILE_SHARE_WRITE);
                status = mount.Rename(L"a.txt", L"b.txt", kFailIfExists, kNoCallerPid);
            }

            AssertStatus(STATUS_SHARING_VIOLATION, status,
                L"The rename must fail when the whiteout at the new name cannot go");
            Assert::IsFalse(env.FileExists(env.Upper(), L"b.txt"),
                L"The failed rename must leave no upper entry at the new name");
            Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"a.txt")),
                L"The failed rename must leave no whiteout at the old name");
            AssertOnlyEntryShownAs(mount, L"", L"a.txt");
            Assert::AreEqual(std::string("source"), ReadThroughMount(mount, L"a.txt"),
                L"a.txt must still show the source file");
        }
    }

    TEST_METHOD(Rename_UpperDirectoryOntoWhitedOutNameWhenTheWhiteoutCannotGo_LeavesTheSourceNotOpaque) {
        for (const bool lowerFileAtSource : {false, true}) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Upper(), L"src\\a.txt", "a");
            if (lowerFileAtSource) {
                env.WriteFile(env.Lower(0), L"src", "lower file");
            }
            env.WriteFile(env.Lower(0), L"dst\\old.txt", "old");
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"dst"), "");
            const LayerConfig config = env.MakeConfig();
            ::LayerMount::LayerMount mount(config);

            NTSTATUS status = STATUS_SUCCESS;
            {
                const ScopedHandle heldMarker =
                    HoldOpen(env.Upper() + L"\\" + WhiteoutMarkerPath(L"dst"),
                             FILE_SHARE_READ | FILE_SHARE_WRITE);
                status = mount.Rename(L"src", L"dst", kFailIfExists, kNoCallerPid);
            }

            AssertStatus(STATUS_SHARING_VIOLATION, status,
                L"The rename must fail when the whiteout at the new name cannot go");
            AssertOnlyEntryShownAs(mount, L"src", L"a.txt");
            const WhiteoutManager markers(config, nullptr);
            Assert::IsFalse(markers.IsOpaque(L"src"),
                L"The undone rename must take off the opaque marker it gave src");
        }
    }

    TEST_METHOD(ReplaceRename_WhenTheWhiteoutCannotGoAndTheFileCannotMoveBack_KeepsTheReplacedFileInTheWorkDirectory) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"d\\a.txt", "source");
        env.WriteFile(env.Upper(), L"b.txt", "keep");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"b.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        NTSTATUS status = STATUS_SUCCESS;
        {
            const DirectoryRefusesNewEntries sourceParentRefusesNewFiles(env.Upper() + L"\\d",
                                                                         FILE_ADD_FILE);
            const ScopedHandle heldMarker =
                HoldOpen(env.Upper() + L"\\" + WhiteoutMarkerPath(L"b.txt"),
                         FILE_SHARE_READ | FILE_SHARE_WRITE);
            status = mount.Rename(L"d\\a.txt", L"b.txt", kReplaceIfExists, kNoCallerPid);
        }

        AssertStatus(STATUS_SHARING_VIOLATION, status,
            L"The rename must fail when the whiteout at the new name cannot go");
        Assert::IsTrue(WorkHoldsFileWithContent(env, "keep"),
            L"The work directory must keep the replaced b.txt");
    }

    TEST_METHOD(ReplaceRename_WhenTheOldNameWhiteoutCannotBeWrittenAndTheFileCannotMoveBack_KeepsTheReplacedFileInTheWorkDirectory) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\a.txt", "lower");
        env.WriteFile(env.Upper(), L"d\\a.txt", "source");
        env.WriteFile(env.Upper(), L"b.txt", "keep");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        NTSTATUS status = STATUS_SUCCESS;
        {
            const DirectoryRefusesNewEntries sourceParentRefusesWhiteoutAndMoveBack(
                env.Upper() + L"\\d", FILE_ADD_FILE);
            status = mount.Rename(L"d\\a.txt", L"b.txt", kReplaceIfExists, kNoCallerPid);
        }

        AssertStatus(STATUS_ACCESS_DENIED, status,
            L"The rename must fail when the engine cannot write the whiteout at the old name");
        Assert::IsTrue(WorkHoldsFileWithContent(env, "keep"),
            L"The work directory must keep the replaced b.txt");
    }

    TEST_METHOD(ReplaceRename_DirectoryWhenTheOldNameWhiteoutCannotBeWrittenAndItCannotMoveBack_KeepsTheReplacedDirectoryInTheWorkDirectory) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\s\\old.txt", "old");
        env.WriteFile(env.Upper(), L"d\\s\\a.txt", "a");
        env.WriteFile(env.Upper(), OpaqueMarkerPath(L"d\\s"), "");
        env.WriteFile(env.Lower(0), L"dst\\gone.txt", "gone");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"dst\\gone.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        NTSTATUS status = STATUS_SUCCESS;
        {
            const DirectoryRefusesNewEntries sourceParentRefusesWhiteoutAndMoveBack(
                env.Upper() + L"\\d", FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY);
            status = mount.Rename(L"d\\s", L"dst", kReplaceIfExists, kNoCallerPid);
        }

        AssertStatus(STATUS_ACCESS_DENIED, status,
            L"The rename must fail when the engine cannot write the whiteout at the old name");
        Assert::IsTrue(WorkHoldsFileNamed(env, WhiteoutMarkerPath(L"gone.txt")),
            L"The work directory must keep the replaced dst with its whiteout marker");
    }

    TEST_METHOD(ReplaceRename_MergedDirectoryWhenItsUpperCannotMoveAsideAndItsCopyCannotGo_KeepsTheReplacedDirectoryInTheWorkDirectory) {
        TempLayerEnvironment env(1);
        WriteMergedSrc(env);
        env.WriteFile(env.Lower(0), L"dst\\gone.txt", "gone");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"dst\\gone.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_ACCESS_DENIED,
            RenameSrcToDstWhenItsUpperCannotMoveAsideAndItsCopyCannotGo(env, mount, kReplaceIfExists),
            L"The rename must fail when the upper src cannot move aside");
        Assert::IsTrue(env.FileExists(env.Upper(), L"dst\\sub"),
            L"The merged copy of src must stay at dst when its removal fails");
        Assert::IsTrue(WorkHoldsFileNamed(env, WhiteoutMarkerPath(L"gone.txt")),
            L"The work directory must keep the replaced dst with its whiteout marker");
    }

    TEST_METHOD(Rename_MergedDirectoryWhenItsUpperCannotMoveAsideAndItsCopyCannotGo_EmitsAWarningAtTheNewName) {
        for (const BOOLEAN replace : {kFailIfExists, kReplaceIfExists}) {
            TempLayerEnvironment env(1);
            WriteMergedSrc(env);
            EmittedWarnings warnings;
            ::LayerMount::LayerMount mount(env.MakeConfig());
            mount.Events().Set(&CollectWarning, &warnings);

            AssertStatus(STATUS_ACCESS_DENIED,
                RenameSrcToDstWhenItsUpperCannotMoveAsideAndItsCopyCannotGo(env, mount, replace),
                L"The rename must fail when the upper src cannot move aside");
            Assert::AreEqual<size_t>(1, warnings.results.size(),
                L"The copy that stays at the new name must emit one warning");
            Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_ACCESS_DENIED), warnings.results[0],
                L"The warning must carry the failure of the removal of the copy");
            Assert::AreEqual(std::wstring(L"dst"), warnings.paths[0],
                L"The warning must name the path where the copy stays");
        }
    }

    TEST_METHOD(ReplaceRename_WhenTheWhiteoutCannotGoAndTheFileCannotMoveBack_EmitsAWarningAtTheNewName) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"d\\a.txt", "source");
        env.WriteFile(env.Upper(), L"b.txt", "keep");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"b.txt"), "");
        EmittedWarnings warnings;
        ::LayerMount::LayerMount mount(env.MakeConfig());
        mount.Events().Set(&CollectWarning, &warnings);

        {
            const DirectoryRefusesNewEntries sourceParentRefusesNewFiles(env.Upper() + L"\\d",
                                                                         FILE_ADD_FILE);
            const ScopedHandle heldMarker =
                HoldOpen(env.Upper() + L"\\" + WhiteoutMarkerPath(L"b.txt"),
                         FILE_SHARE_READ | FILE_SHARE_WRITE);
            AssertStatus(STATUS_SHARING_VIOLATION,
                mount.Rename(L"d\\a.txt", L"b.txt", kReplaceIfExists, kNoCallerPid),
                L"The rename must fail when the whiteout at the new name cannot go");
        }

        Assert::AreEqual<size_t>(1, warnings.results.size(),
            L"The undo that cannot move the file back must emit one warning");
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_ACCESS_DENIED), warnings.results[0],
            L"The warning must carry the failure of the move back");
        Assert::AreEqual(std::wstring(L"b.txt"), warnings.paths[0],
            L"The warning must name the path where the file stays");
    }

    TEST_METHOD(Rename_DirectoryOntoWhitedOutNameWhenTheWhiteoutCannotGo_KeepsTheOldName) {
        for (const DirectoryLayer sourceLayer : {DirectoryLayer::Upper, DirectoryLayer::Lower}) {
            TempLayerEnvironment env(1);
            env.WriteFile(LayerRoot(env, sourceLayer), L"src\\a.txt", "a");
            env.WriteFile(env.Lower(0), L"dst\\old.txt", "old");
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"dst"), "");
            ::LayerMount::LayerMount mount(env.MakeConfig());

            NTSTATUS status = STATUS_SUCCESS;
            {
                const ScopedHandle heldMarker =
                    HoldOpen(env.Upper() + L"\\" + WhiteoutMarkerPath(L"dst"),
                             FILE_SHARE_READ | FILE_SHARE_WRITE);
                status = mount.Rename(L"src", L"dst", kFailIfExists, kNoCallerPid);
            }

            AssertStatus(STATUS_SHARING_VIOLATION, status,
                L"The rename must fail when the whiteout at the new name cannot go");
            Assert::IsFalse(env.FileExists(env.Upper(), L"dst"),
                L"The failed rename must leave no upper entry at the new name");
            Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"src")),
                L"The failed rename must leave no whiteout at the old name");
            AssertOnlyEntryShownAs(mount, L"", L"src");
            AssertOnlyEntryShownAs(mount, L"src", L"a.txt");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"dst"),
                L"An open of dst after the failed rename must find nothing");
        }
    }

    TEST_METHOD(Rename_MergedDirectoryWhoseUpperCannotGo_KeepsEveryChildAtTheOldName) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"src\\a.txt", "upper only");
        env.WriteFile(env.Upper(), L"src\\x.txt", "held");
        env.WriteFile(env.Lower(0), L"src\\b.txt", "lower");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        NTSTATUS status = STATUS_SUCCESS;
        {
            // An open child without FILE_SHARE_DELETE lets the copy read it
            // but keeps the upper src from going. NTFS lists a.txt first, so a
            // delete of the upper src removes a.txt before it fails on x.txt.
            const ScopedHandle heldChild =
                HoldOpen(env.Upper() + L"\\src\\x.txt", FILE_SHARE_READ | FILE_SHARE_WRITE);
            status = mount.Rename(L"src", L"dst", kFailIfExists, kNoCallerPid);
        }

        Assert::IsFalse(NT_SUCCESS(status), L"The rename must fail when the upper src cannot go");
        Assert::IsFalse(env.FileExists(env.Upper(), L"dst"),
            L"The failed rename must remove the copy at the new name");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"src")),
            L"The failed rename must leave no whiteout at the old name");
        AssertOnlyEntryShownAs(mount, L"", L"src");
        AssertEntryShownAs(mount, L"src", L"a.txt", L"a.txt");
        AssertEntryShownAs(mount, L"src", L"b.txt", L"b.txt");
        AssertEntryShownAs(mount, L"src", L"x.txt", L"x.txt");
        Assert::AreEqual(std::string("upper only"), ReadThroughMount(mount, L"src\\a.txt"),
            L"src\\a.txt must still show the upper file");
    }

    TEST_METHOD(ReplaceRename_FileOntoReadOnlyUpperFile_FailsAndChangesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a.txt", "lower");
        env.WriteFile(env.Upper(), L"b.txt", "keep");
        Assert::IsTrue(::SetFileAttributesW((env.Upper() + L"\\b.txt").c_str(),
                                            FILE_ATTRIBUTE_READONLY) != FALSE,
            L"The test must make the upper b.txt read-only");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());
        const LayerSnapshot workBefore(env.Staging());

        AssertStatus(STATUS_ACCESS_DENIED,
            mount.Rename(L"a.txt", L"b.txt", kReplaceIfExists, kNoCallerPid),
            L"A replace-rename onto a read-only upper file must fail");
        upperBefore.AssertUnchanged(L"The refused rename must not copy a.txt up");
        workBefore.AssertUnchanged(L"The refused rename must leave nothing in the work directory");
        Assert::AreEqual(std::string("keep"), ReadThroughMount(mount, L"b.txt"),
            L"b.txt must still show the read-only file");
    }
};

TEST_CLASS(MountDirectoryCopyUpTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(Create_FileInCloudPlaceholderLowerDirectory_CopiesTheDirectoryUpAsAPlainDirectory) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        if (!layers.PlaceholderWithFileOrSkipped(L"d", L"d\\x.txt", "lower")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d\\new.txt", kNoCreateOptions),
            L"A create of a file in the cloud placeholder directory must succeed");

        const std::wstring upperD = env.Upper() + L"\\d";
        Assert::IsTrue(HasAttribute(upperD, FILE_ATTRIBUTE_DIRECTORY),
            L"The create must copy d up as a directory");
        Assert::IsFalse(HasAttribute(upperD, FILE_ATTRIBUTE_REPARSE_POINT),
            L"The copy of d must be a plain directory");
        Assert::IsTrue(env.FileExists(env.Upper(), L"d\\new.txt"),
            L"The new file must be in the copy of d");
    }

    TEST_METHOD(Create_FileInCloudPlaceholderLowerDirectory_GivesTheCopyTheLowerDirectorysCreationTime) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        if (!layers.PlaceholderWithFileOrSkipped(L"d", L"d\\x.txt", "lower")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        const FILETIME lowerCreation = MakeFileTime(2011, 6, 7);
        StampTimes(env.Lower(0) + L"\\d", lowerCreation, MakeFileTime(2011, 8, 9),
                   MakeFileTime(2011, 10, 11));
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d\\new.txt", kNoCreateOptions),
            L"A create of a file in the cloud placeholder directory must succeed");

        FILETIME creation{};
        FILETIME access{};
        FILETIME write{};
        GetTimes(env.Upper() + L"\\d", &creation, &access, &write);
        Assert::IsTrue(FileTimesEqual(lowerCreation, creation),
            L"The copy of d must keep the creation time of the lower d");
    }

    TEST_METHOD(Create_FileInPinnedCloudPlaceholderLowerDirectory_GivesTheCopyNoPinnedAttribute) {
        CloudPlaceholderLayers layers{SyncRootLayer::Lower};
        if (!layers.PlaceholderWithFileOrSkipped(L"d", L"d\\x.txt", "lower") ||
            !layers.syncRoot.PinnedOrSkipped(layers.env.Lower(0) + L"\\d")) {
            return;
        }
        TempLayerEnvironment& env = layers.env;
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d\\new.txt", kNoCreateOptions),
            L"A create of a file in the pinned cloud placeholder directory must succeed");

        Assert::IsFalse(HasAttribute(env.Upper() + L"\\d", FILE_ATTRIBUTE_PINNED),
            L"The copy of d must not carry the pin state of the lower d");
    }

    TEST_METHOD(Create_FileInLowerDirectoryWithUnhandledReparseTag_FailsAndCopiesNothingUp) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"d");
        if (!NonLinkReparseTagSetOrSkipped(env.Lower(0) + L"\\d")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_IO_REPARSE_TAG_NOT_HANDLED,
            CreateThroughMount(mount, L"d\\new.txt", kNoCreateOptions),
            L"A create in a directory whose reparse tag no filter handles must fail");

        Assert::IsFalse(env.FileExists(env.Upper(), L"d"),
            L"The failed copy-up must leave nothing at d in the upper");
        Assert::IsTrue(EntriesUnder(env.Staging()).empty(),
            L"The failed copy-up must leave nothing in the work directory");
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

    TEST_METHOD(WriteOpen_LowerFile_ListsTheFileInTheLowersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Readme.TXT", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, OpenForWriteAndClose(mount, L"readme.txt"),
            L"A write open of the lower file must succeed");
        Assert::AreEqual(std::wstring(L"Readme.TXT"), StoredLeafName(env.Upper() + L"\\readme.txt"),
            L"The write open must copy the file up under the lower's name");

        AssertOnlyEntryShownAs(mount, L"", L"Readme.TXT");
    }

    TEST_METHOD(CreateStream_OnLowerFile_ListsTheFileInTheLowersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Readme.TXT", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS,
            CreateThroughMount(mount, L"readme.txt:notes", kNoCreateOptions),
            L"A create of a stream on the lower file must succeed");
        Assert::AreEqual(std::wstring(L"Readme.TXT"), StoredLeafName(env.Upper() + L"\\readme.txt"),
            L"The stream create must copy the file up under the lower's name");

        AssertOnlyEntryShownAs(mount, L"", L"Readme.TXT");
    }

    TEST_METHOD(Create_NewFile_ListsTheFileInTheCallersCase) {
        TempLayerEnvironment env(1);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"NewFile.TXT", kNoCreateOptions),
            L"A create of a new file must succeed");
        Assert::AreEqual(std::wstring(L"NewFile.TXT"),
            StoredLeafName(env.Upper() + L"\\newfile.txt"),
            L"The create must name the upper file in the caller's case");

        AssertOnlyEntryShownAs(mount, L"", L"NewFile.TXT");
    }

    TEST_METHOD(Create_NewDirectory_ListsTheDirectoryInTheCallersCase) {
        TempLayerEnvironment env(1);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"NewDir", FILE_DIRECTORY_FILE),
            L"A create of a new directory must succeed");
        Assert::AreEqual(std::wstring(L"NewDir"), StoredLeafName(env.Upper() + L"\\newdir"),
            L"The create must name the upper directory in the caller's case");

        AssertOnlyEntryShownAs(mount, L"", L"NewDir");
    }

    TEST_METHOD(OverwriteOpen_LowerFile_ListsTheFileInTheLowersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Readme.TXT", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        constexpr UINT32 keepAttributes = 0u;
        constexpr BOOLEAN mergeAttributes = FALSE;

        std::unique_ptr<::LayerMount::FileContext> ctx;
        ::LayerMount::InternalFileInfo info{};
        AssertStatus(STATUS_SUCCESS,
            mount.Open(L"readme.txt", FILE_READ_DATA, kNoCreateOptions, kNoCallerPid, &ctx, &info),
            L"A read open of the lower file must succeed");
        const NTSTATUS overwriteStatus = mount.Overwrite(ctx.get(), keepAttributes,
                                                         mergeAttributes, kNoAllocationSize, &info);
        mount.Close(ctx.get());

        AssertStatus(STATUS_SUCCESS, overwriteStatus, L"The overwrite of the lower file must succeed");
        Assert::AreEqual(std::wstring(L"Readme.TXT"), StoredLeafName(env.Upper() + L"\\readme.txt"),
            L"The overwrite must copy the file up under the lower's name");

        AssertOnlyEntryShownAs(mount, L"", L"Readme.TXT");
    }

    TEST_METHOD(Create_OverDeletedLowerFile_ListsTheFileInTheCallersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Docs.txt", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Delete(L"docs.txt", kNoCallerPid),
            L"The delete of the lower file must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"docs.txt")),
            L"The delete must write a whiteout");
        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"DOCS.TXT", kNoCreateOptions),
            L"A create over the deleted lower file must succeed");

        AssertOnlyEntryShownAs(mount, L"", L"DOCS.TXT");
    }

    TEST_METHOD(Create_OverDeletedLowerDirectory_ListsTheDirectoryInTheCallersCase) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"Docs");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Delete(L"docs", kNoCallerPid),
            L"The delete of the lower directory must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"docs")),
            L"The delete must write a whiteout");
        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"DOCS", FILE_DIRECTORY_FILE),
            L"A create over the deleted lower directory must succeed");

        AssertOnlyEntryShownAs(mount, L"", L"DOCS");
    }

    TEST_METHOD(CreateStream_NewStream_KeepsTheCallersCase) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"readme.txt", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS,
            CreateThroughMount(mount, L"readme.txt:Notes", kNoCreateOptions),
            L"A create of a new stream must succeed");

        std::vector<::LayerMount::InternalStreamInfo> streams;
        AssertStatus(STATUS_SUCCESS, mount.EnumerateStreams(L"readme.txt", streams),
            L"The stream listing must succeed");
        Assert::AreEqual(size_t{1}, streams.size(), L"The file must hold one named stream");
        Assert::AreEqual(std::wstring(L":Notes:$DATA"), streams[0].name,
            L"The stream must keep the name in the caller's case");
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

    TEST_METHOD(Create_UnderLowerJunction_CreatesTheChildInTheJunctionTarget) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        const std::wstring target = env.Root() + L"\\target";
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Lower(0) + L"\\link", target)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot lowerBefore(env.Lower(0));

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"link\\child.txt", kNoCreateOptions),
            L"A create under the lower junction must succeed");

        Assert::IsTrue(env.FileExists(target, L"child.txt"),
            L"The create must put child.txt in the junction target");
        AssertEntryShownAs(mount, L"link", L"child.txt", L"child.txt");
        Assert::IsTrue(HasAttribute(env.Lower(0) + L"\\link", FILE_ATTRIBUTE_REPARSE_POINT),
            L"The lower link must stay a junction");
        lowerBefore.AssertUnchanged(L"The create must not change the lower's entries");
    }

    TEST_METHOD(Create_TwoLevelsUnderLowerLink_CreatesTheChildInTheLinkTarget) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            for (const UINT32 createOptions : {kNoCreateOptions, static_cast<UINT32>(FILE_DIRECTORY_FILE)}) {
                TempLayerEnvironment env(1);
                env.WriteFile(env.Root(), L"target\\sub\\inside.txt", "inside");
                if (!LinkCreatedOrSkipped(createLink, env.Lower(0) + L"\\link",
                                          env.Root() + L"\\target")) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());

                AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"link\\sub\\new", createOptions),
                    L"A create two levels under a lower link must succeed");

                Assert::IsTrue(env.FileExists(env.Root(), L"target\\sub\\new"),
                    L"The create must put new in the link target");
                AssertEntryShownAs(mount, L"link\\sub", L"new", L"new");
                AssertUpperLinkListsWholeTarget(env, mount, L"link", env.Root() + L"\\target");
            }
        }
    }

    TEST_METHOD(WriteOpen_FileUnderLowerLink_OpensTheTargetFile) {
        constexpr size_t kAboveShellThreshold = 2 * 1024 * 1024;
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Root(), L"target\\foo", "foo");
            env.WriteFile(env.Root(), L"target\\sub\\big.bin", std::string(kAboveShellThreshold, 'b'));
            if (!LinkCreatedOrSkipped(createLink, env.Lower(0) + L"\\link", env.Root() + L"\\target")) {
                continue;
            }
            ::LayerMount::LayerMount mount(env.MakeConfig());

            AssertStatus(STATUS_SUCCESS, OpenForWriteAndClose(mount, L"link\\foo"),
                L"A write open of a file under a lower link must succeed");
            AssertStatus(STATUS_SUCCESS, OpenForWriteAndClose(mount, L"link\\sub\\big.bin"),
                L"A write open of a large file two levels under a lower link must succeed");

            Assert::AreEqual(std::string("foo"), ReadThroughMount(mount, L"link\\foo"),
                L"The mount must show the target's foo");
            Assert::AreEqual(std::string(kReadThroughMountBytes, 'b'),
                ReadThroughMount(mount, L"link\\sub\\big.bin"),
                L"The mount must show the target's data of big.bin");
            Assert::IsFalse(HasAttribute(env.Root() + L"\\target\\sub\\big.bin",
                                         FILE_ATTRIBUTE_SPARSE_FILE),
                L"The write open must not make the target's file a sparse shell");
            AssertUpperLinkListsWholeTarget(env, mount, L"link", env.Root() + L"\\target");
        }
    }

    TEST_METHOD(SetInfo_AttributesOfFileUnderLowerJunction_ChangeTheTargetFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Root(), L"target\\foo", "foo");
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, env.Lower(0) + L"\\link",
                                  env.Root() + L"\\target")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};
        Assert::IsTrue(NT_SUCCESS(mount.Open(L"link\\foo", FILE_READ_ATTRIBUTES, kNoCreateOptions,
                                             kNoCallerPid, &ctx, &info)),
            L"Preconditions: the file under the lower junction must open");
        const SetInfoRequest hide{FILE_ATTRIBUTE_HIDDEN, 0, 0, 0, 0, kUnchangedSize, kUnchangedSize};

        const NTSTATUS status = mount.SetInfo(ctx.get(), hide, nullptr);
        mount.Close(ctx.get());

        AssertStatus(STATUS_SUCCESS, status, L"A set of attributes on a file under a lower junction must succeed");
        Assert::IsTrue(HasAttribute(env.Root() + L"\\target\\foo", FILE_ATTRIBUTE_HIDDEN),
            L"The set must change the attributes of the target's file");
        AssertUpperLinkListsWholeTarget(env, mount, L"link", env.Root() + L"\\target");
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
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            CreateThroughMount(mount, L"foo\\b.txt", kNoCreateOptions),
            L"A create under a lower file must fail");
        upperBefore.AssertUnchanged(L"The failed create must write nothing in the upper");
        AssertRootShowsOnlyFileNamed(mount, L"Foo");
    }

    TEST_METHOD(Create_TwoLevelsUnderLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            CreateThroughMount(mount, L"foo\\bar\\b.txt", kNoCreateOptions),
            L"A create two levels under a lower file must fail");
        upperBefore.AssertUnchanged(L"The failed create must write nothing in the upper");
        AssertRootShowsOnlyFileNamed(mount, L"Foo");
    }

    TEST_METHOD(Create_UnderWhitedOutLowerFile_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"Foo", "x");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"Foo"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            CreateThroughMount(mount, L"foo\\b.txt", kNoCreateOptions),
            L"A create under a whited-out lower file must fail");
        upperBefore.AssertUnchanged(L"The failed create must write nothing in the upper");
    }

    TEST_METHOD(Create_UnderParentInNoLayer_FailsAndWritesNothing) {
        TempLayerEnvironment env(1);
        ::LayerMount::LayerMount mount(env.MakeConfig());
        const LayerSnapshot upperBefore(env.Upper());

        AssertStatus(STATUS_OBJECT_PATH_NOT_FOUND,
            CreateThroughMount(mount, L"NewParent\\b.txt", kNoCreateOptions),
            L"A create under a parent in no layer must fail");
        upperBefore.AssertUnchanged(L"The failed create must write nothing in the upper");
    }

    TEST_METHOD(Create_UnderParentCreatedThroughTheOverlay_Succeeds) {
        TempLayerEnvironment env(1);
        ::LayerMount::LayerMount mount(env.MakeConfig());
        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"NewParent", FILE_DIRECTORY_FILE),
            L"The create of the directory NewParent must succeed");

        AssertStatus(STATUS_SUCCESS,
            CreateThroughMount(mount, L"NewParent\\b.txt", kNoCreateOptions),
            L"A create under a directory made through the overlay must succeed");
        AssertOnlyEntryShownAs(mount, L"NewParent", L"b.txt");
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
