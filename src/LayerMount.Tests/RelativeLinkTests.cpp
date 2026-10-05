#include "pch.h"
#include "TestFixture.h"

#include <set>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

namespace {

bool RelativeLowerLinkBuiltOrSkipped(const TempLayerEnvironment& env) {
    env.WriteFile(env.Lower(0), L"target\\foo", "foo");
    env.CreateDir(env.Lower(0), L"a");
    return LinkCreatedOrSkipped(CreateDirectorySymlink, env.Lower(0) + L"\\a\\link",
                                L"..\\target");
}

std::set<std::wstring> ListingKeys(const ::LayerMount::LayerMount& mount, const std::wstring& dir) {
    const MergedDirectory listed = mount.MergeDirectoryEntries(dir);
    AssertStatus(STATUS_SUCCESS, listed.status,
        (L"The listing of " + dir + L" must succeed").c_str());
    std::set<std::wstring> keys;
    for (const auto& entry : listed.entries) {
        keys.insert(entry.first);
    }
    return keys;
}

std::unique_ptr<FileContext> OpenLinkItselfWithDeleteAccess(::LayerMount::LayerMount& mount,
                                                     const std::wstring& link) {
    std::unique_ptr<FileContext> ctx;
    InternalFileInfo info{};
    AssertStatus(STATUS_SUCCESS,
        mount.Open(link, DELETE | FILE_READ_ATTRIBUTES, FILE_OPEN_REPARSE_POINT, kNoCallerPid,
                   &ctx, &info),
        (L"The open of the link " + link + L" itself must succeed").c_str());
    return ctx;
}

void AssertTargetFooKept(const TempLayerEnvironment& env, ::LayerMount::LayerMount& mount) {
    Assert::AreEqual(std::string("foo"), env.ReadFile(env.Lower(0), L"target\\foo"),
        L"The lower's target\\foo must stay");
    Assert::AreEqual(std::string("foo"), ReadThroughMount(mount, L"target\\foo"),
        L"The view's target\\foo must stay");
}

}

