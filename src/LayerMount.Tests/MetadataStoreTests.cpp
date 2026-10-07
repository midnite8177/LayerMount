// Unit tests for MetadataStore. Test the class in isolation — do NOT mount
// an overlay, do NOT invoke host-adapter callbacks. Requires NTFS for
// ADS support.

#include "pch.h"
#include "TestFixture.h"

#include "MetadataStore.h"
#include "SidecarMetadata.h"
#include "StreamTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::HasOverlayStream;

namespace LayerMountTests {

// Helper: create a real file under a unique temp directory. Returns the full path.
// Lifetime of the enclosing env is managed by the test method.
static std::wstring CreateTestFile(const TempLayerEnvironment& env,
                                    const std::wstring& name,
                                    const std::string& content = "x") {
    env.WriteFile(env.Upper(), name, content);
    return env.Upper() + L"\\" + name;
}

static std::wstring PaddedNameIn(const std::wstring& directory, size_t pathLength) {
    Assert::IsTrue(directory.size() + 2 <= pathLength,
        L"Precondition: the directory leaves room for the padded name");
    return std::wstring(pathLength - directory.size() - 1, L'p');
}

static void AssertAdsRecordStaysOnTheLink(LinkCreator createLink, LinkTarget targetKind) {
    TempLayerEnvironment env(0);
    const std::wstring targetPath = LinkTargetPath(env, targetKind);
    const std::wstring link = env.Upper() + L"\\link";
    if (!LinkToTargetCreatedOrSkipped(env, createLink, targetKind, link)) {
        return;
    }
    LayerMountMetadata written;
    written.hasStableIndexNumber = true;
    written.stableIndexNumber = 0x0102030405060708ull;

    Assert::IsTrue(MetadataStore::WriteLayerMountMetadata(link, written, nullptr),
        L"The ADS write onto the link must succeed");

    const LayerMountMetadata read = MetadataStore::ReadLayerMountMetadata(link, nullptr);
    Assert::IsTrue(read.hasStableIndexNumber, L"The link must read back its own record");
    Assert::AreEqual(written.stableIndexNumber, read.stableIndexNumber,
        L"The link must read back the stable ID that the write stored");
    Assert::IsFalse(HasOverlayStream(targetPath),
        L"The write must put no :overlay stream on the link target");
}

TEST_CLASS(MetadataStoreTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(ReadLayerMountMetadata_NoStream_ReturnsDefaults) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"empty.txt");

