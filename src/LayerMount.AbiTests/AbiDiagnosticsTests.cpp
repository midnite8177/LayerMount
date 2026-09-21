#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// Diagnostics & process tracker. GetStats increments on IO;
// ProcessTracker enable + ExportJson produces valid parseable JSON via
// the two-call pattern.
TEST_CLASS(AbiDiagnosticsTests) {
public:
    TEST_METHOD(GetStats_IncrementsAfterWrite) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_STATS before{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetStats(mount.Get(), &before));

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\stats.txt",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, opened));
        UINT32 written = 0;
        LM_FILE_INFO postWrite{};
        Assert::AreEqual<HRESULT>(S_OK,
            WriteFromStart(opened.handle, "hello", 5, &written, &postWrite));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        LM_STATS after{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetStats(mount.Get(), &after));

        Assert::IsTrue(after.writeCount > before.writeCount,
            L"writeCount must advance after LayerMountWriteFile");
        Assert::IsTrue(after.bytesWritten >= before.bytesWritten + 5,
            L"bytesWritten must account for the 5-byte write");
    }

    TEST_METHOD(ProcessTrackerEnable_ExportJson_ProducesValidJson) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        constexpr BOOL enable = TRUE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountProcessTrackerEnable(mount.Get(), enable));

        // Touch a file so the tracker has at least one event.
        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\tracked.txt",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, opened));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        // Sizing call.
        SIZE_T required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountProcessTrackerExportJson(mount.Get(),
                nullptr, 0, &required));
        Assert::IsTrue(required > 0, L"required chars should be > 0 after an event");

        std::vector<wchar_t> buf(required);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountProcessTrackerExportJson(mount.Get(),
                buf.data(), buf.size(), &required));

        // Very loose JSON shape check: first non-whitespace char is '{'
        // or '[' (the export is a JSON document).
        size_t i = 0;
        while (i < buf.size() && (buf[i] == L' ' || buf[i] == L'\n'
                                   || buf[i] == L'\r' || buf[i] == L'\t'))
            ++i;
        Assert::IsTrue(i < buf.size() &&
                       (buf[i] == L'{' || buf[i] == L'['),
            L"Export should start with JSON object/array opener");
    }

    TEST_METHOD(ProcessTrackerExportCsv_SizingCall_PopulatesRequired) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        // ExportCsv requires the tracker to be enabled; without it the ABI
        // returns E_ILLEGAL_METHOD_CALL.
        constexpr BOOL enable = TRUE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountProcessTrackerEnable(mount.Get(), enable));

        SIZE_T required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountProcessTrackerExportCsv(mount.Get(),
                nullptr, 0, &required));
        Assert::IsTrue(required >= 1,
            L"CSV export required size must include at least the NUL terminator");
    }

    TEST_METHOD(ProcessTrackerExportCsv_Disabled_ReturnsIllegalMethodCall) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        SIZE_T required = 0;
        HRESULT hr = ::LayerMountProcessTrackerExportCsv(
            mount.Get(), nullptr, 0, &required);
        Assert::AreEqual<HRESULT>(E_ILLEGAL_METHOD_CALL, hr,
            L"ExportCsv with tracker disabled must return E_ILLEGAL_METHOD_CALL");
    }
};

} // namespace LayerMountAbiTests
