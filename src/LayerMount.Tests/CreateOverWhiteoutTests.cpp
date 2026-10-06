#include "pch.h"
#include "TestFixture.h"

#include "MetadataStore.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMount {

class ScopedHandleInfoQuery {
public:
    ScopedHandleInfoQuery(LayerMount& mount, LayerMount::HandleInfoQuery query)
        : mount_(mount), saved_(mount.handleInfoQuery_) {
        mount_.handleInfoQuery_ = query;
    }
    ~ScopedHandleInfoQuery() { mount_.handleInfoQuery_ = saved_; }
    ScopedHandleInfoQuery(const ScopedHandleInfoQuery&) = delete;
    ScopedHandleInfoQuery& operator=(const ScopedHandleInfoQuery&) = delete;

private:
    LayerMount& mount_;
    LayerMount::HandleInfoQuery saved_;
};

}

namespace LayerMountTests {

TEST_CLASS(CreateOverWhiteoutTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(CreateFile_OverWhitedOutLowerDirectory_IsEmptyUpperFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\inner.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d", kNoCreateOptions),
            L"A file create-new over a whited-out lower directory must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"d"),
            L"The create must write the new file in the upper");
        Assert::IsFalse(HasAttribute(env.Upper() + L"\\d", FILE_ATTRIBUTE_DIRECTORY),
            L"The create must write a file, not a directory");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"d")),
            L"The create must remove the whiteout");
        Assert::AreEqual(UINT64{0}, FileSizeThroughMount(mount, L"d"),
            L"The mount must show the new empty file");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\inner.txt"),
            L"The new file must hide the children of the deleted lower directory");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"d\\inner.txt"),
            L"The create must leave the lower directory's child as it was");
    }

    TEST_METHOD(FileCreatedOverWhitedOutLowerDirectory_HidesChildrenFromDeleteRenameAndListing) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\inner.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d", kNoCreateOptions),
            L"A file create-new over a whited-out lower directory must succeed");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, mount.Delete(L"d\\inner.txt", kNoCallerPid),
            L"A delete must not find a child of the deleted lower directory");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND,
            mount.Rename(L"d\\inner.txt", L"moved.txt", kFailIfExists, kNoCallerPid),
            L"A rename must not find a child of the deleted lower directory");
        Assert::IsTrue(mount.MergeDirectoryEntries(L"d").entries.empty(),
            L"A listing at the new file must show no child of the deleted lower directory");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"d\\inner.txt"),
            L"The delete and the rename must leave the lower child as it was");
    }

    TEST_METHOD(Delete_FileCreatedOverWhitedOutLowerDirectory_WritesWhiteoutBack) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"d\\inner.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"d"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d", kNoCreateOptions),
            L"A file create-new over a whited-out lower directory must succeed");
        AssertStatus(STATUS_SUCCESS, mount.Delete(L"d", kNoCallerPid),
            L"The delete of the new upper file must succeed");
        Assert::IsFalse(env.FileExists(env.Upper(), L"d"),
            L"The delete must remove the upper file");
        Assert::IsTrue(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"d")),
            L"The delete must write a whiteout, because the lower still holds the directory");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d"),
            L"After the delete the mount must hide the lower directory");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\inner.txt"),
            L"After the delete the mount must hide the lower directory's child");
    }

    TEST_METHOD(CreateDirectory_OverWhitedOutLowerFile_IsNotOpaque) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"f"), "");
        const auto config = env.MakeConfig();
        ::LayerMount::LayerMount mount(config);

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f", FILE_DIRECTORY_FILE),
            L"A directory create-new over a whited-out lower file must succeed");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\f", FILE_ATTRIBUTE_DIRECTORY),
            L"The create must write the new directory in the upper");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"f")),
            L"The create must remove the whiteout");
        Assert::IsFalse(env.FileExists(env.Upper(), OpaqueMarkerPath(L"f")),
            L"A directory over a lower file has no lower children to hide, so it must not be opaque");
        Assert::IsFalse(::LayerMount::MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\f", &config),
            L"The new directory must have no opaque metadata marker");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"f"),
            L"The create must leave the lower file as it was");
    }

    TEST_METHOD(CreateFile_OverWhitedOutLowerFileInNestedDirectory_IsEmptyUpperFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"a\\b\\c.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"a\\b\\c.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"a\\b\\c.txt", kNoCreateOptions),
            L"A create-new over a whited-out lower file in a nested directory must succeed");
        Assert::IsTrue(env.FileExists(env.Upper(), L"a\\b\\c.txt"),
            L"The create must write the new file in the upper");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"a\\b\\c.txt")),
            L"The create must remove the whiteout");
        Assert::AreEqual(UINT64{0}, FileSizeThroughMount(mount, L"a\\b\\c.txt"),
            L"The mount must show the new empty file, not the lower data");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"a\\b\\c.txt"),
            L"The create must leave the lower file as it was");
    }

    TEST_METHOD(CreateDeleteCreate_OverWhitedOutLowerFile_EndsWithEmptyUpperFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"f.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f.txt", kNoCreateOptions),
            L"The first create-new over the whited-out lower file must succeed");
        AssertStatus(STATUS_SUCCESS, mount.Delete(L"f.txt", kNoCallerPid),
            L"The delete of the new upper file must succeed");
        Assert::IsFalse(env.FileExists(env.Upper(), L"f.txt"),
            L"The delete must remove the upper file");
        Assert::IsTrue(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"f.txt")),
            L"The delete must write a whiteout, because the lower still holds the file");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"f.txt"),
            L"After the delete the mount must hide the lower file");

        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f.txt", kNoCreateOptions),
            L"The second create-new over the whited-out lower file must succeed");
        Assert::IsFalse(env.FileExists(env.Upper(), WhiteoutMarkerPath(L"f.txt")),
            L"The second create must remove the whiteout");
        Assert::AreEqual(UINT64{0}, FileSizeThroughMount(mount, L"f.txt"),
            L"The mount must show the new empty file, not the lower data");
        Assert::AreEqual(std::string("lower"), env.ReadFile(env.Lower(0), L"f.txt"),
            L"Creates and deletes through the mount must leave the lower file as it was");
    }

    TEST_METHOD(Create_NameNextToWhiteoutNamedFileInLinkTarget_KeepsTheTargetFile) {
        for (const ::LayerMount::LayerSource linkSource : kLinkLayerSources) {
            for (const LinkCreator createLink : kDirectoryLinkCreators) {
                TempLayerEnvironment env(1);
                if (!LinkToWhiteoutNamedFileCreatedOrSkipped(env, linkSource, createLink)) {
                    continue;
                }
                ::LayerMount::LayerMount mount(env.MakeConfig());

                AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"link\\foo", kNoCreateOptions),
                    L"A create of link\\foo must succeed");
                Assert::IsTrue(env.FileExists(env.Root(), L"target\\foo"),
                    L"The create must write foo in the link target");
                Assert::AreEqual(std::string("target"), env.ReadFile(env.Root(), WhiteoutMarkerPath(L"target\\foo")),
                    L"The create must keep the .wh.foo file in the link target");
            }
        }
    }

    TEST_METHOD(Create_WhiteoutNamedFileUnderUpperLink_WritesAnOrdinaryFileInTheTarget) {
        for (const LinkCreator createLink : kDirectoryLinkCreators) {
            TempLayerEnvironment env(1);
            env.CreateDir(env.Root(), L"target");
            if (!LinkCreatedOrSkipped(createLink, env.Upper() + L"\\link", env.Root() + L"\\target")) {
                continue;
            }
            ::LayerMount::LayerMount mount(env.MakeConfig());

            AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"link\\.wh.new", kNoCreateOptions),
                L"A create of a .wh. name under a link must succeed");
            AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"link\\.wh.dir", FILE_DIRECTORY_FILE),
                L"A directory create of a .wh. name under a link must succeed");
            AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"link\\.wh.dir\\child", kNoCreateOptions),
                L"A create under a .wh. directory under a link must succeed");

            Assert::IsTrue(env.FileExists(env.Root(), L"target\\.wh.new"),
                L"The create must write the .wh.new file in the link target");
            Assert::IsTrue(env.FileExists(env.Root(), L"target\\.wh.dir\\child"),
                L"The creates must write the .wh.dir directory and its child in the link target");
            const ::LayerMount::MergedDirectory listed = mount.MergeDirectoryEntries(L"link");
            Assert::IsTrue(listed.entries.count(L".wh.new") == 1 && listed.entries.count(L".wh.dir") == 1,
                L"The listing of the link must show the new entries");
            Assert::IsTrue(mount.MergeDirectoryEntries(L"link\\.wh.dir").entries.count(L"child") == 1,
                L"The listing of the .wh.dir directory must show its child");
        }
    }

    TEST_METHOD(CreateFile_OverWhitedOutLowerFileWhenTheWhiteoutCannotGo_LeavesTheNameDeleted) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"b.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"b.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SHARING_VIOLATION,
            CreateWhileWhiteoutHeld(env, mount, L"b.txt", L"b.txt", kNoCreateOptions),
            L"The create must fail when the whiteout at the name cannot go");
        AssertNameStaysWhitedOutInEmptyRoot(env, mount, L"b.txt");
        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"b.txt", kNoCreateOptions),
            L"Once the marker is free, a create at the name must succeed");
    }

    TEST_METHOD(CreateReadOnlyFile_OverWhitedOutLowerFileWhenTheWhiteoutCannotGo_LeavesTheNameDeleted) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"b.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"b.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        ::LayerMount::LayerMount::CreateRequest request{};
        request.relativePath = L"b.txt";
        request.createOptions = kNoCreateOptions;
        request.grantedAccess = FILE_ALL_ACCESS;
        request.fileAttributes = FILE_ATTRIBUTE_READONLY;
        request.securityDescriptor = kDefaultSecurity;
        request.allocationSize = kNoAllocationSize;
        request.callerPid = kNoCallerPid;
        std::unique_ptr<::LayerMount::FileContext> ctx;
        ::LayerMount::InternalFileInfo info{};
        NTSTATUS status = STATUS_SUCCESS;
        {
            const ::LayerMount::ScopedHandle heldMarker =
                HoldOpen(env.Upper() + L"\\" + WhiteoutMarkerPath(L"b.txt"),
                         FILE_SHARE_READ | FILE_SHARE_WRITE);
            status = mount.Create(request, &ctx, &info);
        }

        AssertStatus(STATUS_SHARING_VIOLATION, status,
            L"The create of a read-only file must fail when the whiteout at the name cannot go");
        Assert::IsFalse(static_cast<bool>(ctx), L"The failed create must return no file context");
        AssertNameStaysWhitedOutInEmptyRoot(env, mount, L"b.txt");
    }

    TEST_METHOD(CreateFile_OverWhitedOutLowerFileWhenTheWhiteoutCannotGo_HidesTheNameInAnOverlayStackedOnTheUpper) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"b.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"b.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SHARING_VIOLATION,
            CreateWhileWhiteoutHeld(env, mount, L"b.txt", L"b.txt", kNoCreateOptions),
            L"The create must fail when the whiteout at the name cannot go");

        TempLayerEnvironment stacked(0);
        auto stackedConfig = stacked.MakeConfig();
        stackedConfig.lowerPaths = {env.Upper(), env.Lower(0)};
        ::LayerMount::LayerMount stackedMount(stackedConfig);
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(stackedMount, L"b.txt"),
            L"An overlay with the upper as its first lower must find nothing at the name");
    }

    TEST_METHOD(CreateDirectory_OverWhitedOutLowerDirectoryWhenTheWhiteoutCannotGo_LeavesNoOpaqueDirectory) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\inner.txt", "lower");
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"d"), "");
            auto config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_SHARING_VIOLATION,
                CreateWhileWhiteoutHeld(env, mount, L"d", L"d", FILE_DIRECTORY_FILE),
                L"The create must fail when the whiteout at the name cannot go");
            AssertNameStaysWhitedOutInEmptyRoot(env, mount, L"d");
            Assert::IsFalse(::LayerMount::MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\d", &config),
                L"The failed create must leave no opaque metadata marker for d");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\inner.txt"),
                L"The mount must still hide the deleted lower directory's child");
            AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"d", FILE_DIRECTORY_FILE),
                L"Once the marker is free, a directory create at the name must succeed");
        });
    }

    TEST_METHOD(CreateDirectory_OverWhitedOutLowerFileWhenTheWhiteoutCannotGo_LeavesTheNameDeleted) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"f"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SHARING_VIOLATION,
            CreateWhileWhiteoutHeld(env, mount, L"f", L"f", FILE_DIRECTORY_FILE),
            L"The create must fail when the whiteout at the name cannot go");
        AssertNameStaysWhitedOutInEmptyRoot(env, mount, L"f");
        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"f", FILE_DIRECTORY_FILE),
            L"Once the marker is free, a directory create at the name must succeed");
    }

    TEST_METHOD(CreateStream_AtWhitedOutNameWhenTheWhiteoutCannotGo_LeavesNoHostFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"b.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"b.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_SHARING_VIOLATION,
            CreateWhileWhiteoutHeld(env, mount, L"b.txt:s", L"b.txt", kNoCreateOptions),
            L"The stream create must fail when the whiteout at the host name cannot go");
        AssertNameStaysWhitedOutInEmptyRoot(env, mount, L"b.txt");
        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"b.txt:s", kNoCreateOptions),
            L"Once the marker is free, a stream create at the name must succeed");
    }

    TEST_METHOD(CreateFile_AtWhitedOutNameWhenTheHandleQueryFails_LeavesTheNameDeleted) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"b.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"b.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_ACCESS_DENIED,
            CreateWhileHandleQueryFails(mount, L"b.txt", kNoCreateOptions),
            L"The create must fail with the error of the handle query");
        AssertNameStaysWhitedOutInEmptyRoot(env, mount, L"b.txt");
        AssertStatus(STATUS_SUCCESS, CreateThroughMount(mount, L"b.txt", kNoCreateOptions),
            L"Once the query works, a create at the name must succeed");
    }

    TEST_METHOD(CreateFile_AtNewNameWhenTheHandleQueryFails_LeavesNoEntry) {
        TempLayerEnvironment env(1);
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_ACCESS_DENIED,
            CreateWhileHandleQueryFails(mount, L"new.txt", kNoCreateOptions),
            L"The create must fail with the error of the handle query");
        Assert::IsFalse(env.FileExists(env.Upper(), L"new.txt"),
            L"The failed create must leave no upper entry at the name");
        AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"new.txt"),
            L"The mount must not show the name after the failed create");
        Assert::IsTrue(mount.MergeDirectoryEntries(L"").entries.empty(),
            L"The root must stay empty after the failed create");
    }

    TEST_METHOD(CreateDirectory_OverWhitedOutLowerDirectoryWhenTheHandleQueryFails_LeavesNoOpaqueDirectory) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\inner.txt", "lower");
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"d"), "");
            auto config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::LayerMount mount(config);

            AssertStatus(STATUS_ACCESS_DENIED,
                CreateWhileHandleQueryFails(mount, L"d", FILE_DIRECTORY_FILE),
                L"The directory create must fail with the error of the handle query");
            AssertNameStaysWhitedOutInEmptyRoot(env, mount, L"d");
            Assert::IsFalse(::LayerMount::MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\d", &config),
                L"The failed create must leave no opaque metadata marker for d");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND, OpenThroughMount(mount, L"d\\inner.txt"),
                L"The mount must still hide the deleted lower directory's child");
        });
    }

    TEST_METHOD(CreateStream_AtWhitedOutNameWhenTheHandleQueryFails_LeavesNoHostFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"b.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"b.txt"), "");
        ::LayerMount::LayerMount mount(env.MakeConfig());

        AssertStatus(STATUS_ACCESS_DENIED,
            CreateWhileHandleQueryFails(mount, L"b.txt:s", kNoCreateOptions),
            L"The stream create must fail with the error of the handle query");
        AssertNameStaysWhitedOutInEmptyRoot(env, mount, L"b.txt");
    }

private:
    static BOOL WINAPI FailingHandleInfoQuery(HANDLE, LPBY_HANDLE_FILE_INFORMATION) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    static NTSTATUS CreateWhileHandleQueryFails(::LayerMount::LayerMount& mount,
                                                const std::wstring& name,
                                                UINT32 createOptions) {
        const ::LayerMount::ScopedHandleInfoQuery failingQuery(mount, &FailingHandleInfoQuery);
        return CreateThroughMount(mount, name, createOptions);
    }

    // Creates name through the mount while a handle without FILE_SHARE_DELETE
    // holds the upper whiteout marker of whitedOutName open, and returns the
    // create's status.
    static NTSTATUS CreateWhileWhiteoutHeld(const TempLayerEnvironment& env,
                                            ::LayerMount::LayerMount& mount,
                                            const std::wstring& name,
                                            const std::wstring& whitedOutName,
                                            UINT32 createOptions) {
        const ::LayerMount::ScopedHandle heldMarker =
            HoldOpen(env.Upper() + L"\\" + WhiteoutMarkerPath(whitedOutName),
                     FILE_SHARE_READ | FILE_SHARE_WRITE);
        return CreateThroughMount(mount, name, createOptions);
    }
};

}
