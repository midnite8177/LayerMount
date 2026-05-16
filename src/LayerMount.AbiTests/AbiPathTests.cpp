#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// Path/volume primitives: ResolvePath two-call pattern, volume label
// readback, EnsureInUpperLayer copy-up smoke.
TEST_CLASS(AbiPathTests) {
public:
    TEST_METHOD(ResolvePath_LowerLayerFile_ResolvesToLower) {
        TempLayerEnv  env(1);
        env.WriteLowerFile(0, L"data.bin", "xyz");
        LayerMountHolder mount = CreateLayerMount(env);

        std::vector<wchar_t> buf(MAX_PATH);
        LM_RESOLVED_PATH rp{};
        rp.absolutePath      = buf.data();
        rp.absolutePathChars = buf.size();

        HRESULT hr = ::LayerMountResolvePath(mount.Get(), L"\\data.bin", &rp);
        Assert::AreEqual<HRESULT>(S_OK, hr);
        Assert::AreEqual<int>(LM_LAYER_LOWER, static_cast<int>(rp.source));
        Assert::AreEqual<INT32>(0, rp.lowerIndex);
    }

    TEST_METHOD(ResolvePath_ShortBuffer_ReturnsMoreDataAndRequired) {
        TempLayerEnv  env(1);
        env.WriteLowerFile(0, L"long_name_file.dat", "x");
        LayerMountHolder mount = CreateLayerMount(env);

        wchar_t tiny[4] = {};
        LM_RESOLVED_PATH rp{};
        rp.absolutePath      = tiny;
        rp.absolutePathChars = 4;

        HRESULT hr = ::LayerMountResolvePath(mount.Get(),
                                          L"\\long_name_file.dat", &rp);
        Assert::AreEqual<HRESULT>(
            HRESULT_FROM_WIN32(ERROR_MORE_DATA), hr,
            L"A short absolutePath buffer must surface ERROR_MORE_DATA");
        Assert::IsTrue(rp.absolutePathRequired > 4,
            L"required chars must exceed the 4-char tiny buffer");
    }

    TEST_METHOD(GetVolumeInfo_ReturnslayermountLabel) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_VOLUME_INFO vi{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetVolumeInfo(mount.Get(), &vi));

        // volumeLabelLength is in BYTES; convert to chars.
        const UINT32 labelChars = vi.volumeLabelLength / sizeof(WCHAR);
        Assert::IsTrue(labelChars > 0 && labelChars < 32,
            L"volume label should be a sensible short string");
        std::wstring label(vi.volumeLabel, labelChars);
        Assert::AreEqual<std::wstring>(L"LayerMount", label);
    }

    TEST_METHOD(EnsureInUpperLayer_LowerFile_PromotesToUpper) {
        TempLayerEnv  env(1);
        env.WriteLowerFile(0, L"promote.txt", "lower contents");
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\promote.txt"));

        // Upper must now contain the physical file.
        const std::wstring upperPath = env.Upper() + L"\\promote.txt";
        DWORD attrs = ::GetFileAttributesW(upperPath.c_str());
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES, attrs,
            L"After EnsureInUpperLayer, file must exist in the upper layer");
    }
};

} // namespace LayerMountAbiTests
