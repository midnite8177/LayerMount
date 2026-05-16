#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

// Recursive search under <upper>\.overlay for *.meta.json sidecar files.
bool HasSidecarMetadataJson(const std::wstring& upperRoot) {
    const std::wstring sidecarDir = upperRoot + L"\\.overlay";
    std::error_code ec;
    if (!std::filesystem::exists(sidecarDir, ec)) return false;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(sidecarDir, ec)) {
        if (entry.is_regular_file() &&
            entry.path().extension() == L".meta.json") {
            return true;
        }
        // metacopy dir may store files with double extension like
        // <sha1>.meta.json; recursive_directory_iterator walks them.
        std::wstring filename = entry.path().filename().wstring();
        if (filename.size() > 10 &&
            filename.rfind(L".meta.json") != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

// Try to open the :overlay ADS on a given upper-layer file. Returns true
// when the stream exists (opened with CreateFileW), false otherwise.
bool HasLayerMountAds(const std::wstring& upperFile) {
    const std::wstring adsPath = upperFile + L":overlay";
    HANDLE h = ::CreateFileW(adsPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

struct EventSink {
    std::mutex                mu;
    std::vector<LM_EVENT_TYPE> types;
    std::vector<std::wstring>   messages;
};

void LM_CALL SinkEvent(const LM_EVENT* evt, void* ctx) {
    if (!evt || !ctx) return;
    auto* s = static_cast<EventSink*>(ctx);
    std::lock_guard<std::mutex> lock(s->mu);
    s->types.push_back(evt->type);
    s->messages.emplace_back(evt->message ? evt->message : L"");
}

// Create a directory junction (lowerPath -> targetPath) via cmd /c mklink.
// Junctions do not require SeCreateSymbolicLinkPrivilege, so this works
// off-admin. Returns true on success.
bool CreateDirectoryJunction(const std::wstring& junction,
                              const std::wstring& target) {
    // Quote both paths to tolerate spaces.
    std::wstring cmd = L"cmd.exe /c mklink /J \"" + junction +
                       L"\" \"" + target + L"\" >nul 2>&1";
    return _wsystem(cmd.c_str()) == 0;
}

} // namespace

// Capability-degradation paths.
TEST_CLASS(AbiCapabilityDegradationTests) {
public:
    TEST_METHOD(ClearAds_CopyUp_WritesMetadataToSidecarJson) {
        TempLayerEnv  env(1);
        env.WriteLowerFile(0, L"foo.txt", "lower-contents");

        // LM_CAP_ADS cleared -- sidecar JSON must be used.
        const UINT32 caps = LM_CAP_REPARSE_POINTS
                          | LM_CAP_SPARSE_FILES
                          | LM_CAP_MULTIPLE_STREAMS
                          | LM_CAP_NTFS_ACLS;
        LayerMountHolder mount = CreateLayerMount(env, caps);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\foo.txt"));

        const std::wstring upperFile = env.Upper() + L"\\foo.txt";
        Assert::IsTrue(std::filesystem::exists(upperFile),
            L"copy-up should have produced the upper-layer file");

        Assert::IsTrue(HasSidecarMetadataJson(env.Upper()),
            L"With LM_CAP_ADS cleared, a *.meta.json sidecar must exist "
            L"under <upper>\\.overlay\\");
        Assert::IsFalse(HasLayerMountAds(upperFile),
            L"No :overlay ADS stream should be created when LM_CAP_ADS is cleared");
    }

    TEST_METHOD(DefaultAds_CopyUp_WritesToAdsNotSidecar) {
        TempLayerEnv  env(1);
        env.WriteLowerFile(0, L"foo.txt", "lower-contents");

        // Default caps include LM_CAP_ADS -- the ADS branch must be used.
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\foo.txt"));

        const std::wstring upperFile = env.Upper() + L"\\foo.txt";
        Assert::IsTrue(HasLayerMountAds(upperFile),
            L"With LM_CAP_ADS set, a :overlay ADS must be present on the "
            L"upper-layer file");
        Assert::IsFalse(HasSidecarMetadataJson(env.Upper()),
            L"Sidecar *.meta.json must NOT appear when LM_CAP_ADS is set");
    }

    TEST_METHOD(ClearReparsePoints_RenameReparseSourceDirectory_FiresWarning) {
        TempLayerEnv env(1);
        // Seed lower with <target> dir containing a file + a <link> junction
        // pointing at it.
        const std::wstring lower  = env.Lower(0);
        const std::wstring target = lower + L"\\target";
        std::filesystem::create_directories(target);
        { std::ofstream f(target + L"\\inside.txt"); f << "inside-payload"; }

        const std::wstring junction = lower + L"\\link";
        if (!CreateDirectoryJunction(junction, target)) {
            Logger::WriteMessage(L"Skipping: could not create junction (mklink failed)");
            return;
        }

        // Capability: LM_CAP_REPARSE_POINTS cleared.
        const UINT32 caps = LM_CAP_ADS
                          | LM_CAP_SPARSE_FILES
                          | LM_CAP_MULTIPLE_STREAMS
                          | LM_CAP_NTFS_ACLS;
        LayerMountHolder mount = CreateLayerMount(env, caps);

        EventSink sink;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountSetEventCallback(mount.Get(), &SinkEvent, &sink));

        // Rename the reparse-source directory.
        HRESULT hr = ::LayerMountRenameFile(
            mount.Get(), L"\\link", L"\\link-renamed", /*replaceIfExists*/ FALSE);
        // Clear callback before asserting (covers both success and failure).
        (void)::LayerMountSetEventCallback(mount.Get(), nullptr, nullptr);

        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"Rename should succeed even when reparse caps are degraded");

        // Assert the warning fired.
        std::lock_guard<std::mutex> lock(sink.mu);
        bool sawWarning = false;
        for (size_t i = 0; i < sink.types.size(); ++i) {
            if (sink.types[i] == LM_EVT_WARNING &&
                sink.messages[i].find(L"forced full recursive copy-up") !=
                    std::wstring::npos) {
                sawWarning = true;
                break;
            }
        }
        Assert::IsTrue(sawWarning,
            L"LM_CAP_REPARSE_POINTS cleared + reparse-source rename must "
            L"emit LM_EVT_WARNING with the 'forced full recursive copy-up' "
            L"message");
    }
};

} // namespace LayerMountAbiTests
