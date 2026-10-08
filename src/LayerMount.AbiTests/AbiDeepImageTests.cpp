#include "pch.h"
#include "AbiTestFixture.h"
#include "DeepPathAbiHelpers.h"
#include "ImageTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using LayerMountTestShared::DeepLeafName;
using LayerMountTestShared::ExtendedFormOf;
using LayerMountTestShared::ExtendedPathUnder;
using LayerMountTestShared::MakeDeepLayerRoot;

namespace LayerMountAbiTests {

namespace {

const LM_IMAGE_PACK_OPTIONS* const kNoPackOptions = nullptr;

void Pack(LM_HANDLE mount, const std::wstring& sourceDir, const std::wstring& imagePath,
          const LM_IMAGE_PACK_OPTIONS* options) {
    LM_IMAGE_HANDLE image = nullptr;
    const HRESULT hr = ::LayerMountImagePack(mount, sourceDir.c_str(), imagePath.c_str(),
                                             kCompressionLevel, options, &image);
    Assert::AreEqual<HRESULT>(S_OK, hr,
        (L"LayerMountImagePack succeeds: " + LastErrorMessage(hr)).c_str());
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountImageClose(image));
}

void Unpack(LM_HANDLE mount, const std::wstring& imagePath, const std::wstring& targetDir,
            BOOL verifyChecksum) {
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountImageUnpack(mount, imagePath.c_str(), targetDir.c_str(), verifyChecksum),
        L"LayerMountImageUnpack succeeds");
}

void CopyKeepingTimes(const std::wstring& from, const std::wstring& to) {
    std::filesystem::create_directories(std::filesystem::path(to).parent_path());
    constexpr BOOL failIfExists = TRUE;
    Assert::IsTrue(::CopyFileW(from.c_str(), to.c_str(), failIfExists) != FALSE,
        L"CopyFileW copies the file with its last-write time");
}

}

