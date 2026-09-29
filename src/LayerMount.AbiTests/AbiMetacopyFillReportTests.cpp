#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {
namespace {

constexpr HRESULT kHrSharingViolationNt = HRESULT_FROM_NT(STATUS_SHARING_VIOLATION);

constexpr const wchar_t* kShellName = L"big.bin";
constexpr const wchar_t* kShellPath = L"\\big.bin";

class HeldHostFile {
public:
    HeldHostFile(const std::wstring& path, DWORD access, DWORD shareMode)
        : handle_(::CreateFileW(path.c_str(), access, shareMode, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        Assert::IsTrue(handle_ != INVALID_HANDLE_VALUE,
                       (L"the test holds " + path).c_str());
    }

    ~HeldHostFile() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }

    HeldHostFile(const HeldHostFile&)            = delete;
    HeldHostFile& operator=(const HeldHostFile&) = delete;

private:
    HANDLE handle_;
};

// The fill opens the origin with FILE_SHARE_READ only, so this writer
// makes that open fail with a sharing violation.
HeldHostFile BlockFillWithLowerWriter(const TempLayerEnv& env) {
    return HeldHostFile(env.Lower(0) + kShellPath, GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE);
}

void StageShell(const TempLayerEnv& env, LM_HANDLE mount) {
    (void)WriteLargeLowerFile(env, 0, kShellName);
    OpenedFile staged;
    Assert::AreEqual<HRESULT>(S_OK,
        OpenOverlayFile(mount, kShellPath, kAttributeOnlyAccess, kNoCreateOptions, staged),
        L"the attribute-only open succeeds");
    Assert::IsTrue((staged.info.fileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0,
                   L"the attribute-only open staged a metacopy shell");
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(staged.handle));
}

LM_FILE_HANDLE OpenAttributeOnlyShell(const TempLayerEnv& env, LM_HANDLE mount) {
    StageShell(env, mount);
    OpenedFile opened;
    Assert::AreEqual<HRESULT>(S_OK,
        OpenOverlayFile(mount, kShellPath, kAttributeOnlyAccess, kNoCreateOptions, opened),
        L"the attribute-only open of the shell succeeds");
    return opened.handle;
}

bool LastFailureWasFill() {
    BOOL wasFill = FALSE;
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountGetLastFailureWasFill(&wasFill));
    return wasFill != FALSE;
}

std::wstring LastErrorMessage(HRESULT hr) {
    std::vector<wchar_t> buffer(1024);
    SIZE_T required = 0;
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountGetLastErrorMessage(hr, buffer.data(), buffer.size(), &required));
    return required == 0 ? std::wstring() : std::wstring(buffer.data());
}

bool Contains(const std::wstring& text, const wchar_t* part) {
    return text.find(part) != std::wstring::npos;
}

HRESULT OpenForRead(LM_HANDLE mount, PCWSTR relativePath) {
    OpenedFile opened;
    const HRESULT hr =
        OpenOverlayFile(mount, relativePath, GENERIC_READ, kNoCreateOptions, opened);
    if (opened.handle != nullptr) {
        (void)::LayerMountCloseFile(opened.handle);
    }
    return hr;
}

}

