#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

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
};

} // namespace LayerMountAbiTests