TEST_CLASS(AbiDeepImageTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        LayerMountTestShared::AssertHostRefusesPlainPathsPastMaxPath();
    }

    TEST_METHOD(PackUnpack_TreeUnderDeepRoot_UnpacksTheSameContentIntoADeepTarget) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = MakeDeepLayerRoot(env.Root(), L"DeepSource");
        WriteText(ExtendedPathUnder(source, L"top.txt"), "top");
        WriteText(ExtendedPathUnder(source, L"sub\\inner\\nested.bin"), "\x01\x02\x03");
        const std::wstring imagePath =
            MakeDeepLayerRoot(env.Root(), L"DeepImages") + L"\\tree.lmnt";

        Pack(mount.Get(), source, imagePath, kNoPackOptions);
        const std::wstring target = MakeDeepLayerRoot(env.Root(), L"DeepTarget");
        Unpack(mount.Get(), imagePath, target, kVerifyChecksum);

        Assert::AreEqual<std::string>("top", ReadAllBytes(ExtendedPathUnder(target, L"top.txt")),
            L"The unpack writes the top-level file in the deep target");
        Assert::AreEqual<std::string>("\x01\x02\x03",
            ReadAllBytes(ExtendedPathUnder(target, L"sub\\inner\\nested.bin")),
            L"The unpack writes the nested file in the deep target");
        Assert::AreEqual<UINT64>(2, FileCountOf(mount.Get(), imagePath),
            L"The image counts the two files");
    }

    TEST_METHOD(PackUnpack_ShallowRootsWithAnEntryPastMaxPath_RoundTripsTheEntry) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = env.Root() + L"\\src";
        const std::wstring target = env.Root() + L"\\dst";
        const std::wstring deepName = DeepLeafName(source, L"DeepDir");
        WriteText(ExtendedFormOf(source + L"\\shallow.txt"), "shallow");
        WriteText(ExtendedPathUnder(source, deepName + L"\\deep.txt"), "deep");
        const std::wstring imagePath = env.Root() + L"\\shallow.lmnt";

        Pack(mount.Get(), source, imagePath, kNoPackOptions);
        Unpack(mount.Get(), imagePath, target, kVerifyChecksum);

        Assert::AreEqual<std::string>("shallow", ReadAllBytes(target + L"\\shallow.txt"),
            L"The unpack writes the shallow file");
        Assert::AreEqual<std::string>("deep",
            ReadAllBytes(ExtendedPathUnder(target, deepName + L"\\deep.txt")),
            L"The unpack writes the file whose path passes MAX_PATH under the shallow target");
    }

    TEST_METHOD(PackDifferential_DeepTreeAgainstDeepBase_RecordsTheChangesAndTheWhiteout) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring base = MakeDeepLayerRoot(env.Root(), L"DeepBase");
        const std::wstring source = MakeDeepLayerRoot(env.Root(), L"DeepSource");
        WriteText(ExtendedPathUnder(base, L"sub\\kept.txt"), "kept");
        WriteText(ExtendedPathUnder(base, L"sub\\removed.txt"), "removed");
        WriteText(ExtendedPathUnder(base, L"changed.txt"), "old");
        CopyKeepingTimes(ExtendedPathUnder(base, L"sub\\kept.txt"),
                         ExtendedPathUnder(source, L"sub\\kept.txt"));
        WriteText(ExtendedPathUnder(source, L"sub\\added.txt"), "added");
        WriteText(ExtendedPathUnder(source, L"changed.txt"), "changed");
        const std::wstring imagePath = env.Root() + L"\\diff.lmnt";

        LM_IMAGE_HANDLE image = nullptr;
        const HRESULT hr = ::LayerMountImagePackDifferential(
            mount.Get(), source.c_str(), base.c_str(), imagePath.c_str(), kCompressionLevel,
            kNoPackOptions, &image);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            (L"LayerMountImagePackDifferential succeeds: " + LastErrorMessage(hr)).c_str());
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountImageClose(image));
        const std::wstring target = MakeDeepLayerRoot(env.Root(), L"DeepTarget");
        Unpack(mount.Get(), imagePath, target, kVerifyChecksum);

        Assert::AreEqual<UINT64>(3, FileCountOf(mount.Get(), imagePath),
            L"The image counts changed.txt, sub/added.txt and the whiteout of sub/removed.txt");
        Assert::IsTrue(SortedNamesIn(ExtendedFormOf(target)) ==
                           std::vector<std::wstring>{L"changed.txt", L"sub"},
            L"The unpacked root holds changed.txt and sub");
        Assert::IsTrue(SortedNamesIn(ExtendedPathUnder(target, L"sub")) ==
                           std::vector<std::wstring>{L".wh.removed.txt", L"added.txt"},
            L"The unpacked sub holds added.txt and the whiteout of removed.txt, and no kept.txt");
        Assert::AreEqual<std::string>("changed",
            ReadAllBytes(ExtendedPathUnder(target, L"changed.txt")),
            L"The image holds the new content of changed.txt");
    }

    TEST_METHOD(ValidateGetMetadata_ImageAtDeepPath_ValidatesAndGivesItsMetadata) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = env.Root() + L"\\src";
        WriteText(source + L"\\only.txt", "only");
        const std::wstring imagePath =
            MakeDeepLayerRoot(env.Root(), L"DeepImages") + L"\\one.lmnt";
        const std::wstring author = L"deep author";
        LM_IMAGE_PACK_OPTIONS options{};
        options.structSize = sizeof(options);
        options.author = author.c_str();

        Pack(mount.Get(), source, imagePath, &options);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountImageValidate(mount.Get(), imagePath.c_str()),
            L"The image at the deep path validates");
        wchar_t authorBuffer[64] = {};
        LM_IMAGE_METADATA metadata{};
        metadata.author = authorBuffer;
        metadata.authorChars = _countof(authorBuffer);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageGetMetadata(mount.Get(), imagePath.c_str(), &metadata),
            L"The metadata read of the image at the deep path succeeds");
        Assert::AreEqual<UINT64>(1, metadata.fileCount, L"The metadata counts the one file");
        Assert::AreEqual(author, std::wstring(authorBuffer),
            L"The metadata gives the author that the pack stamped");
    }

    TEST_METHOD(CreateGetManifest_ManifestAtDeepPathListingADeepImage_GivesTheImagePathAsWritten) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = env.Root() + L"\\src";
        WriteText(source + L"\\only.txt", "only");
        const std::wstring imagePath =
            MakeDeepLayerRoot(env.Root(), L"DeepImages") + L"\\one.lmnt";
        Pack(mount.Get(), source, imagePath, kNoPackOptions);
        const std::wstring manifestPath =
            MakeDeepLayerRoot(env.Root(), L"DeepManifests") + L"\\nested\\layers.json";
        const PCWSTR imagePaths[] = {imagePath.c_str()};

        const HRESULT createHr = ::LayerMountImageCreateManifest(
            mount.Get(), manifestPath.c_str(), imagePaths, _countof(imagePaths));
        Assert::AreEqual<HRESULT>(S_OK, createHr,
            (L"LayerMountImageCreateManifest writes the manifest at the deep path: " +
             LastErrorMessage(createHr)).c_str());

        std::vector<wchar_t> pathBuffer(1024);
        LM_IMAGE_MANIFEST_ENTRY entry{};
        entry.imagePath = pathBuffer.data();
        entry.imagePathChars = pathBuffer.size();
        LM_IMAGE_MANIFEST manifest{};
        manifest.entryCount = 1;
        manifest.entries = &entry;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageGetManifest(mount.Get(), manifestPath.c_str(), &manifest),
            L"LayerMountImageGetManifest reads the manifest at the deep path");
        Assert::AreEqual<UINT32>(1, manifest.entryCount, L"The manifest lists the one image");
        Assert::AreEqual(imagePath, std::wstring(pathBuffer.data()),
            L"The manifest gives the image path as the caller wrote it");
        Assert::AreEqual<size_t>(64, std::wstring(entry.checksumHex).size(),
            L"The manifest gives the SHA-256 of the image");
    }

    TEST_METHOD(Unpack_MetadataWhiteoutUnderASubdirectory_WritesTheMarkerInADeepTarget) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::string archive = ArchiveFileEntry("sub/keep.txt", "keep") + ArchiveSentinel();
        const std::wstring imagePath = env.Root() + L"\\whiteout.lmnt";
        WriteImageWithZeroChecksum(imagePath, R"({"whiteouts":["sub/gone.txt"]})", archive);
        const std::wstring target = MakeDeepLayerRoot(env.Root(), L"DeepTarget");

        Unpack(mount.Get(), imagePath, target, kSkipChecksum);

        Assert::IsTrue(SortedNamesIn(ExtendedPathUnder(target, L"sub")) ==
                           std::vector<std::wstring>{L".wh.gone.txt", L"keep.txt"},
            L"The unpack writes keep.txt and the whiteout marker of gone.txt in the deep sub");
    }

    TEST_METHOD(Unpack_EntryUnderTheShortNameOfAnExistingSidecarInADeepTarget_WritesOnlyTheOtherEntries) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring target = MakeDeepLayerRoot(env.Root(), L"DeepTarget");
        const std::wstring sidecar = ExtendedPathUnder(target, L".overlay");
        std::filesystem::create_directories(sidecar);
        const std::optional<std::wstring> shortName = ShortNameOf(sidecar);
        if (!shortName) {
            Logger::WriteMessage(L"[SKIP] The volume gave .overlay no short name");
            return;
        }

        const std::string archive =
            ArchiveFileEntry(WideToNarrow(*shortName) + "/x", "short") +
            ArchiveFileEntry("keep.txt", "keep") +
            ArchiveSentinel();
        const std::wstring imagePath = env.Root() + L"\\short.lmnt";
        WriteImageWithZeroChecksum(imagePath, R"({"whiteouts":[]})", archive);

        Unpack(mount.Get(), imagePath, target, kSkipChecksum);

        Assert::IsTrue(SortedNamesIn(sidecar).empty(),
            L"The unpack writes nothing in .overlay through its short name");
        Assert::AreEqual<std::string>("keep", ReadAllBytes(ExtendedPathUnder(target, L"keep.txt")),
            L"The unpack writes keep.txt in the deep target");
    }
};

}
