#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

struct EventCapture {
    std::mutex                     mu;
    std::vector<LM_EVENT_TYPE>    types;
    std::vector<std::wstring>      paths;
    std::vector<std::wstring>      messages;
};

void LM_CALL CaptureEvent(const LM_EVENT* evt, void* ctx) {
    if (evt == nullptr || ctx == nullptr) return;
    auto* cap = static_cast<EventCapture*>(ctx);
    std::lock_guard<std::mutex> lock(cap->mu);
    cap->types.push_back(evt->type);
    cap->paths.emplace_back(evt->relativePath ? evt->relativePath : L"");
    cap->messages.emplace_back(evt->message     ? evt->message     : L"");
}

} // namespace

// Event callback fan-out. Install a callback, trigger a copy-up (which
// emits LM_EVT_COPY_UP), and assert the callback captured it.
TEST_CLASS(AbiEventsTests) {
public:
    TEST_METHOD(SetEventCallback_CopyUp_FiresEvent) {
        TempLayerEnv  env(1);
        env.WriteLowerFile(0, L"promo.txt", "from-lower");
        LayerMountHolder mount = CreateLayerMount(env);

        EventCapture cap;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountSetEventCallback(mount.Get(), &CaptureEvent, &cap));

        // Trigger a copy-up via EnsureInUpperLayer.
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\promo.txt"));

        // Unsubscribe to stop any stragglers before asserting.
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountSetEventCallback(mount.Get(), nullptr, nullptr));

        std::lock_guard<std::mutex> lock(cap.mu);
        bool sawCopyUp = false;
        for (auto t : cap.types) {
            if (t == LM_EVT_COPY_UP) { sawCopyUp = true; break; }
        }
        Assert::IsTrue(sawCopyUp,
            L"EnsureInUpperLayer on a lower-layer file should emit LM_EVT_COPY_UP");
    }

    TEST_METHOD(SetEventCallback_Null_Clears) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountSetEventCallback(mount.Get(), nullptr, nullptr));
    }

    TEST_METHOD(SetEventCallback_NullLayerMountHandle_ReturnsEHandle) {
        Assert::AreEqual<HRESULT>(E_HANDLE,
            ::LayerMountSetEventCallback(nullptr, &CaptureEvent, nullptr));
    }
};

} // namespace LayerMountAbiTests