TEST_CLASS(AbiMetacopyFillReportTests) {
public:
    TEST_METHOD(OpenForRead_FillFailsOnHeldLower_ReportsFillWithTheFillStatus) {
        TempLayerEnv     env(1);
        LayerMountHolder mount = CreateLayerMount(env);
        StageShell(env, mount.Get());
        HeldHostFile writer = BlockFillWithLowerWriter(env);

        const HRESULT hr = OpenForRead(mount.Get(), kShellPath);

        Assert::AreEqual<HRESULT>(kHrSharingViolationNt, hr,
            L"the open fails with the fill's status");
        Assert::IsTrue(LastFailureWasFill(), L"the failure is reported as a fill failure");
    }

    TEST_METHOD(OpenForRead_FillFails_MessageNamesTheFillStageAndStatus) {
        TempLayerEnv     env(1);
        LayerMountHolder mount = CreateLayerMount(env);
        StageShell(env, mount.Get());
        HeldHostFile writer = BlockFillWithLowerWriter(env);

        const HRESULT      hr      = OpenForRead(mount.Get(), kShellPath);
        const std::wstring message = LastErrorMessage(hr);

        Logger::WriteMessage(message.c_str());
        Assert::IsTrue(Contains(message, L"fill"), L"the message names the fill");
        Assert::IsTrue(Contains(message, L"origin"), L"the message names the failed stage");
        Assert::IsTrue(Contains(message, L"0xC0000043"),
                       L"the message names the underlying status");
    }

    TEST_METHOD(OpenForRead_UpperFileHeldWithoutSharing_SameStatusIsNotAFill) {
        TempLayerEnv env(1);
        env.WriteUpperFile(L"plain.txt", "plain bytes");
        LayerMountHolder mount = CreateLayerMount(env);
        HeldHostFile     exclusive(env.Upper() + L"\\plain.txt", GENERIC_READ, 0u);

        const HRESULT hr = OpenForRead(mount.Get(), L"\\plain.txt");

        Assert::AreEqual<HRESULT>(kHrSharingViolationNt, hr,
            L"the open of a plain file fails with a sharing violation");
        Assert::IsFalse(LastFailureWasFill(), L"a refused open is not a fill failure");
    }

    TEST_METHOD(FailureOutsideAFill_AfterAFillFailure_ClearsTheReport) {
        TempLayerEnv env(1);
        env.WriteUpperFile(L"plain.txt", "plain bytes");
        LayerMountHolder mount = CreateLayerMount(env);
        StageShell(env, mount.Get());
        HeldHostFile writer = BlockFillWithLowerWriter(env);
        HeldHostFile exclusive(env.Upper() + L"\\plain.txt", GENERIC_READ, 0u);

        Assert::AreEqual<HRESULT>(kHrSharingViolationNt, OpenForRead(mount.Get(), kShellPath));
        Assert::IsTrue(LastFailureWasFill(), L"the first failure came from a fill");

        Assert::AreEqual<HRESULT>(kHrSharingViolationNt,
            OpenForRead(mount.Get(), L"\\plain.txt"));
        Assert::IsFalse(LastFailureWasFill(),
            L"a later failure outside a fill clears the report");
    }

    TEST_METHOD(FailedArgumentCheck_AfterAFillFailure_ClearsTheReport) {
        TempLayerEnv     env(1);
        LayerMountHolder mount = CreateLayerMount(env);
        StageShell(env, mount.Get());
        HeldHostFile writer = BlockFillWithLowerWriter(env);

        Assert::AreEqual<HRESULT>(kHrSharingViolationNt, OpenForRead(mount.Get(), kShellPath));
        Assert::IsTrue(LastFailureWasFill(), L"the failure came from a fill");

        LM_FILE_HANDLE file = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(E_INVALIDARG,
            ::LayerMountOpenFile(mount.Get(), nullptr, GENERIC_READ, kNoCreateOptions,
                                 0, &file, &info),
            L"an open with no path fails its argument check");
        Assert::IsFalse(LastFailureWasFill(),
            L"a failed argument check clears the report");
    }

    TEST_METHOD(StatusTranslation_AfterAFillFailure_KeepsTheReport) {
        TempLayerEnv     env(1);
        LayerMountHolder mount = CreateLayerMount(env);
        StageShell(env, mount.Get());
        HeldHostFile writer = BlockFillWithLowerWriter(env);

        const HRESULT hr = OpenForRead(mount.Get(), kShellPath);
        NTSTATUS status = 0;
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountHResultToNtStatus(hr, &status));

        Assert::AreEqual<NTSTATUS>(STATUS_SHARING_VIOLATION, status);
        Assert::IsTrue(LastFailureWasFill(),
            L"the translation of the failed status keeps the report");
    }

    TEST_METHOD(SuccessfulCall_AfterAFillFailure_ClearsTheReport) {
        TempLayerEnv     env(1);
        LayerMountHolder mount = CreateLayerMount(env);
        StageShell(env, mount.Get());
        HeldHostFile writer = BlockFillWithLowerWriter(env);

        Assert::AreEqual<HRESULT>(kHrSharingViolationNt, OpenForRead(mount.Get(), kShellPath));
        Assert::IsTrue(LastFailureWasFill(), L"the failure came from a fill");

        LM_STATS stats{};
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountGetStats(mount.Get(), &stats));
        Assert::IsFalse(LastFailureWasFill(), L"a successful call clears the report");
    }

    TEST_METHOD(ReportRead_DoesNotClearTheReport) {
        TempLayerEnv     env(1);
        LayerMountHolder mount = CreateLayerMount(env);
        StageShell(env, mount.Get());
        HeldHostFile writer = BlockFillWithLowerWriter(env);

        const HRESULT hr = OpenForRead(mount.Get(), kShellPath);
        (void)LastErrorMessage(hr);

        Assert::IsTrue(LastFailureWasFill(), L"the first read reports the fill");
        Assert::IsTrue(LastFailureWasFill(), L"a second read still reports the fill");
    }

    TEST_METHOD(Write_FillFailsOnHeldLower_ReportsFillWithTheFillStatus) {
        TempLayerEnv         env(1);
        LayerMountHolder     mount = CreateLayerMount(env);
        const LM_FILE_HANDLE fh    = OpenAttributeOnlyShell(env, mount.Get());
        HeldHostFile         writer = BlockFillWithLowerWriter(env);

        const char payload[] = "payload";
        UINT32     written   = 0;
        const HRESULT hr = WriteFromStart(fh, payload, sizeof(payload), &written, nullptr);

        Assert::AreEqual<HRESULT>(kHrSharingViolationNt, hr,
            L"the write fails with the fill's status");
        Assert::IsTrue(LastFailureWasFill(), L"the failure is reported as a fill failure");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }

    TEST_METHOD(SetSize_FillFailsOnHeldLower_ReportsFillWithTheFillStatus) {
        TempLayerEnv         env(1);
        LayerMountHolder     mount = CreateLayerMount(env);
        const LM_FILE_HANDLE fh    = OpenAttributeOnlyShell(env, mount.Get());
        HeldHostFile         writer = BlockFillWithLowerWriter(env);

        constexpr UINT64 newSize = 4096;
        const HRESULT    hr      = SetFileSize(fh, newSize, nullptr);

        Assert::AreEqual<HRESULT>(kHrSharingViolationNt, hr,
            L"the set-size fails with the fill's status");
        Assert::IsTrue(LastFailureWasFill(), L"the failure is reported as a fill failure");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }

    TEST_METHOD(LayerMountGetLastFailureWasFill_NullOut_ReturnsEPointer) {
        Assert::AreEqual<HRESULT>(E_POINTER, ::LayerMountGetLastFailureWasFill(nullptr));
    }
};

}
