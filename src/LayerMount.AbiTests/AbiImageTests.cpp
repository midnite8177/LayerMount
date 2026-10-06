#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

constexpr INT32 kCompressionLevel = 3;
constexpr BOOL  kVerifyChecksum   = TRUE;
constexpr BOOL  kSkipChecksum     = FALSE;

constexpr UINT64 kImageHeaderSize       = 128;
constexpr UINT32 kImageFormatVersion    = 1;
constexpr UINT32 kZstdCompressionFlags  = 1;
constexpr size_t kArchiveEntryHeaderSize = 24;
constexpr UINT8  kFileEntry             = 0;
constexpr UINT8  kDirectoryEntry        = 1;
constexpr UINT8  kNotWhiteout           = 0;
constexpr UINT64 kNoModifiedTime        = 0;

constexpr UINT32 kZstdFrameMagic = 0xFD2FB528;
constexpr UINT8  kSingleSegmentWithOneByteContentSize = 0x20;
constexpr UINT32 kLastRawBlock = 1;
constexpr int    kBlockSizeShift = 3;
constexpr size_t kBlockHeaderBytes = 3;

std::string WideToNarrow(const std::wstring& ascii) {
    std::string narrow;
    for (const wchar_t c : ascii) narrow.push_back(static_cast<char>(c));
    return narrow;
}

UINT64 FileCountOf(LM_HANDLE mount, const std::wstring& imagePath) {
    LM_IMAGE_METADATA metadata{};
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountImageGetMetadata(mount, imagePath.c_str(), &metadata),
        L"LayerMountImageGetMetadata");
    return metadata.fileCount;
}

template <typename T>
void AppendLittleEndian(std::string& out, T value) {
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.append(bytes, sizeof(T));
}

std::string ArchiveEntryHeader(const std::string& path, UINT64 size, UINT32 attributes,
                               UINT8 isDirectory) {
    std::string header;
    AppendLittleEndian<UINT16>(header, static_cast<UINT16>(path.size()));
    AppendLittleEndian<UINT64>(header, size);
    AppendLittleEndian<UINT32>(header, attributes);
    AppendLittleEndian<UINT64>(header, kNoModifiedTime);
    AppendLittleEndian<UINT8>(header, isDirectory);
    AppendLittleEndian<UINT8>(header, kNotWhiteout);
    return header;
}

std::string ArchiveDirectoryEntry(const std::string& path) {
    return ArchiveEntryHeader(path, 0, FILE_ATTRIBUTE_DIRECTORY, kDirectoryEntry) + path;
}

std::string ArchiveFileEntry(const std::string& path, const std::string& data) {
    return ArchiveEntryHeader(path, data.size(), FILE_ATTRIBUTE_NORMAL, kFileEntry) + path + data;
}

std::string ArchiveSentinel() {
    std::string sentinel;
    AppendLittleEndian<UINT16>(sentinel, 0xFFFF);
    sentinel.append(kArchiveEntryHeaderSize - sizeof(UINT16), '\0');
    return sentinel;
}

std::string RawZstdFrame(const std::string& content) {
    Assert::IsTrue(content.size() <= MAXBYTE,
        L"The frame descriptor gives the content size one byte, so the content holds at most 255 bytes");
    std::string frame;
    AppendLittleEndian<UINT32>(frame, kZstdFrameMagic);
    AppendLittleEndian<UINT8>(frame, kSingleSegmentWithOneByteContentSize);
    AppendLittleEndian<UINT8>(frame, static_cast<UINT8>(content.size()));
    const UINT32 blockHeader =
        kLastRawBlock | (static_cast<UINT32>(content.size()) << kBlockSizeShift);
    frame.append(reinterpret_cast<const char*>(&blockHeader), kBlockHeaderBytes);
    return frame + content;
}

void WriteImageWithZeroChecksum(const std::wstring& imagePath, const std::string& metadataJson,
                        const std::string& archive) {
    const std::string metadata = metadataJson + '\0';
    const std::string data = RawZstdFrame(archive);
    std::string image = std::string("OVLYIMG", 8);
    AppendLittleEndian<UINT32>(image, kImageFormatVersion);
    AppendLittleEndian<UINT32>(image, kZstdCompressionFlags);
    AppendLittleEndian<UINT64>(image, kImageHeaderSize);
    AppendLittleEndian<UINT64>(image, metadata.size());
    AppendLittleEndian<UINT64>(image, kImageHeaderSize + metadata.size());
    AppendLittleEndian<UINT64>(image, data.size());
    image.append(static_cast<size_t>(kImageHeaderSize) - image.size(), '\0');
    std::ofstream out(imagePath, std::ios::binary | std::ios::trunc);
    out << image << metadata << data;
}

}

TEST_CLASS(AbiImageTests) {
public:
    TEST_METHOD(PackValidateUnpack_RoundTrip_ProducesIdenticalBytes) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        // Seed a tiny source tree inside the temp env.
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

        // Byte-compare the two files round-tripped.
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

        // Truncate the image on disk so the checksum fails.
        std::error_code ec;
        std::filesystem::resize_file(imagePath, 16, ec);
        Assert::IsFalse(!!ec, L"resize_file should succeed on our own tmp file");

        HRESULT hr = ::LayerMountImageValidate(mount.Get(), imagePath.c_str());
        Assert::AreNotEqual<HRESULT>(S_OK, hr,
            L"A truncated .lmnt must fail LayerMountImageValidate");
    }

    TEST_METHOD(GetManifest_OnDestroyedMountHandle_ReturnsEHandle) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_HANDLE stale = mount.Get();
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountDestroy(mount.Release()));

        LM_IMAGE_MANIFEST manifest{};
        HRESULT hr = ::LayerMountImageGetManifest(
            stale, L"C:\\does-not-matter.lmnt", &manifest);
        Assert::AreEqual<HRESULT>(E_HANDLE, hr,
            L"A destroyed mount handle must return E_HANDLE");
    }
};

} // namespace LayerMountAbiTests
