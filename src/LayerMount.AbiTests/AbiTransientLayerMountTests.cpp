#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// LayerMountCreateTransient is a host-portable convenience for short-lived
// overlays (CLI subcommands like vhd/vss/image that need an LM_HANDLE
// to drive the per-overlay primitives but don't actually mount).
TEST_CLASS(AbiTransientLayerMountTests) {
public:
    static std::wstring MakeTempDir() {
        wchar_t tempBuf[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, tempBuf);
        GUID g{};
        ::CoCreateGuid(&g);
        wchar_t guidBuf[64] = {};
        ::StringFromGUID2(g, guidBuf, 64);
        return std::wstring(tempBuf) + L"LayerMountTransient_" + guidBuf;
    }

    static void RemoveAll(const std::wstring& path) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    TEST_METHOD(CreateAndDestroy_HappyPath) {
        const std::wstring workDir = MakeTempDir();

        LM_HANDLE h = nullptr;
        const HRESULT hr = ::LayerMountCreateTransient(
            workDir.c_str(),
            LM_CAP_ADS | LM_CAP_REPARSE_POINTS | LM_CAP_SPARSE_FILES |
                LM_CAP_MULTIPLE_STREAMS | LM_CAP_NTFS_ACLS,
            &h);
        Assert::AreEqual<HRESULT>(S_OK, hr);
        Assert::IsNotNull(h);

        // Handle is usable: GetStats accepts any live LM_HANDLE.
        LM_STATS stats{};
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountGetStats(h, &stats));

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountDestroy(h));
        RemoveAll(workDir);
    }

    TEST_METHOD(CreatesMissingWorkDirectory) {
        const std::wstring workDir = MakeTempDir();
        Assert::IsFalse(std::filesystem::exists(workDir),
            L"Test precondition: workDir must not exist yet");

        LM_HANDLE h = nullptr;
        const HRESULT hr = ::LayerMountCreateTransient(workDir.c_str(),
            LM_CAP_NONE, &h);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"LayerMountCreateTransient must create the workDir on demand");
        Assert::IsTrue(std::filesystem::exists(workDir),
            L"workDir should exist after the call");

        ::LayerMountDestroy(h);
        RemoveAll(workDir);
    }

    TEST_METHOD(NullArgs_ReturnsExpectedHResults) {
        LM_HANDLE h = nullptr;
        Assert::AreEqual<HRESULT>(E_INVALIDARG,
            ::LayerMountCreateTransient(nullptr, LM_CAP_NONE, &h));
        Assert::AreEqual<HRESULT>(E_POINTER,
            ::LayerMountCreateTransient(L"C:\\Temp", LM_CAP_NONE, nullptr));
    }
};

} // namespace LayerMountAbiTests
