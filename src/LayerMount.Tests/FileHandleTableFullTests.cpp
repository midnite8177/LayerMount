#include "pch.h"
#include "TestFixture.h"

#include "MetadataStore.h"
#include "../abi/FileHandleOpen.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountTests {

TEST_CLASS(FileHandleTableFullTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(CreateFile_AtWhitedOutNameWhenTheFileTableIsFull_LeavesTheNameDeleted) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"b.txt", "lower");
        env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"b.txt"), "");
        ::LayerMount::abi::FileTable files(kOneSlot);
        TakeTheOnlySlot(files);
        const auto mountHolder = MakeMountHolder(env.MakeConfig());

        Assert::AreEqual<HRESULT>(E_OUTOFMEMORY,
            CreateThroughFileTable(files, mountHolder, L"b.txt", kNoCreateOptions),
            L"A file create on a full file handle table must fail with E_OUTOFMEMORY");
        AssertNameStaysWhitedOutInEmptyRoot(env, *mountHolder->core, L"b.txt");
        Assert::AreEqual(0u, mountHolder->childCount.load(),
            L"The failed create must not count a child handle");
    }

    TEST_METHOD(CreateDirectory_OverWhitedOutLowerDirectoryWhenTheFileTableIsFull_LeavesNoOpaqueDirectory) {
        ForEachMetadataStore([](UINT32 capabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"d\\inner.txt", "lower");
            env.WriteFile(env.Upper(), WhiteoutMarkerPath(L"d"), "");
            auto config = env.MakeConfig();
            config.hostCapabilities = capabilities;
            ::LayerMount::abi::FileTable files(kOneSlot);
            TakeTheOnlySlot(files);
            const auto mountHolder = MakeMountHolder(config);

            Assert::AreEqual<HRESULT>(E_OUTOFMEMORY,
                CreateThroughFileTable(files, mountHolder, L"d", FILE_DIRECTORY_FILE),
                L"A directory create on a full file handle table must fail with E_OUTOFMEMORY");
            AssertNameStaysWhitedOutInEmptyRoot(env, *mountHolder->core, L"d");
            Assert::IsFalse(::LayerMount::MetadataStore::HasOpaqueMetadata(env.Upper() + L"\\d", &config),
                L"The failed create must leave no opaque metadata marker for d");
            AssertStatus(STATUS_OBJECT_NAME_NOT_FOUND,
                OpenThroughMount(*mountHolder->core, L"d\\inner.txt"),
                L"The mount must still hide the deleted lower directory's child");
        });
    }

    TEST_METHOD(OpenForWrite_LowerFileWhenTheFileTableIsFull_MakesNoCopyUp) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        ::LayerMount::abi::FileTable files(kOneSlot);
        TakeTheOnlySlot(files);
        const auto mountHolder = MakeMountHolder(env.MakeConfig());

        std::uint64_t encoded = 0;
        Assert::AreEqual<HRESULT>(E_OUTOFMEMORY,
            OpenForWriteThroughFileTable(files, mountHolder, L"f.txt", &encoded),
            L"An open for write on a full file handle table must fail with E_OUTOFMEMORY");
        Assert::IsFalse(env.FileExists(env.Upper(), L"f.txt"),
            L"The failed open must make no copy-up in the upper");
        Assert::AreEqual(std::string("lower"), ReadThroughMount(*mountHolder->core, L"f.txt"),
            L"The mount must still show the lower file");
        Assert::AreEqual(0u, mountHolder->childCount.load(),
            L"The failed open must not count a child handle");
    }

    TEST_METHOD(OpenForWrite_LowerFileWithAFreeSlot_CopiesUpAndInstallsTheHandle) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        ::LayerMount::abi::FileTable files(kOneSlot);
        const auto mountHolder = MakeMountHolder(env.MakeConfig());

        std::uint64_t encoded = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenForWriteThroughFileTable(files, mountHolder, L"f.txt", &encoded),
            L"An open for write with a free slot must succeed");
        const InstalledFileHandle closeAtEnd(files, encoded);
        Assert::IsTrue(env.FileExists(env.Upper(), L"f.txt"),
            L"The open for write must copy the lower file up");
        const auto installed = files.Resolve(encoded);
        Assert::IsTrue(installed != nullptr && installed->ctx != nullptr,
            L"The returned handle must resolve to the open file");
        Assert::AreEqual(1u, mountHolder->childCount.load(),
            L"The open must count one child handle");
    }

    TEST_METHOD(Reserve_SlotUntilInstalled_DoesNotResolveOrFree) {
        ::LayerMount::abi::FileTable files(kOneSlot);
        const std::uint64_t reserved = files.Reserve();
        Assert::AreNotEqual(std::uint64_t{0}, reserved,
            L"The reservation of the only slot must succeed");

        Assert::IsTrue(files.Resolve(reserved) == nullptr,
            L"A reserved slot must not resolve");
        Assert::IsTrue(files.Free(reserved) == nullptr,
            L"A free of a reserved slot must do nothing");
        Assert::AreEqual(std::uint64_t{0}, files.Allocate(std::make_shared<::LayerMount::abi::FileHolder>()),
            L"A reserved slot must stay taken");

        Assert::IsTrue(files.Install(reserved, std::make_shared<::LayerMount::abi::FileHolder>()),
            L"The install of the reservation must succeed");
        Assert::IsTrue(files.Resolve(reserved) != nullptr,
            L"An installed slot must resolve");
    }

    TEST_METHOD(Release_ReservedSlot_FreesItForTheNextAllocation) {
        ::LayerMount::abi::FileTable files(kOneSlot);
        const std::uint64_t reserved = files.Reserve();
        files.Release(reserved);

        Assert::IsFalse(files.Install(reserved, std::make_shared<::LayerMount::abi::FileHolder>()),
            L"A released reservation must not install");
        Assert::AreNotEqual(std::uint64_t{0}, files.Allocate(std::make_shared<::LayerMount::abi::FileHolder>()),
            L"The released slot must be free again");
    }

