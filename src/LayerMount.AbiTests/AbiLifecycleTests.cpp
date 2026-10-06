#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

// Imports a directory that holds an empty work directory into a VHD under
// root, attaches the VHD read-only and returns the path of that work
// directory on the VHD's volume. The attach ends when *vhd closes.
std::wstring AttachVhdWithWorkDirectory(LM_HANDLE mount, const std::wstring& root,
                                        LM_VHD_HANDLE* vhd) {
    const std::wstring source = root + L"\\vhd-source";
    const std::wstring vhdPath = root + L"\\work.vhdx";
    std::filesystem::create_directories(source + L"\\work");
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountVhdImport(mount, source.c_str(), vhdPath.c_str(), 0),
        L"LayerMountVhdImport makes the VHD");

    return AttachVhdVolumeReadOnly(mount, vhdPath, vhd) + L"work";
}

}

// Lifecycle round-trip + an engine-reachable error path.
TEST_CLASS(AbiLifecycleTests) {
public:
    TEST_METHOD(Create_HappyPath_ReturnsUsableHandle) {
        TempLayerEnv  env(1);
        LayerMountHolder mount = CreateLayerMount(env);

        // The only observable "handle is live" check against the public
        // ABI that doesn't require a populated state is GetStats.
        LM_STATS stats{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetStats(mount.Get(), &stats));
    }

    TEST_METHOD(Destroy_ReleasesHandle_SubsequentUseIsEHandle) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_HANDLE raw = mount.Release();
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountDestroy(raw));

        LM_STATS stats{};
        Assert::AreEqual<HRESULT>(E_HANDLE,
            ::LayerMountGetStats(raw, &stats));
    }

    TEST_METHOD(Destroy_NullHandle_ReturnsEHandle) {
        Assert::AreEqual<HRESULT>(E_HANDLE, ::LayerMountDestroy(nullptr));
    }

    TEST_METHOD(Create_NullLowerPathEntry_ReturnsEInvalidArg) {
        TempLayerEnv  env(0);
        ConfigBuilder b(env);

        // Force a lowerPaths array with a NULL entry.
        PCWSTR  bogusLowers[1] = { nullptr };
        LM_CONFIG cfg = *b.Ptr();
        cfg.lowerPathCount = 1;
        cfg.lowerPaths     = bogusLowers;

        LM_HANDLE h  = nullptr;
        HRESULT    hr = ::LayerMountCreate(&cfg, &h);
        Assert::AreEqual<HRESULT>(E_INVALIDARG, hr,
            L"A NULL entry in lowerPaths[] must be rejected");
        Assert::IsNull(h);
    }

    TEST_METHOD(Create_WorkDirectoryOnAnotherVolume_ReturnsEInvalidArgWithAMessage) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        LayerMountHolder vhdMount = CreateLayerMount(env);
        LM_VHD_HANDLE vhd = nullptr;
        const std::wstring workDir = AttachVhdWithWorkDirectory(vhdMount.Get(), env.Root(), &vhd);

        ConfigBuilder b(env);
        LM_CONFIG cfg = *b.Ptr();
        cfg.workDirPath = workDir.c_str();
        LM_HANDLE h = nullptr;
        const HRESULT hr = ::LayerMountCreate(&cfg, &h);
        std::vector<wchar_t> message(1024);
        SIZE_T required = 0;
        const HRESULT messageHr =
            ::LayerMountGetLastErrorMessage(hr, message.data(), message.size(), &required);
        if (h != nullptr) {
            (void)::LayerMountDestroy(h);
        }
        (void)::LayerMountVhdClose(vhd);

        Assert::AreEqual<HRESULT>(E_INVALIDARG, hr,
            L"A work directory on another volume than the upper fails the create");
        Assert::IsNull(h);
        Assert::AreEqual<HRESULT>(S_OK, messageHr, L"LayerMountGetLastErrorMessage reads the message");
        Assert::IsTrue(required > 1, L"The failed create leaves a message that says why");
    }
};

} // namespace LayerMountAbiTests
