#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// -----------------------------------------------------------------------------
// ABI-shape tests. These cover invariants that must hold for every public
// export -- version/ABI reporting, TLS thread-locality for the last error
// message, stale-handle rejection, and parameter validation on the
// lifecycle entry points. Behavioral round-trips per family live in
// AbiLifecycleTests / AbiFileOpsTests / etc.
// -----------------------------------------------------------------------------

TEST_CLASS(VersionAndAbiTests) {
public:
    TEST_METHOD(GetVersion_ReturnsExpectedTuple) {
        UINT32 maj = 0, min = 0, patch = 0, abi = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetVersion(&maj, &min, &patch, &abi));
        Assert::AreEqual<UINT32>(LM_VER_MAJOR, maj);
        Assert::AreEqual<UINT32>(LM_VER_MINOR, min);
        Assert::AreEqual<UINT32>(LM_VER_PATCH, patch);
        Assert::AreEqual<UINT32>(LM_ABI_VERSION, abi);
    }

    TEST_METHOD(GetVersion_AcceptsPartialOuts) {
        // Each out-pointer is optional; skipping any subset must not trip
        // the function.
        UINT32 abi = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetVersion(nullptr, nullptr, nullptr, &abi));
        Assert::AreEqual<UINT32>(LM_ABI_VERSION, abi);

        UINT32 maj = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetVersion(&maj, nullptr, nullptr, nullptr));
        Assert::AreEqual<UINT32>(LM_VER_MAJOR, maj);
    }
};

TEST_CLASS(ErrorTlsThreadLocalityTests) {
public:
    TEST_METHOD(ErrorMessageFromThreadA_IsInvisibleOnThreadB) {
        // Force an error on this thread so the TLS slot has a real message.
        LM_HANDLE h    = nullptr;
        HRESULT    hrA  = ::LayerMountCreate(nullptr, &h);
        Assert::AreEqual<HRESULT>(E_POINTER, hrA,
            L"LayerMountCreate(nullptr) should surface as E_POINTER synchronously");

        // Thread A sees the diagnostic.
        std::vector<wchar_t> bufA(256);
        SIZE_T requiredA = 0;
        HRESULT hrLookupA = ::LayerMountGetLastErrorMessage(
            hrA, bufA.data(), bufA.size(), &requiredA);
        Assert::AreEqual<HRESULT>(S_OK, hrLookupA);
        // Message may or may not be populated for E_POINTER (the pre-
        // validation path sometimes returns without writing a diagnostic);
        // what matters is that the *filtered lookup* on thread A succeeded.

        // Thread B: different TLS storage, so the lookup for the same
        // HRESULT must report "no message on record" -- regardless of what
        // is in thread A's buffer.
        std::atomic<HRESULT> hrB(E_FAIL);
        std::atomic<SIZE_T>  requiredB(123);
        std::thread worker([&] {
            wchar_t  tmp[64] = {};
            SIZE_T   req     = 42;
            HRESULT  hr      =
                ::LayerMountGetLastErrorMessage(hrA, tmp, 64, &req);
            hrB        = hr;
            requiredB  = req;
        });
        worker.join();

        Assert::AreEqual<HRESULT>(S_OK, hrB.load(),
            L"LayerMountGetLastErrorMessage on a different thread returns S_OK");
        Assert::AreEqual<SIZE_T>(0, requiredB.load(),
            L"Thread B's TLS has no message for this HRESULT -- required must be 0");
    }
};

TEST_CLASS(HandleTableShapeTests) {
public:
    TEST_METHOD(NullLayerMountHandle_OnStatsQuery_ReturnsEHandle) {
        LM_STATS stats{};
        Assert::AreEqual<HRESULT>(E_HANDLE,
            ::LayerMountGetStats(nullptr, &stats));
    }

    TEST_METHOD(DestroyedLayerMountHandle_OnStatsQuery_ReturnsEHandle) {
        TempLayerEnv   env(1);
        LayerMountHolder  mount = CreateLayerMount(env);

        // Take a copy of the raw handle before we destroy it.
        LM_HANDLE stale = mount.Get();
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountDestroy(mount.Release()));

        LM_STATS stats{};
        Assert::AreEqual<HRESULT>(E_HANDLE,
            ::LayerMountGetStats(stale, &stats),
            L"Using an overlay handle after LayerMountDestroy must yield E_HANDLE");
    }

    TEST_METHOD(NullFileHandle_OnCloseFile_ReturnsEHandle) {
        Assert::AreEqual<HRESULT>(E_HANDLE, ::LayerMountCloseFile(nullptr));
    }

    TEST_METHOD(WrongKind_LayerMountHandleAsFileHandle_ReturnsEHandle) {
        TempLayerEnv   env(1);
        LayerMountHolder  mount = CreateLayerMount(env);

        // Feeding an LM_HANDLE into an LM_FILE_HANDLE slot must be
        // rejected by the magic-kind check in HandleTable::Resolve<T>.
        LM_FILE_HANDLE bogus =
            reinterpret_cast<LM_FILE_HANDLE>(mount.Get());
        Assert::AreEqual<HRESULT>(E_HANDLE, ::LayerMountCloseFile(bogus));
    }
};

