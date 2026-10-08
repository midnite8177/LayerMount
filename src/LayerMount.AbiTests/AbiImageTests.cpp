#include "pch.h"
#include "AbiTestFixture.h"
#include "ImageTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

constexpr UINT32 kPackOptionsSizeThroughAuthor = ABI_SIZE_THROUGH(LM_IMAGE_PACK_OPTIONS, author);
constexpr UINT32 kPackOptionsSizeThroughDescription = ABI_SIZE_THROUGH(LM_IMAGE_PACK_OPTIONS, description);

LM_IMAGE_PACK_OPTIONS PackOptionsWithAuthor(UINT32 structSize, const std::wstring& author) {
    LM_IMAGE_PACK_OPTIONS options{};
    options.structSize = structSize;
    options.author = author.c_str();
    return options;
}

}

TEST_CLASS(AbiImageTests) {
public:
    TEST_METHOD(PackValidateUnpack_RoundTrip_ProducesIdenticalBytes) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        const std::wstring srcDir = env.Root() + L"\\src";
        std::filesystem::create_directories(srcDir);
        {
            std::ofstream f(srcDir + L"\\a.txt", std::ios::binary);
            f << "alpha";
        }
        {
            std::ofstream f(srcDir + L"\\b.bin", std::ios::binary);
            f << "\x01\x02\x03\x04";
        }

        const std::wstring imagePath  = env.Root() + L"\\out.lmnt";
        LM_IMAGE_HANDLE   img        = nullptr;
        constexpr INT32   compressionLevel = 3;
        const LM_IMAGE_PACK_OPTIONS* const noPackOptions = nullptr;
        HRESULT hr = ::LayerMountImagePack(
            mount.Get(), srcDir.c_str(), imagePath.c_str(),
            compressionLevel, noPackOptions, &img);
        Assert::AreEqual<HRESULT>(S_OK, hr, L"ImagePack");
        Assert::IsNotNull(img);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountImageClose(img));

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageValidate(mount.Get(), imagePath.c_str()));

        const std::wstring dstDir = env.Root() + L"\\dst";
        std::filesystem::create_directories(dstDir);
        constexpr BOOL verifyChecksum = TRUE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageUnpack(mount.Get(), imagePath.c_str(),
                                 dstDir.c_str(), verifyChecksum));

        Assert::AreEqual<std::string>(
            ReadAllBytes(srcDir + L"\\a.txt"), ReadAllBytes(dstDir + L"\\a.txt"));
        Assert::AreEqual<std::string>(
            ReadAllBytes(srcDir + L"\\b.bin"), ReadAllBytes(dstDir + L"\\b.bin"));
    }

    TEST_METHOD(Pack_LiveTransientOverlay_LeavesOutTheSidecarStore) {
        TempLayerEnv env(0);
        const std::wstring workDir = env.Root() + L"\\transient";
        LayerMountHolder overlay = CreateTransient(workDir);
        WriteThroughOverlay(overlay.Get(), L"\\file.txt", "payload");

        const std::wstring imagePath = env.Root() + L"\\live.lmnt";
        LM_IMAGE_HANDLE img = nullptr;
        const LM_IMAGE_PACK_OPTIONS* const noPackOptions = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImagePack(overlay.Get(), workDir.c_str(), imagePath.c_str(),
                                  kCompressionLevel, noPackOptions, &img),
            L"A pack of a live overlay's upper succeeds");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountImageClose(img));

        const std::wstring dstDir = env.Root() + L"\\dst";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageUnpack(overlay.Get(), imagePath.c_str(), dstDir.c_str(),
                                    kVerifyChecksum));
        Assert::AreEqual<UINT64>(1, FileCountOf(overlay.Get(), imagePath),
            L"The image counts file.txt and nothing from .overlay");
        Assert::IsTrue(SortedNamesIn(dstDir) == std::vector<std::wstring>{L"file.txt"},
            L"The unpacked tree holds file.txt and no .overlay");
        Assert::AreEqual<std::string>("payload", ReadAllBytes(dstDir + L"\\file.txt"));
    }

    TEST_METHOD(PackDifferential_BetweenTwoLiveOverlays_RecordsNoSidecarStoreEntry) {
        TempLayerEnv env(0);
        const std::wstring sourceDir = env.Root() + L"\\source";
        const std::wstring baseDir = env.Root() + L"\\base";
        LayerMountHolder source = CreateTransient(sourceDir);
        LayerMountHolder base = CreateTransient(baseDir);
        WriteThroughOverlay(source.Get(), L"\\added.txt", "added");
        WriteThroughOverlay(base.Get(), L"\\removed.txt", "removed");

        const std::wstring imagePath = env.Root() + L"\\diff.lmnt";
        LM_IMAGE_HANDLE img = nullptr;
        const LM_IMAGE_PACK_OPTIONS* const noPackOptions = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImagePackDifferential(source.Get(), sourceDir.c_str(),
                                              baseDir.c_str(), imagePath.c_str(),
                                              kCompressionLevel, noPackOptions, &img),
            L"A differential pack between two live overlays' uppers succeeds");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountImageClose(img));

        const std::wstring dstDir = env.Root() + L"\\dst";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageUnpack(source.Get(), imagePath.c_str(), dstDir.c_str(),
                                    kVerifyChecksum));
        Assert::AreEqual<UINT64>(2, FileCountOf(source.Get(), imagePath),
            L"The image counts added.txt and the whiteout of removed.txt only");
        const std::vector<std::wstring> expected{L".wh.removed.txt", L"added.txt"};
        Assert::IsTrue(SortedNamesIn(dstDir) == expected,
            L"The unpacked tree holds added.txt and the whiteout of removed.txt, "
            L"with no .overlay and no whiteout of .overlay");
    }

    TEST_METHOD(Unpack_ImageWithRootSidecarEntries_WritesOnlyTheOtherEntries) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        const std::string archive =
            ArchiveDirectoryEntry(".overlay") +
            ArchiveFileEntry(".overlay/held.txt", "held") +
            ArchiveFileEntry(".OVERLAY./dotted.txt", "dot") +
            ArchiveFileEntry(".wh..overlay", "") +
            ArchiveFileEntry("keep.txt", "keep") +
            ArchiveSentinel();
        const std::wstring imagePath = env.Root() + L"\\crafted.lmnt";
        WriteImageWithZeroChecksum(imagePath,
            R"({"whiteouts":[".overlay/gone.txt",".overlay"]})", archive);

        const std::wstring dstDir = env.Root() + L"\\dst";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageUnpack(mount.Get(), imagePath.c_str(), dstDir.c_str(),
                                    kSkipChecksum));
        Assert::IsTrue(SortedNamesIn(dstDir) == std::vector<std::wstring>{L"keep.txt"},
            L"The unpack writes keep.txt, nothing at .overlay and no whiteout of .overlay");
        Assert::AreEqual<std::string>("keep", ReadAllBytes(dstDir + L"\\keep.txt"));
    }

    TEST_METHOD(Unpack_EntryWithAStreamNameOnTheSidecar_FailsAsABadPathAndWritesNoSidecar) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        const std::string archive = ArchiveFileEntry(".overlay:x", "stream") + ArchiveSentinel();
        const std::wstring imagePath = env.Root() + L"\\stream.lmnt";
        WriteImageWithZeroChecksum(imagePath, R"({"whiteouts":[]})", archive);

        const std::wstring dstDir = env.Root() + L"\\dst";
        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME),
            ::LayerMountImageUnpack(mount.Get(), imagePath.c_str(), dstDir.c_str(),
                                    kSkipChecksum),
            L"An archive path with a colon fails as a bad path");
        Assert::IsFalse(std::filesystem::exists(dstDir + L"\\.overlay"),
            L"The unpack writes nothing at .overlay");
    }

    TEST_METHOD(Unpack_EntryUnderTheShortNameOfAnExistingSidecar_WritesNothingInTheSidecar) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring dstDir = env.Root() + L"\\dst";
        const std::wstring sidecar = dstDir + L"\\.overlay";
        std::filesystem::create_directories(sidecar);
        const std::optional<std::wstring> shortName = ShortNameOf(sidecar);
        if (!shortName) {
            Logger::WriteMessage(L"[SKIP] The volume gave .overlay no short name");
            return;
        }

        const std::string archive =
            ArchiveFileEntry(WideToNarrow(*shortName) + "/x", "short") + ArchiveSentinel();
        const std::wstring imagePath = env.Root() + L"\\short.lmnt";
        WriteImageWithZeroChecksum(imagePath, R"({"whiteouts":[]})", archive);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageUnpack(mount.Get(), imagePath.c_str(), dstDir.c_str(),
                                    kSkipChecksum));
        Assert::IsTrue(SortedNamesIn(sidecar).empty(),
            L"The unpack writes nothing in .overlay through its short name");
    }

    TEST_METHOD(PackUnpack_OverlayDirectoryBelowTheRoot_RoundTrips) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring srcDir = env.Root() + L"\\src";
        std::filesystem::create_directories(srcDir + L"\\sub\\.overlay");
        { std::ofstream f(srcDir + L"\\sub\\.overlay\\data.txt", std::ios::binary); f << "nested"; }

        const std::wstring imagePath = env.Root() + L"\\nested.lmnt";
        LM_IMAGE_HANDLE img = nullptr;
        const LM_IMAGE_PACK_OPTIONS* const noPackOptions = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImagePack(mount.Get(), srcDir.c_str(), imagePath.c_str(),
                                  kCompressionLevel, noPackOptions, &img));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountImageClose(img));

        const std::wstring dstDir = env.Root() + L"\\dst";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageUnpack(mount.Get(), imagePath.c_str(), dstDir.c_str(),
                                    kVerifyChecksum));
        Assert::AreEqual<std::string>("nested",
            ReadAllBytes(dstDir + L"\\sub\\.overlay\\data.txt"),
            L"A .overlay below the root packs and unpacks as user data");
    }

    TEST_METHOD(Pack_OutputPathUnderAFile_ReportsTheFileInThePlaceOfTheParent) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring srcDir = env.Root() + L"\\src";
        WriteText(srcDir + L"\\only.txt", "only");
        const std::wstring blocker = env.Root() + L"\\blocker";
        WriteText(blocker, "a file in the place of the parent directory");
        const std::wstring imagePath = blocker + L"\\image.lmnt";
        const LM_IMAGE_PACK_OPTIONS* const noPackOptions = nullptr;

        LM_IMAGE_HANDLE img = nullptr;
        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
            ::LayerMountImagePack(mount.Get(), srcDir.c_str(), imagePath.c_str(),
                                  kCompressionLevel, noPackOptions, &img),
            L"The pack reports the file that blocks the parent directory, not an access failure");
    }

    TEST_METHOD(Pack_OptionsWithStructSizeBelowTheShippedStruct_FailsWithInvalidArgAndWritesNoImage) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring srcDir = env.Root() + L"\\src";
        WriteText(srcDir + L"\\only.txt", "only");
        const std::wstring imagePath = env.Root() + L"\\short-options.lmnt";
        const std::wstring author = L"short author";
        const LM_IMAGE_PACK_OPTIONS options =
            PackOptionsWithAuthor(kPackOptionsSizeThroughAuthor, author);

        LM_IMAGE_HANDLE img = nullptr;
        Assert::AreEqual<HRESULT>(E_INVALIDARG,
            ::LayerMountImagePack(mount.Get(), srcDir.c_str(), imagePath.c_str(),
                                  kCompressionLevel, &options, &img),
            L"The pack refuses options whose structSize stops short of description");
        Assert::IsFalse(std::filesystem::exists(imagePath),
            L"The refused pack writes no image file");
    }

    TEST_METHOD(PackDifferential_OptionsWithStructSizeBelowTheShippedStruct_FailsWithInvalidArgAndWritesNoImage) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring sourceDir = env.Root() + L"\\source";
        const std::wstring baseDir = env.Root() + L"\\base";
        WriteText(sourceDir + L"\\added.txt", "added");
        WriteText(baseDir + L"\\removed.txt", "removed");
        const std::wstring imagePath = env.Root() + L"\\short-options-diff.lmnt";
        const std::wstring author = L"short author";
        const LM_IMAGE_PACK_OPTIONS options =
            PackOptionsWithAuthor(kPackOptionsSizeThroughAuthor, author);

        LM_IMAGE_HANDLE img = nullptr;
        Assert::AreEqual<HRESULT>(E_INVALIDARG,
            ::LayerMountImagePackDifferential(mount.Get(), sourceDir.c_str(), baseDir.c_str(),
                                              imagePath.c_str(), kCompressionLevel,
                                              &options, &img),
            L"The differential pack refuses options whose structSize stops short of description");
        Assert::IsFalse(std::filesystem::exists(imagePath),
            L"The refused differential pack writes no image file");
    }

    TEST_METHOD(Pack_OptionsWithStructSizeThroughDescription_StampsTheAuthor) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring srcDir = env.Root() + L"\\src";
        WriteText(srcDir + L"\\only.txt", "only");
        const std::wstring imagePath = env.Root() + L"\\minimum-options.lmnt";
        const std::wstring author = L"minimum author";
        const LM_IMAGE_PACK_OPTIONS options =
            PackOptionsWithAuthor(kPackOptionsSizeThroughDescription, author);

        LM_IMAGE_HANDLE img = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImagePack(mount.Get(), srcDir.c_str(), imagePath.c_str(),
                                  kCompressionLevel, &options, &img),
            L"The pack accepts options whose structSize ends at description");
        wchar_t authorBuffer[64] = {};
        LM_IMAGE_METADATA metadata{};
        metadata.author = authorBuffer;
        metadata.authorChars = _countof(authorBuffer);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageGetMetadata(mount.Get(), imagePath.c_str(), &metadata),
            L"The metadata read of the packed image succeeds");
        Assert::AreEqual(author, std::wstring(authorBuffer),
            L"The metadata gives the author that the pack stamped");
    }

    TEST_METHOD(Unpack_EntryNameWithATrailingSlash_FailsAsABadPath) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::string archive = ArchiveDirectoryEntry("dir/") + ArchiveSentinel();
        const std::wstring imagePath = env.Root() + L"\\slash.lmnt";
        WriteImageWithZeroChecksum(imagePath, R"({"whiteouts":[]})", archive);
        const std::wstring dstDir = env.Root() + L"\\dst";

        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME),
            ::LayerMountImageUnpack(mount.Get(), imagePath.c_str(), dstDir.c_str(),
                                    kSkipChecksum),
            L"The unpack refuses an archive name that ends in a slash");
    }

    TEST_METHOD(Validate_OnTruncatedImage_Fails) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        const std::wstring srcDir = env.Root() + L"\\src";
        std::filesystem::create_directories(srcDir);
        { std::ofstream f(srcDir + L"\\only.txt"); f << "content"; }

        const std::wstring imagePath = env.Root() + L"\\broken.lmnt";
        LM_IMAGE_HANDLE   img       = nullptr;
        constexpr INT32   compressionLevel = 1;
        const LM_IMAGE_PACK_OPTIONS* const noPackOptions = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImagePack(mount.Get(), srcDir.c_str(),
                               imagePath.c_str(), compressionLevel, noPackOptions, &img));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountImageClose(img));

        std::error_code ec;
        std::filesystem::resize_file(imagePath, 16, ec);
        Assert::IsFalse(!!ec, L"resize_file should succeed on our own tmp file");

        HRESULT hr = ::LayerMountImageValidate(mount.Get(), imagePath.c_str());
        Assert::AreNotEqual<HRESULT>(S_OK, hr,
            L"A truncated .lmnt must fail LayerMountImageValidate");
    }

    TEST_METHOD(GetManifest_OnDestroyedMountHandle_ReturnsEHandle) {
        TempLayerEnv  env(0);
        const LM_HANDLE stale = DestroyedMountHandle(env);

        LM_IMAGE_MANIFEST manifest{};
        HRESULT hr = ::LayerMountImageGetManifest(
            stale, L"C:\\does-not-matter.lmnt", &manifest);
        Assert::AreEqual<HRESULT>(E_HANDLE, hr,
            L"A destroyed mount handle must return E_HANDLE");
    }
};

}
