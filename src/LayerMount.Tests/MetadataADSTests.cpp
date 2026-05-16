// Unit tests for MetadataADS. Test the class in isolation — do NOT mount
// an overlay, do NOT invoke host-adapter callbacks. Requires NTFS for
// ADS support.

#include "pch.h"
#include "TestFixture.h"

#include "MetadataADS.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

// Helper: create a real file under a unique temp directory. Returns the full path.
// Lifetime of the enclosing env is managed by the test method.
static std::wstring CreateTestFile(const TempLayerEnvironment& env,
                                    const std::wstring& name,
                                    const std::string& content = "x") {
    env.WriteFile(env.Upper(), name, content);
    return env.Upper() + L"\\" + name;
}

TEST_CLASS(MetadataADSTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(ReadLayerMountMetadata_NoStream_ReturnsDefaults) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"empty.txt");

        LayerMountMetadata md = MetadataADS::ReadLayerMountMetadata(path);
        Assert::IsFalse(md.opaque);
        Assert::IsFalse(md.metacopy);
        Assert::IsTrue(md.redirect.empty());
        Assert::IsTrue(md.originLayer.empty());
        // copyUpTimestamp defaults to zero FILETIME
        Assert::AreEqual(DWORD{0}, md.copyUpTimestamp.dwLowDateTime);
        Assert::AreEqual(DWORD{0}, md.copyUpTimestamp.dwHighDateTime);
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

        Assert::IsTrue(MetadataADS::WriteLayerMountMetadata(path, input));

        LayerMountMetadata output = MetadataADS::ReadLayerMountMetadata(path);
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
        MetadataADS::WriteLayerMountMetadata(path, a);

        LayerMountMetadata b;
        b.originLayer = L"second";
        MetadataADS::WriteLayerMountMetadata(path, b);

        LayerMountMetadata got = MetadataADS::ReadLayerMountMetadata(path);
        Assert::AreEqual(std::wstring(L"second"), got.originLayer);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsOpaqueBool) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"op.txt");

        LayerMountMetadata md;
        md.opaque = true;
        MetadataADS::WriteLayerMountMetadata(path, md);

        Assert::IsTrue(MetadataADS::ReadLayerMountMetadata(path).opaque);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsMetacopyBool) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"mc.txt");

        LayerMountMetadata md;
        md.metacopy = true;
        MetadataADS::WriteLayerMountMetadata(path, md);

        Assert::IsTrue(MetadataADS::ReadLayerMountMetadata(path).metacopy);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsRedirectWide) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"rd.txt");

        LayerMountMetadata md;
        // Unicode content (Greek alpha, Chinese character)
        md.redirect = L"\u03b1\\\u4e2d\\target.txt";
        MetadataADS::WriteLayerMountMetadata(path, md);

        LayerMountMetadata got = MetadataADS::ReadLayerMountMetadata(path);
        Assert::AreEqual(md.redirect, got.redirect);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsCopyUpTimestamp) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"ts.txt");

        LayerMountMetadata md;
        md.copyUpTimestamp.dwLowDateTime = 0xCAFEBABE;
        md.copyUpTimestamp.dwHighDateTime = 0x87654321;
        MetadataADS::WriteLayerMountMetadata(path, md);

        LayerMountMetadata got = MetadataADS::ReadLayerMountMetadata(path);
        Assert::AreEqual(DWORD{0xCAFEBABE}, got.copyUpTimestamp.dwLowDateTime);
        Assert::AreEqual(DWORD{0x87654321}, got.copyUpTimestamp.dwHighDateTime);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsOriginLayer) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"ol.txt");

        LayerMountMetadata md;
        md.originLayer = L"D:\\some\\lower\\layer\\path";
        MetadataADS::WriteLayerMountMetadata(path, md);

        LayerMountMetadata got = MetadataADS::ReadLayerMountMetadata(path);
        Assert::AreEqual(md.originLayer, got.originLayer);
    }

    TEST_METHOD(WriteLayerMountMetadata_PersistsStableIndexNumber) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"id.txt");

        LayerMountMetadata md;
        md.hasStableIndexNumber = true;
        md.stableIndexNumber = 0x8877665544332211ull;
        MetadataADS::WriteLayerMountMetadata(path, md);

        LayerMountMetadata got = MetadataADS::ReadLayerMountMetadata(path);
        Assert::IsTrue(got.hasStableIndexNumber);
        Assert::AreEqual(md.stableIndexNumber, got.stableIndexNumber);
    }

    TEST_METHOD(RemoveLayerMountMetadata_ExistingStream_Removes) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"rm.txt");

        LayerMountMetadata md;
        md.originLayer = L"exists";
        MetadataADS::WriteLayerMountMetadata(path, md);

        Assert::IsTrue(MetadataADS::RemoveLayerMountMetadata(path));

        LayerMountMetadata got = MetadataADS::ReadLayerMountMetadata(path);
        Assert::IsTrue(got.originLayer.empty(),
            L"After remove, read should return defaults");
    }

    TEST_METHOD(RemoveLayerMountMetadata_MissingStream_ReturnsTrue) {
        TempLayerEnvironment env(0);
        std::wstring path = CreateTestFile(env, L"nostream.txt");

        Assert::IsTrue(MetadataADS::RemoveLayerMountMetadata(path),
            L"Remove should be idempotent");
    }

    // --- Opaque ADS ---

    TEST_METHOD(HasOpaqueADS_NoStream_ReturnsFalse) {
        TempLayerEnvironment env(0);
        env.CreateDir(env.Upper(), L"od");
        std::wstring dir = env.Upper() + L"\\od";

        Assert::IsFalse(MetadataADS::HasOpaqueADS(dir));
    }

    TEST_METHOD(SetOpaqueADS_ThenHasOpaqueADS_ReturnsTrue) {
        TempLayerEnvironment env(0);
        env.CreateDir(env.Upper(), L"od");
        std::wstring dir = env.Upper() + L"\\od";

        Assert::IsTrue(MetadataADS::SetOpaqueADS(dir));
        Assert::IsTrue(MetadataADS::HasOpaqueADS(dir));
    }

    TEST_METHOD(RemoveOpaqueADS_ThenHasOpaqueADS_ReturnsFalse) {
        TempLayerEnvironment env(0);
        env.CreateDir(env.Upper(), L"od");
        std::wstring dir = env.Upper() + L"\\od";

        MetadataADS::SetOpaqueADS(dir);
        Assert::IsTrue(MetadataADS::HasOpaqueADS(dir));

        Assert::IsTrue(MetadataADS::RemoveOpaqueADS(dir));
        Assert::IsFalse(MetadataADS::HasOpaqueADS(dir));
    }

    // Note: a previously-planned test `WriteLayerMountMetadata_FileDoesNotExist_ReturnsFalse`
    // was removed — NTFS semantics allow creating a base file implicitly when
    // writing to its ADS (CreateFileW on "path:stream" with CREATE_ALWAYS). That
    // behavior is acceptable for this layer; the overlay never writes metadata
    // for files it hasn't just created or opened itself.
};

} // namespace LayerMountTests