TEST_CLASS(RelativeLinkTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(Write_FileUnderRelativeLowerLink_ChangesTheViewTargetAndCopiesNoLink) {
        TempLayerEnvironment env(1);
        if (!RelativeLowerLinkBuiltOrSkipped(env)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        WriteThroughMount(mount, L"a\\link\\foo", "first");

        Assert::IsFalse(env.FileExists(env.Upper(), L"a\\link"),
            L"The write must not copy the relative link up");
        Assert::AreEqual(std::string("first"), env.ReadFile(env.Upper(), L"target\\foo"),
            L"The write must copy target\\foo up and change it");
        Assert::AreEqual(std::string("foo"), env.ReadFile(env.Lower(0), L"target\\foo"),
            L"The write must leave the lower's target\\foo as it was");
        Assert::AreEqual(std::string("first"), ReadThroughMount(mount, L"target\\foo"),
            L"The view's target\\foo must show the write");
        Assert::AreEqual(std::string("first"), ReadThroughMount(mount, L"a\\link\\foo"),
            L"The view's a\\link\\foo must show the write");

        WriteThroughMount(mount, L"a\\link\\foo", "again");

        Assert::AreEqual(std::string("again"), ReadThroughMount(mount, L"target\\foo"),
            L"A second write through the link must change the same file");
        Assert::IsFalse(env.FileExists(env.Upper(), L"a\\link"),
            L"A second write must not copy the relative link up");
    }

    TEST_METHOD(Open_FileUnderRelativeUpperLink_OpensTheViewTarget) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"target\\foo", "foo");
        env.CreateDir(env.Upper(), L"a");
        if (!LinkCreatedOrSkipped(CreateDirectorySymlink, env.Upper() + L"\\a\\link",
                                  L"..\\target")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(std::string("foo"), ReadThroughMount(mount, L"a\\link\\foo"),
            L"A relative link in the upper must resolve in the view, where the lower holds target");
    }

    TEST_METHOD(Open_FileUnderRelativeLinkAfterADeleteOfTheTargetFile_FailsWithNotFound) {
        TempLayerEnvironment env(1);
        if (!RelativeLowerLinkBuiltOrSkipped(env)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Delete(L"target\\foo", kNoCallerPid),
            L"The delete of target\\foo must succeed");

        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"a\\link\\foo"),
            L"The view must not show a\\link\\foo once target\\foo is deleted");
    }

    TEST_METHOD(MergeDirectoryEntries_RelativeLowerLink_ListsTheMergedTarget) {
        TempLayerEnvironment env(1);
        if (!RelativeLowerLinkBuiltOrSkipped(env)) {
            return;
        }
        env.WriteFile(env.Lower(0), L"target\\kept.txt", "kept");
        ::LayerMount::LayerMount mount(env.MakeConfig());
        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"target\\new.txt", kNoCreateOptions),
            L"Preconditions: the create of target\\new.txt must succeed");
        AssertStatus(STATUS_SUCCESS, mount.Delete(L"target\\foo", kNoCallerPid),
            L"Preconditions: the delete of target\\foo must succeed");

        const std::set<std::wstring> expected{L"kept.txt", L"new.txt"};
        Assert::IsTrue(expected == ListingKeys(mount, L"target"),
            L"Preconditions: the view's target must list kept.txt and new.txt");
        Assert::IsTrue(expected == ListingKeys(mount, L"a\\link"),
            L"The listing of a\\link must be the merged listing of target");
    }

    TEST_METHOD(Create_UnderRelativeLowerLink_CreatesTheEntryInTheViewTarget) {
        TempLayerEnvironment env(1);
        if (!RelativeLowerLinkBuiltOrSkipped(env)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"a\\link\\new.txt", kNoCreateOptions),
            L"A create under the relative link must succeed");

        Assert::IsTrue(env.FileExists(env.Upper(), L"target\\new.txt"),
            L"The create must put new.txt in the upper's target");
        Assert::IsFalse(env.FileExists(env.Upper(), L"a\\link"),
            L"The create must not copy the relative link up");
        AssertStatus(STATUS_SUCCESS, OpenThroughMount(mount, L"target\\new.txt"),
            L"The view's target must show new.txt");
    }

    TEST_METHOD(Write_RelativeFileSymlink_ChangesItsViewTargetAndCopiesNoLink) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"data.txt", "data");
        if (!LinkCreatedOrSkipped(CreateFileSymlink, env.Lower(0) + L"\\f", L"data.txt")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        WriteThroughMount(mount, L"f", "write");

        Assert::IsFalse(env.FileExists(env.Upper(), L"f"),
            L"The write must not copy the relative file link up");
        Assert::AreEqual(std::string("write"), env.ReadFile(env.Upper(), L"data.txt"),
            L"The write must copy data.txt up and change it");
        Assert::AreEqual(std::string("write"), ReadThroughMount(mount, L"f"),
            L"The view's f must show the write");
    }

    TEST_METHOD(Open_RelativeFileSymlinkAsReparsePoint_OpensTheLinkItself) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"data.txt", "data");
        if (!LinkCreatedOrSkipped(CreateFileSymlink, env.Lower(0) + L"\\f", L"data.txt")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx;
        InternalFileInfo info{};

        const NTSTATUS status = mount.Open(L"f", FILE_READ_ATTRIBUTES, FILE_OPEN_REPARSE_POINT,
                                           kNoCallerPid, &ctx, &info);
        if (ctx) mount.Close(ctx.get());

        AssertStatus(STATUS_SUCCESS, status, L"The open of the link itself must succeed");
        Assert::AreEqual(static_cast<UINT32>(IO_REPARSE_TAG_SYMLINK), info.ReparseTag,
            L"The open must report the link, not its target");
    }

    TEST_METHOD(Delete_RelativeLowerLink_RemovesTheLinkAndKeepsTheTarget) {
        TempLayerEnvironment env(1);
        if (!RelativeLowerLinkBuiltOrSkipped(env)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Delete(L"a\\link", kNoCallerPid),
            L"The delete of the relative link must succeed");

        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"a\\link"),
            L"The view must not show the deleted link");
        AssertStatus(STATUS_SUCCESS, OpenThroughMount(mount, L"target\\foo"),
            L"The delete of the link must keep target\\foo");
    }

    TEST_METHOD(Rename_RelativeLowerLink_MovesTheLinkWithItsTargetText) {
        TempLayerEnvironment env(1);
        if (!RelativeLowerLinkBuiltOrSkipped(env)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"a\\link", L"a\\moved", kFailIfExists, kNoCallerPid),
            L"The rename of the relative link must succeed");

        Assert::AreEqual(static_cast<DWORD>(IO_REPARSE_TAG_SYMLINK),
            ReparseTagOf(env.Upper() + L"\\a\\moved"),
            L"The upper must hold a\\moved as a symbolic link");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"a\\link"),
            L"The view must not show the link at its old name");
        Assert::AreEqual(std::string("foo"), ReadThroughMount(mount, L"a\\moved\\foo"),
            L"The moved link must resolve to the view's target");
        Assert::AreEqual(std::string("foo"), ReadThroughMount(mount, L"target\\foo"),
            L"The rename of the link must keep target\\foo");
    }

    TEST_METHOD(Delete_OpenRelativeLowerLink_RemovesTheLinkAndKeepsTheTarget) {
        TempLayerEnvironment env(1);
        if (!RelativeLowerLinkBuiltOrSkipped(env)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx = OpenLinkItselfWithDeleteAccess(mount, L"a\\link");

        const NTSTATUS status = mount.Delete(ctx.get());
        mount.Close(ctx.get());

        AssertStatus(STATUS_SUCCESS, status, L"The delete of the open link must succeed");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"a\\link"),
            L"The view must not show the deleted link");
        AssertTargetFooKept(env, mount);
    }

    TEST_METHOD(Rename_OpenRelativeLowerLink_MovesTheLinkAndKeepsTheTarget) {
        TempLayerEnvironment env(1);
        if (!RelativeLowerLinkBuiltOrSkipped(env)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        std::unique_ptr<FileContext> ctx = OpenLinkItselfWithDeleteAccess(mount, L"a\\link");

        const NTSTATUS status = mount.Rename(ctx.get(), L"a\\moved", kFailIfExists, kNoCallerPid);
        mount.Close(ctx.get());

        AssertStatus(STATUS_SUCCESS, status, L"The rename of the open link must succeed");
        Assert::AreEqual(static_cast<DWORD>(IO_REPARSE_TAG_SYMLINK),
            ReparseTagOf(env.Upper() + L"\\a\\moved"),
            L"The upper must hold a\\moved as a symbolic link");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"a\\link"),
            L"The view must not show the link at its old name");
        Assert::AreEqual(std::string("foo"), ReadThroughMount(mount, L"a\\moved\\foo"),
            L"The moved link must resolve to the view's target");
        AssertTargetFooKept(env, mount);
    }

    TEST_METHOD(Rename_DirectoryHoldingRelativeLowerLink_KeepsTheRelativeTargetText) {
        TempLayerEnvironment env(1);
        if (!RelativeLowerLinkBuiltOrSkipped(env)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"b", FILE_DIRECTORY_FILE),
            L"Preconditions: the create of b must succeed");

        AssertStatus(STATUS_SUCCESS, mount.Rename(L"a", L"b\\c", kFailIfExists, kNoCallerPid),
            L"The rename of a to b\\c must succeed");

        const SymlinkTarget moved = ReadSymlinkTarget(env.Upper() + L"\\b\\c\\link");
        Assert::IsTrue(moved.relative, L"The moved link must stay a relative symbolic link");
        Assert::AreEqual(std::wstring(L"..\\target"), moved.substituteName,
            L"The moved link must keep its target text");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"b\\c\\link\\foo"),
            L"The moved link must resolve from b\\c, and the view holds no b\\target");
        AssertTargetFooKept(env, mount);
    }

    TEST_METHOD(SetReparsePoint_RelativeLinkGivenANewTarget_OpensThroughTheNewTarget) {
        TempLayerEnvironment env(1);
        if (!RelativeLowerLinkBuiltOrSkipped(env)) {
            return;
        }
        env.WriteFile(env.Lower(0), L"other\\foo", "other");
        const std::vector<BYTE> otherLink = RelativeSymlinkReparseBuffer(L"..\\other");
        const std::wstring probe = env.Root() + L"\\probe";
        fs::create_directory(probe);
        if (!ReparseBufferSetOrSkipped(probe, otherLink)) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());
        Assert::AreEqual(std::string("foo"), ReadThroughMount(mount, L"a\\link\\foo"),
            L"Preconditions: a\\link\\foo must read target's data");

        AssertStatus(STATUS_SUCCESS,
            mount.SetReparsePoint(L"a\\link", otherLink.data(), otherLink.size(), kNoCallerPid),
            L"The change of the link's target must succeed");

        Assert::AreEqual(std::string("other"), ReadThroughMount(mount, L"a\\link\\foo"),
            L"a\\link\\foo must read other's data once the link names ..\\other");
    }

    TEST_METHOD(Open_DotComponentBeforeRelativeLowerLink_OpensTheViewTarget) {
        TempLayerEnvironment env(1);
        if (!RelativeLowerLinkBuiltOrSkipped(env)) {
            return;
        }
        env.WriteFile(env.Upper(), L"target\\foo", "upper");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(std::string("upper"), ReadThroughMount(mount, L"a\\.\\link\\foo"),
            L"The link must resolve in the view, where the upper's target\\foo hides the lower's");
    }

    TEST_METHOD(Open_RelativeLinkClimbingAboveTheRoot_StaysAtTheRoot) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"target\\foo", "foo");
        env.CreateDir(env.Lower(0), L"a");
        if (!LinkCreatedOrSkipped(CreateDirectorySymlink, env.Lower(0) + L"\\a\\up",
                                  L"..\\..\\..\\target")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        Assert::AreEqual(std::string("foo"), ReadThroughMount(mount, L"a\\up\\foo"),
            L"A .. above the view root must stay at the root");
    }

    TEST_METHOD(Open_RelativeLinkLoop_FailsWithReparsePointNotResolved) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"a");
        if (!LinkCreatedOrSkipped(CreateDirectorySymlink, env.Lower(0) + L"\\a\\loop", L"loop")) {
            return;
        }
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_REPARSE_POINT_NOT_RESOLVED, OpenThroughMount(mount, L"a\\loop\\x"),
            L"A link that leads to itself must fail after the hop limit");
    }
};

}