private:
    static constexpr std::uint32_t kOneSlot = 1;

    // Frees the handle, closes its file context, and uncounts it on the
    // mount, as LayerMountCloseFile does.
    class InstalledFileHandle {
    public:
        InstalledFileHandle(::LayerMount::abi::FileTable& files, std::uint64_t encoded)
            : files_(files), encoded_(encoded) {}
        InstalledFileHandle(const InstalledFileHandle&) = delete;
        InstalledFileHandle& operator=(const InstalledFileHandle&) = delete;
        ~InstalledFileHandle() {
            const auto holder = files_.Free(encoded_);
            if (holder == nullptr) {
                return;
            }
            if (holder->ctx != nullptr) {
                holder->mount->Close(holder->ctx.get());
            }
            holder->parentOwner->childCount.fetch_sub(1);
        }

    private:
        ::LayerMount::abi::FileTable& files_;
        const std::uint64_t encoded_;
    };

    static void TakeTheOnlySlot(::LayerMount::abi::FileTable& files) {
        Assert::AreNotEqual(std::uint64_t{0},
            files.Allocate(std::make_shared<::LayerMount::abi::FileHolder>()),
            L"The test must take the table's only slot");
    }

    static std::shared_ptr<::LayerMount::abi::LayerMountHolder> MakeMountHolder(
        const ::LayerMount::LayerConfig& config) {
        auto holder = std::make_shared<::LayerMount::abi::LayerMountHolder>();
        holder->core = std::make_unique<::LayerMount::LayerMount>(config);
        return holder;
    }

    static HRESULT CreateThroughFileTable(
        ::LayerMount::abi::FileTable& files,
        const std::shared_ptr<::LayerMount::abi::LayerMountHolder>& mountHolder,
        const std::wstring& path,
        UINT32 createOptions) {
        std::uint64_t encoded = 0;
        ::LayerMount::InternalFileInfo info{};
        return ::LayerMount::abi::CreateFileHandle(
            files, mountHolder, MakeFullAccessCreateRequest(path, createOptions), &encoded, &info);
    }

    static HRESULT OpenForWriteThroughFileTable(
        ::LayerMount::abi::FileTable& files,
        const std::shared_ptr<::LayerMount::abi::LayerMountHolder>& mountHolder,
        const std::wstring& path,
        std::uint64_t* outEncoded) {
        ::LayerMount::abi::OpenRequest request{};
        request.relativePath = path;
        request.grantedAccess = FILE_READ_DATA | FILE_WRITE_DATA;
        request.createOptions = kNoCreateOptions;
        request.callerPid = kNoCallerPid;
        ::LayerMount::InternalFileInfo info{};
        return ::LayerMount::abi::OpenFileHandle(files, mountHolder, request, outEncoded, &info);
    }
};

}
