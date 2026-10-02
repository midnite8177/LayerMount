#include "pch.h"
#include "AbiTestFixture.h"
#include "StreamTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using LayerMountTestShared::HasOverlayStream;
using LayerMountTestShared::HasStream;

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

// Writes inside.txt holding "inside-payload" into target and creates the
// lower junction "link" to target. Logs a skip and returns false when the
// junction cannot be created.
bool BuildLowerJunction(const TempLayerEnv& env, const std::wstring& target) {
    std::filesystem::create_directories(target);
    { std::ofstream f(target + L"\\inside.txt"); f << "inside-payload"; }
    if (!CreateDirectoryJunction(env.Lower(0) + L"\\link", target)) {
        Logger::WriteMessage(L"Skipping: could not create junction (mklink failed)");
        return false;
    }
    return true;
}

// Asserts that upperLink is a link and that the file at readPath, reached
// through it, holds expectedPayload on its first line.
void AssertUpperLink(const std::wstring& upperLink,
                     const std::wstring& readPath,
                     const std::string& expectedPayload) {
    const DWORD attrs = ::GetFileAttributesW(upperLink.c_str());
    Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES, attrs,
        (L"The upper must hold " + upperLink).c_str());
    Assert::AreNotEqual<DWORD>(0, attrs & FILE_ATTRIBUTE_REPARSE_POINT,
        (L"The upper " + upperLink + L" must be a link").c_str());
    std::string payload;
    { std::ifstream f(readPath); std::getline(f, payload); }
    Assert::AreEqual(expectedPayload, payload,
        (L"The upper " + upperLink + L" must point to the link target").c_str());
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
        Assert::IsFalse(HasOverlayStream(upperFile),
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
        Assert::IsTrue(HasOverlayStream(upperFile),
            L"With LM_CAP_ADS set, a :overlay ADS must be present on the "
            L"upper-layer file");
        Assert::IsFalse(HasSidecarMetadataJson(env.Upper()),
            L"Sidecar *.meta.json must NOT appear when LM_CAP_ADS is set");
    }

    TEST_METHOD(DefaultAds_EnsureInUpperLayerOnLowerJunction_CopiesTheLinkAndWritesNoStreamOntoItsTarget) {
        TempLayerEnv env(1);
        const std::wstring target = env.Root() + L"\\target";
        if (!BuildLowerJunction(env, target)) {
            return;
        }

        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\link"),
            L"The copy-up of a lower junction must succeed with LM_CAP_ADS set");

        AssertUpperLink(env.Upper() + L"\\link", env.Upper() + L"\\link\\inside.txt",
                        "inside-payload");
        Assert::IsFalse(HasOverlayStream(target),
            L"The copy-up must write no :overlay stream onto the junction target");
    }

    TEST_METHOD(DefaultAds_EnsureInUpperLayerOnLowerFileSymlink_CopiesTheLinkAndWritesNoStreamOntoItsTarget) {
        TempLayerEnv env(1);
        const std::wstring target = env.Root() + L"\\target.txt";
        { std::ofstream f(target); f << "target-payload"; }
        const std::wstring lowerLink = env.Lower(0) + L"\\link.txt";
        if (!::CreateSymbolicLinkW(lowerLink.c_str(), target.c_str(),
                                   SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            Logger::WriteMessage(L"Skipping: could not create the file symlink");
            return;
        }

        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\link.txt"),
            L"The copy-up of a lower file symlink must succeed with LM_CAP_ADS set");

        const std::wstring upperLink = env.Upper() + L"\\link.txt";
        AssertUpperLink(upperLink, upperLink, "target-payload");
        Assert::IsFalse(HasOverlayStream(target),
            L"The copy-up must write no :overlay stream onto the symlink target");
    }

    TEST_METHOD(ClearReparsePoints_RenameLowerJunction_CopiesTheLinkAndWritesNoOpaqueMarkerIntoItsTarget) {
        TempLayerEnv env(1);
        const std::wstring target = env.Root() + L"\\target";
        if (!BuildLowerJunction(env, target)) {
            return;
        }

        const UINT32 caps = LM_CAP_ADS
                          | LM_CAP_SPARSE_FILES
                          | LM_CAP_MULTIPLE_STREAMS
                          | LM_CAP_NTFS_ACLS;
        LayerMountHolder mount = CreateLayerMount(env, caps);

        EventSink sink;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountSetEventCallback(mount.Get(), &SinkEvent, &sink));

        constexpr BOOL replaceIfExists = FALSE;
        HRESULT hr = ::LayerMountRenameFile(
            mount.Get(), L"\\link", L"\\link-renamed", replaceIfExists);
        // An assert that throws destroys sink before mount, so clear it first.
        (void)::LayerMountSetEventCallback(mount.Get(), nullptr, nullptr);

        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"The rename of a lower junction must succeed without reparse-point support");

        AssertUpperLink(env.Upper() + L"\\link-renamed",
                        env.Upper() + L"\\link-renamed\\inside.txt", "inside-payload");
        Assert::IsFalse(std::filesystem::exists(target + L"\\.wh..wh..opq"),
            L"The rename must write no opaque marker file into the junction target");
        Assert::IsFalse(HasStream(target + L":overlay.opaque"),
            L"The rename must write no opaque stream onto the junction target");

        std::lock_guard<std::mutex> lock(sink.mu);
        for (const LM_EVENT_TYPE type : sink.types) {
            Assert::AreNotEqual<int>(LM_EVT_WARNING, type,
                L"The rename must emit no warning");
        }
    }
};

} // namespace LayerMountAbiTests