        LayerMountMetadata md = MetadataStore::ReadLayerMountMetadata(path, nullptr);
        Assert::IsFalse(md.opaque);
        Assert::IsFalse(md.metacopy);
        Assert::IsTrue(md.redirect.empty());
        Assert::IsTrue(md.originLayer.empty());
        // copyUpTimestamp defaults to zero FILETIME
        Assert::AreEqual(DWORD{0}, md.copyUpTimestamp.dwLowDateTime);
        Assert::AreEqual(DWORD{0}, md.copyUpTimestamp.dwHighDateTime);
    }

    TEST_METHOD(ReadLayerMountMetadata_KeepsLastAccessTime) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"atime.txt");

        LayerMountMetadata input;
        input.metacopy = true;
        Assert::IsTrue(MetadataStore::WriteLayerMountMetadata(path, input, nullptr));

        FILETIME creation{}, access{}, write{};
        GetTimes(path, &creation, &access, &write);
        const FILETIME setAccess = MakeFileTime(2019, 6, 15);
        StampTimes(path, creation, setAccess, write);
        GetTimes(path, &creation, &access, &write);
        Assert::IsTrue(FileTimesEqual(setAccess, access),
                       L"the stamp put the access time in place before the read");

        LayerMountMetadata output = MetadataStore::ReadLayerMountMetadata(path, nullptr);
        Assert::IsTrue(output.metacopy);

        GetTimes(path, &creation, &access, &write);
        Assert::IsTrue(FileTimesEqual(setAccess, access),
                       L"the metadata read kept the set access time");
    }

    TEST_METHOD(WriteThenRead_AllFields_RoundTrip) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"round.txt");

        LayerMountMetadata input;
        input.opaque = true;
        input.metacopy = true;
        input.redirect = L"other\\path.txt";
        input.copyUpTimestamp.dwLowDateTime = 0xDEADBEEF;
        input.copyUpTimestamp.dwHighDateTime = 0x12345678;
        input.originLayer = L"C:\\lower\\layer";
        input.hasStableIndexNumber = true;
        input.stableIndexNumber = 0x1122334455667788ull;

        Assert::IsTrue(MetadataStore::WriteLayerMountMetadata(path, input, nullptr));

        LayerMountMetadata output = MetadataStore::ReadLayerMountMetadata(path, nullptr);
        Assert::AreEqual(input.opaque, output.opaque);
        Assert::AreEqual(input.metacopy, output.metacopy);
        Assert::AreEqual(input.redirect, output.redirect);
        Assert::AreEqual(input.copyUpTimestamp.dwLowDateTime, output.copyUpTimestamp.dwLowDateTime);
        Assert::AreEqual(input.copyUpTimestamp.dwHighDateTime, output.copyUpTimestamp.dwHighDateTime);
        Assert::AreEqual(input.originLayer, output.originLayer);
        Assert::AreEqual(input.hasStableIndexNumber, output.hasStableIndexNumber);
        Assert::AreEqual(input.stableIndexNumber, output.stableIndexNumber);
    }

    TEST_METHOD(WriteLayerMountMetadata_OverwritesPrevious) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"ow.txt");

        LayerMountMetadata a;
        a.originLayer = L"first";
        MetadataStore::WriteLayerMountMetadata(path, a, nullptr);

        LayerMountMetadata b;
        b.originLayer = L"second";
        MetadataStore::WriteLayerMountMetadata(path, b, nullptr);

        LayerMountMetadata got = MetadataStore::ReadLayerMountMetadata(path, nullptr);
        Assert::AreEqual(std::wstring(L"second"), got.originLayer);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsOpaqueBool) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"op.txt");

        LayerMountMetadata md;
        md.opaque = true;
        MetadataStore::WriteLayerMountMetadata(path, md, nullptr);

        Assert::IsTrue(MetadataStore::ReadLayerMountMetadata(path, nullptr).opaque);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsMetacopyBool) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"mc.txt");

        LayerMountMetadata md;
        md.metacopy = true;
        MetadataStore::WriteLayerMountMetadata(path, md, nullptr);

        Assert::IsTrue(MetadataStore::ReadLayerMountMetadata(path, nullptr).metacopy);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsRedirectWide) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"rd.txt");

        LayerMountMetadata md;
        // Unicode content (Greek alpha, Chinese character)
        md.redirect = L"\u03b1\\\u4e2d\\target.txt";
        MetadataStore::WriteLayerMountMetadata(path, md, nullptr);

        LayerMountMetadata got = MetadataStore::ReadLayerMountMetadata(path, nullptr);
        Assert::AreEqual(md.redirect, got.redirect);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsCopyUpTimestamp) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"ts.txt");

        LayerMountMetadata md;
        md.copyUpTimestamp.dwLowDateTime = 0xCAFEBABE;
        md.copyUpTimestamp.dwHighDateTime = 0x87654321;
        MetadataStore::WriteLayerMountMetadata(path, md, nullptr);

        LayerMountMetadata got = MetadataStore::ReadLayerMountMetadata(path, nullptr);
        Assert::AreEqual(DWORD{0xCAFEBABE}, got.copyUpTimestamp.dwLowDateTime);
        Assert::AreEqual(DWORD{0x87654321}, got.copyUpTimestamp.dwHighDateTime);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsOriginLayer) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"ol.txt");

        LayerMountMetadata md;
        md.originLayer = L"D:\\some\\lower\\layer\\path";
        MetadataStore::WriteLayerMountMetadata(path, md, nullptr);

        LayerMountMetadata got = MetadataStore::ReadLayerMountMetadata(path, nullptr);
        Assert::AreEqual(md.originLayer, got.originLayer);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsStableIndexNumber) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"id.txt");

        LayerMountMetadata md;
        md.hasStableIndexNumber = true;
        md.stableIndexNumber = 0x8877665544332211ull;
        MetadataStore::WriteLayerMountMetadata(path, md, nullptr);

        LayerMountMetadata got = MetadataStore::ReadLayerMountMetadata(path, nullptr);
        Assert::IsTrue(got.hasStableIndexNumber);
        Assert::AreEqual(md.stableIndexNumber, got.stableIndexNumber);
    }

    TEST_METHOD(RemoveLayerMountMetadata_ExistingStream_Removes) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"rm.txt");

        LayerMountMetadata md;
        md.originLayer = L"exists";
        MetadataStore::WriteLayerMountMetadata(path, md, nullptr);

        Assert::IsTrue(MetadataStore::RemoveLayerMountMetadata(path, nullptr));

        LayerMountMetadata got = MetadataStore::ReadLayerMountMetadata(path, nullptr);
        Assert::IsTrue(got.originLayer.empty(),
            L"After remove, read should return defaults");
    }

    TEST_METHOD(RemoveLayerMountMetadata_MissingStream_ReturnsTrue) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"nostream.txt");

        Assert::IsTrue(MetadataStore::RemoveLayerMountMetadata(path, nullptr),
            L"Remove should be idempotent");
    }

    TEST_METHOD(HasOpaqueMetadata_NoStream_ReturnsFalse) {
        TempLayerEnvironment env(0);
        env.CreateDir(env.Upper(), L"od");
        std::wstring dir = env.Upper() + L"\\od";

        Assert::IsFalse(MetadataStore::HasOpaqueMetadata(dir, nullptr));
    }

    TEST_METHOD(SetOpaqueMetadata_ThenHasOpaqueMetadata_ReturnsTrue) {
        TempLayerEnvironment env(0);
        env.CreateDir(env.Upper(), L"od");
        std::wstring dir = env.Upper() + L"\\od";

        Assert::IsTrue(MetadataStore::SetOpaqueMetadata(dir, nullptr));
        Assert::IsTrue(MetadataStore::HasOpaqueMetadata(dir, nullptr));
    }

    TEST_METHOD(WriteLayerMountMetadata_OnFileSymlink_KeepsTheRecordOnTheLink) {
        AssertAdsRecordStaysOnTheLink(CreateFileSymlink, LinkTarget::File);
    }

    TEST_METHOD(WriteLayerMountMetadata_OnDirectorySymlink_KeepsTheRecordOnTheLink) {
        AssertAdsRecordStaysOnTheLink(CreateDirectorySymlink, LinkTarget::Directory);
    }

    TEST_METHOD(WriteLayerMountMetadata_OnJunction_KeepsTheRecordOnTheLink) {
        AssertAdsRecordStaysOnTheLink(CreateDirectoryJunction, LinkTarget::Directory);
    }

    TEST_METHOD(SetOpaqueMetadata_OnJunction_MarksTheLinkAndNotItsTarget) {
        TempLayerEnvironment env(0);
        env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
        const std::wstring target = env.Root() + L"\\target";
        const std::wstring link = env.Upper() + L"\\link";
        if (!LinkCreatedOrSkipped(CreateDirectoryJunction, link, target)) {
            return;
        }

        Assert::IsTrue(MetadataStore::SetOpaqueMetadata(link, nullptr),
            L"The opaque write onto the junction must succeed");
        Assert::IsTrue(MetadataStore::HasOpaqueMetadata(link, nullptr),
            L"The junction must read back its own opaque marker");
        Assert::IsFalse(MetadataStore::HasOpaqueMetadata(target, nullptr),
            L"The junction target must carry no opaque marker");

        Assert::IsTrue(MetadataStore::RemoveOpaqueMetadata(link, nullptr),
            L"The opaque remove on the junction must succeed");
        Assert::IsFalse(MetadataStore::HasOpaqueMetadata(link, nullptr),
            L"The junction must carry no opaque marker after the remove");
    }

    TEST_METHOD(RemoveOpaqueMetadata_ThenHasOpaqueMetadata_ReturnsFalse) {
        TempLayerEnvironment env(0);
        env.CreateDir(env.Upper(), L"od");
        std::wstring dir = env.Upper() + L"\\od";

        MetadataStore::SetOpaqueMetadata(dir, nullptr);
        Assert::IsTrue(MetadataStore::HasOpaqueMetadata(dir, nullptr));

        Assert::IsTrue(MetadataStore::RemoveOpaqueMetadata(dir, nullptr));
        Assert::IsFalse(MetadataStore::HasOpaqueMetadata(dir, nullptr));
    }

    TEST_METHOD(WriteReadRemove_RecordStreamPathLongerThanMaxPath_RoundTrips) {
        TempLayerEnvironment env(0);
        constexpr size_t kFilePathLength = 255;
        const std::wstring path =
            CreateTestFile(env, PaddedNameIn(env.Upper(), kFilePathLength));
        Assert::IsTrue(path.size() + std::wstring_view(kLayerMountADSStream).size() > MAX_PATH,
            L"Precondition: the record stream path is longer than MAX_PATH");

        LayerMountMetadata written;
        written.originLayer = L"long";
        Assert::IsTrue(MetadataStore::WriteLayerMountMetadata(path, written, nullptr),
            L"The record write onto a long stream path must succeed");
        Assert::IsTrue(HasOverlayStream(L"\\\\?\\" + path),
            L"The record must land in the :overlay stream of the file");
        Assert::AreEqual(written.originLayer,
            MetadataStore::ReadLayerMountMetadata(path, nullptr).originLayer,
            L"The read must return the record that the write stored");

        Assert::IsTrue(MetadataStore::RemoveLayerMountMetadata(path, nullptr),
            L"The record remove on a long stream path must succeed");
        Assert::IsFalse(HasOverlayStream(L"\\\\?\\" + path),
            L"The remove must delete the :overlay stream of the file");
    }

    TEST_METHOD(SetHasRemoveOpaque_MarkerStreamPathLongerThanMaxPath_RoundTrips) {
        TempLayerEnvironment env(0);
        constexpr size_t kDirectoryPathLength = 250;
        const std::wstring dir =
            env.Upper() + L"\\" + PaddedNameIn(env.Upper(), kDirectoryPathLength);
        std::error_code ec;
        fs::create_directories(L"\\\\?\\" + dir, ec);
        Assert::IsFalse(static_cast<bool>(ec),
            L"Precondition: the padded directory must be creatable");
        Assert::IsTrue(dir.size() + std::wstring_view(kOpaqueADSStream).size() > MAX_PATH,
            L"Precondition: the opaque marker stream path is longer than MAX_PATH");

        Assert::IsTrue(MetadataStore::SetOpaqueMetadata(dir, nullptr),
            L"The opaque write onto a long stream path must succeed");
        Assert::IsTrue(MetadataStore::HasOpaqueMetadata(dir, nullptr),
            L"The directory must read back its opaque marker");

        Assert::IsTrue(MetadataStore::RemoveOpaqueMetadata(dir, nullptr),
            L"The opaque remove on a long stream path must succeed");
        Assert::IsFalse(MetadataStore::HasOpaqueMetadata(dir, nullptr),
            L"The directory must carry no opaque marker after the remove");
    }
};

TEST_CLASS(SidecarMetadataTests) {
public:
    TEST_METHOD(WriteThenRead_ExtendedFormUpperWithTrailingSeparator_RoundTrips) {
        TempLayerEnvironment env(0);
        env.WriteFile(env.Upper(), L"a.txt", "x");
        const std::wstring upperRoot = ExtendedDirWithSeparator(env.Upper());
        const std::wstring filePath = upperRoot + L"a.txt";
        LayerMountMetadata written;
        written.metacopy = true;

        Assert::IsTrue(SidecarMetadata::Write(filePath, written, upperRoot),
            L"The sidecar write under an upper that ends in a separator must succeed");
        Assert::IsTrue(SidecarMetadata::Read(filePath, upperRoot).metacopy,
            L"The sidecar read must return the metacopy flag that the write stored");
    }
};

}