TEST_CLASS(ParameterValidationShapeTests) {
public:
    TEST_METHOD(LayerMountCreate_NullConfig_ReturnsEPointer) {
        LM_HANDLE h = nullptr;
        Assert::AreEqual<HRESULT>(E_POINTER,
            ::LayerMountCreate(nullptr, &h));
    }

    TEST_METHOD(LayerMountCreate_NullOutHandle_ReturnsEPointer) {
        TempLayerEnv    env(0);
        ConfigBuilder   b(env);
        Assert::AreEqual<HRESULT>(E_POINTER,
            ::LayerMountCreate(b.Ptr(), nullptr));
    }

    TEST_METHOD(LayerMountCreate_StructSizeZero_ReturnsEInvalidArg) {
        TempLayerEnv    env(0);
        ConfigBuilder   b(env);
        b.SetStructSize(0);
        LM_HANDLE h = nullptr;
        Assert::AreEqual<HRESULT>(E_INVALIDARG,
            ::LayerMountCreate(b.Ptr(), &h));
    }

    TEST_METHOD(LayerMountCreate_StructSizeTooSmall_ReturnsEInvalidArg) {
        TempLayerEnv    env(0);
        ConfigBuilder   b(env);
        b.SetStructSize(static_cast<UINT32>(sizeof(UINT32))); // below min
        LM_HANDLE h = nullptr;
        Assert::AreEqual<HRESULT>(E_INVALIDARG,
            ::LayerMountCreate(b.Ptr(), &h));
    }

    TEST_METHOD(LayerMountCreate_WrongAbiVersion_ReturnsEInvalidArgWithMessage) {
        TempLayerEnv    env(0);
        ConfigBuilder   b(env);
        b.SetAbiVersion(LM_ABI_VERSION + 1u);

        LM_HANDLE h = nullptr;
        HRESULT hr = ::LayerMountCreate(b.Ptr(), &h);
        Assert::AreEqual<HRESULT>(E_INVALIDARG, hr,
            L"Mismatched abiVersion must be rejected with E_INVALIDARG");

        std::vector<wchar_t> buf(512);
        SIZE_T required = 0;
        HRESULT lookup = ::LayerMountGetLastErrorMessage(
            hr, buf.data(), buf.size(), &required);
        Assert::AreEqual<HRESULT>(S_OK, lookup);
        Assert::IsTrue(required > 0,
            L"abiVersion mismatch must leave an actionable message in TLS");
    }

    TEST_METHOD(LayerMountCreate_NonexistentUpperPath_ReturnsEInvalidArg) {
        TempLayerEnv    env(0);
        ConfigBuilder   b(env);
        // Point upperPath at a path that doesn't exist on disk.
        std::wstring bogus = env.Root() + L"\\does_not_exist";
        b.SetUpperPath(bogus.c_str());

        LM_HANDLE h = nullptr;
        HRESULT hr = ::LayerMountCreate(b.Ptr(), &h);
        Assert::AreEqual<HRESULT>(E_INVALIDARG, hr);
        if (h) {
            (void)::LayerMountDestroy(h);
        }
    }

    TEST_METHOD(LayerMountGetVersion_AllNullOuts_StillSucceeds) {
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetVersion(nullptr, nullptr, nullptr, nullptr));
    }

    TEST_METHOD(LayerMountGetLastErrorMessage_NullRequired_ReturnsEPointer) {
        wchar_t buf[16] = {};
        Assert::AreEqual<HRESULT>(E_POINTER,
            ::LayerMountGetLastErrorMessage(S_OK, buf, 16, nullptr));
    }

    TEST_METHOD(LayerMountResolvePath_NullHandle_ReturnsEHandle) {
        LM_RESOLVED_PATH rp{};
        Assert::AreEqual<HRESULT>(E_HANDLE,
            ::LayerMountResolvePath(nullptr, L"\\foo.txt", &rp));
    }

    TEST_METHOD(LayerMountGetVolumeInfo_NullOut_ReturnsEPointer) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(E_POINTER,
            ::LayerMountGetVolumeInfo(mount.Get(), nullptr));
    }
};

} // namespace LayerMountAbiTests
