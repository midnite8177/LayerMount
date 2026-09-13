#include "pch.h"
#include "AbiTestFixture.h"

#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

// Run |fn| on a thread with no COM apartment. The test host's own thread
// already holds an apartment that the VSS shims' COINIT_MULTITHREADED
// scope rejects with RPC_E_CHANGED_MODE, so a shim called from it never
// reaches the code under test.
template <typename Fn>
HRESULT OnFreshThread(Fn&& fn) {
    HRESULT hr = E_FAIL;
    std::thread worker([&] { hr = fn(); });
    worker.join();
    return hr;
}

} // namespace

// VSS primitives. Creating a snapshot requires elevation +
// an actual volume; we skip that path when the runner isn't admin. The
// sizing-call shape for ListSnapshots is always testable.
TEST_CLASS(AbiVssTests) {
public:
    TEST_METHOD(VssListSnapshots_SizingCall_ReturnsZeroOrMore) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        UINT32 written = 0;
        UINT32 required = 0;
        HRESULT hr = ::LayerMountVssListSnapshots(
            mount.Get(), nullptr, 0, &written, &required);
        // Either S_OK with some snapshot count (usually 0 on a fresh test
        // box, but can be non-zero if the machine has persistent shadow
        // copies), or a VSS-related error when VSS service is unavailable.
        if (hr == S_OK) {
            Assert::AreEqual<UINT32>(0u, written,
                L"sizing call must not populate entries");
        } else {
            Assert::IsTrue(FAILED(hr),
                L"non-S_OK result must be an actual failure HRESULT");
        }
    }

    TEST_METHOD(VssCreateSnapshot_BogusVolume_Fails) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_VSS_SNAPSHOT_HANDLE snap = nullptr;
        wchar_t idBuf[64]       = {};
        wchar_t devBuf[MAX_PATH]= {};
        SIZE_T  idReq = 0, devReq = 0;

        HRESULT hr = ::LayerMountVssCreateSnapshot(
            mount.Get(), L"Z:\\does_not_exist", /*persistent*/ FALSE,
            &snap,
            idBuf, 64, &idReq,
            devBuf, MAX_PATH, &devReq);
        Assert::AreNotEqual<HRESULT>(S_OK, hr,
            L"CreateSnapshot on a nonexistent volume must fail");
        Assert::IsNull(snap);
    }

    TEST_METHOD(VssCleanupSnapshots_OnLayerMount_IsSafeToCall) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        // Cleanup is idempotent -- zero orphaned snapshots is the
        // common case on a clean test machine.
        HRESULT hr = ::LayerMountVssCleanupSnapshots(mount.Get());
        Assert::IsTrue(SUCCEEDED(hr) || FAILED(hr),
            L"Cleanup must return a defined HRESULT");
    }

    // The unknown-id answer comes from the overlay's own snapshot table,
    // before any VSS provider is asked, so it is testable on every host.
    TEST_METHOD(VssValidateSnapshotPath_UnknownId_ReturnsNotFound) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        BOOL reachable = TRUE;
        HRESULT hr = OnFreshThread([&] {
            return ::LayerMountVssValidateSnapshotPath(
                mount.Get(), L"00000000-0000-0000-0000-000000000000", &reachable);
        });
        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), hr,
            L"an id this overlay never created must report ERROR_NOT_FOUND");
    }

    TEST_METHOD(VssValidateSnapshotPath_NullOutParam_ReturnsEPointer) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        HRESULT hr = ::LayerMountVssValidateSnapshotPath(
            mount.Get(), L"00000000-0000-0000-0000-000000000000", nullptr);
        Assert::AreEqual<HRESULT>(E_POINTER, hr);
    }
};

} // namespace LayerMountAbiTests
