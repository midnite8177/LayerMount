#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// Layer image primitives. Pack / Validate / Unpack round-trip plus an
// explicit Validate-on-truncated failure path.
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
        HRESULT hr = ::LayerMountImagePack(
            mount.Get(), srcDir.c_str(), imagePath.c_str(),
            /*compressionLevel*/ 3,
            /*options*/ nullptr,
            &img);
        Assert::AreEqual<HRESULT>(S_OK, hr, L"ImagePack");
        Assert::IsNotNull(img);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountImageClose(img));

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageValidate(mount.Get(), imagePath.c_str()));

        const std::wstring dstDir = env.Root() + L"\\dst";
        std::filesystem::create_directories(dstDir);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImageUnpack(mount.Get(), imagePath.c_str(),
                                 dstDir.c_str(), /*verifyChecksum*/ TRUE));

        // Byte-compare the two files round-tripped.
        auto readBytes = [](const std::wstring& p) {
            std::ifstream f(p, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        };
        Assert::AreEqual<std::string>(
            readBytes(srcDir + L"\\a.txt"), readBytes(dstDir + L"\\a.txt"));
        Assert::AreEqual<std::string>(
            readBytes(srcDir + L"\\b.bin"), readBytes(dstDir + L"\\b.bin"));
    }

    TEST_METHOD(Validate_OnTruncatedImage_Fails) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        const std::wstring srcDir = env.Root() + L"\\src";
        std::filesystem::create_directories(srcDir);
        { std::ofstream f(srcDir + L"\\only.txt"); f << "content"; }

        const std::wstring imagePath = env.Root() + L"\\broken.lmnt";
        LM_IMAGE_HANDLE   img       = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountImagePack(mount.Get(), srcDir.c_str(),
                               imagePath.c_str(), 1, nullptr, &img));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountImageClose(img));

        // Truncate the image on disk so the checksum fails.
        std::error_code ec;
        std::filesystem::resize_file(imagePath, 16, ec);
        Assert::IsFalse(!!ec, L"resize_file should succeed on our own tmp file");

        HRESULT hr = ::LayerMountImageValidate(mount.Get(), imagePath.c_str());
        Assert::AreNotEqual<HRESULT>(S_OK, hr,
            L"A truncated .lmnt must fail LayerMountImageValidate");
    }
};

} // namespace LayerMountAbiTests
