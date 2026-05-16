#include "pch.h"
#include "AbiTestFixture.h"

#pragma warning(push)
#pragma warning(disable: 4005)   // STATUS_* macro redefinition between winnt.h and ntstatus.h
#include <ntstatus.h>
#pragma warning(pop)

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

// Per-test scratch directory under %TEMP%\LayerMountAbiMP_<guid>\.
// RAII cleanup so a failing assertion doesn't leak directories.
class TempScratchDir {
public:
    TempScratchDir() {
        wchar_t tempDir[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, tempDir);
        GUID g{};
        (void)::CoCreateGuid(&g);
        wchar_t guidBuf[64] = {};
        ::StringFromGUID2(g, guidBuf, 64);
        root_ = std::wstring(tempDir) + L"LayerMountAbiMP_" + guidBuf;
        std::filesystem::create_directories(root_);
    }
    ~TempScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    TempScratchDir(const TempScratchDir&)            = delete;
    TempScratchDir& operator=(const TempScratchDir&) = delete;

    const std::wstring& Root() const noexcept { return root_; }

    // Returns root\<sub> -- never actually creates it.
    std::wstring Sub(const std::wstring& sub) const {
        return root_ + L"\\" + sub;
    }

private:
    std::wstring root_;
};

} // namespace

// LayerMountPoint* are host-side helpers exposed through the C ABI so
// every Windows host adapter can share one canonical implementation of
// directory-mount-point validation and ownership-tracked cleanup. These
// tests pin the contract.
TEST_CLASS(AbiHostMountPointTests) {
public:

    // -- IsDriveLetter ------------------------------------------------------

    TEST_METHOD(IsDriveLetter_NullOutPointer_ReturnsEPointer) {
        Assert::AreEqual<HRESULT>(E_POINTER,
            ::LayerMountPointIsDriveLetter(L"C:", nullptr));
    }

    TEST_METHOD(IsDriveLetter_NullPath_YieldsFalse) {
        BOOL out = TRUE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointIsDriveLetter(nullptr, &out));
        Assert::AreEqual<BOOL>(FALSE, out);
    }

    TEST_METHOD(IsDriveLetter_RecognizesDriveLetterForms) {
        struct Case { const wchar_t* path; BOOL expected; };
        const Case cases[] = {
            { L"C:",        TRUE  },
            { L"C:\\",      TRUE  },
            { L"z:",        TRUE  },
            { L"z:\\",      TRUE  },
            { L"C:\\foo",   FALSE },
            { L"\\\\?\\C:", FALSE },
            { L"foo",       FALSE },
            { L"",          FALSE },
            { L"1:",        FALSE },     // non-letter
            { L":",         FALSE },
        };
        for (const auto& c : cases) {
            BOOL out = c.expected ? FALSE : TRUE;  // start with opposite
            Assert::AreEqual<HRESULT>(S_OK,
                ::LayerMountPointIsDriveLetter(c.path, &out),
                c.path);
            Assert::AreEqual<BOOL>(c.expected, out, c.path);
        }
    }

    // -- PrepareDirectory ---------------------------------------------------

    TEST_METHOD(PrepareDirectory_NullOutPrep_ReturnsEPointer) {
        Assert::AreEqual<HRESULT>(E_POINTER,
            ::LayerMountPointPrepareDirectory(L"C:\\nope", nullptr));
    }

    TEST_METHOD(PrepareDirectory_EmptyPath_ReturnsInvalidParameter) {
        LM_MOUNT_POINT_PREP prep{};
        const HRESULT hr = ::LayerMountPointPrepareDirectory(L"", &prep);
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_INVALID_PARAMETER), hr);
        Assert::AreEqual<BOOL>(FALSE, prep.directoryCreatedByUs);
    }

    TEST_METHOD(PrepareDirectory_NullPath_ReturnsInvalidParameter) {
        LM_MOUNT_POINT_PREP prep{};
        const HRESULT hr = ::LayerMountPointPrepareDirectory(nullptr, &prep);
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_INVALID_PARAMETER), hr);
    }

    TEST_METHOD(PrepareDirectory_FreshPath_ValidatesWithoutClaimingOwnership) {
        TempScratchDir scratch;
        const std::wstring mp = scratch.Sub(L"mnt");

        LM_MOUNT_POINT_PREP prep{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointPrepareDirectory(mp.c_str(), &prep));

        // PrepareDirectory is validation-only: it must NOT create the leaf
        // (the engine's contract is that the host adapter creates the leaf
        // itself as part of its mount call -- adapters that fail on a
        // pre-existing directory would be pre-empted otherwise), and it
        // must NOT set `directoryCreatedByUs` because nothing has actually
        // been reserved yet. Ownership is committed later by the host
        // adapter after CaptureMountPointIdentity proves the host's mount
        // call created a fresh leaf.
        Assert::IsFalse(std::filesystem::exists(mp));
        Assert::AreEqual<BOOL>(FALSE, prep.directoryCreatedByUs);
    }

    TEST_METHOD(PrepareDirectory_PathExists_ReturnsObjectNameCollision) {
        TempScratchDir scratch;
        const std::wstring mp = scratch.Sub(L"already-here");
        std::filesystem::create_directory(mp);

        LM_MOUNT_POINT_PREP prep{};
        const HRESULT hr = ::LayerMountPointPrepareDirectory(mp.c_str(), &prep);
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_OBJECT_NAME_COLLISION), hr);
        Assert::AreEqual<BOOL>(FALSE, prep.directoryCreatedByUs);
    }

    // -- ReleaseIfSafe round-trip ------------------------------------------

    TEST_METHOD(ReleaseIfSafe_NullPrep_ReturnsEPointer) {
        Assert::AreEqual<HRESULT>(E_POINTER,
            ::LayerMountPointReleaseIfSafe(L"C:\\nope", nullptr));
    }

    TEST_METHOD(ReleaseIfSafe_NotOwnedByUs_LeavesDirectory) {
        TempScratchDir scratch;
        const std::wstring mp = scratch.Sub(L"foreign");
        std::filesystem::create_directory(mp);

        LM_MOUNT_POINT_PREP prep{};   // directoryCreatedByUs = FALSE
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointReleaseIfSafe(mp.c_str(), &prep));
        Assert::IsTrue(std::filesystem::exists(mp));
    }

    TEST_METHOD(RoundTrip_PrepareCreateCaptureRelease_RemovesEmptyDirectory) {
        TempScratchDir scratch;
        const std::wstring mp = scratch.Sub(L"mnt");

        LM_MOUNT_POINT_PREP prep{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointPrepareDirectory(mp.c_str(), &prep));
        Assert::AreEqual<BOOL>(FALSE, prep.directoryCreatedByUs,
            L"PrepareDirectory must not claim ownership pre-mount");

        // Simulate the host's mount call materializing the directory, then
        // explicitly claim ownership (the host adapter contract: capture
        // identity + set directoryCreatedByUs = TRUE once the mount call
        // succeeded, which proves the leaf was fresh).
        std::filesystem::create_directory(mp);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointCaptureIdentity(mp.c_str(), &prep));
        Assert::AreNotEqual<UINT64>(0, prep.volumeSerial,
            L"CaptureIdentity should populate volumeSerial for a real directory");
        Assert::AreEqual<BOOL>(FALSE, prep.directoryCreatedByUs,
            L"CaptureIdentity must be side-effect-free on ownership");
        prep.directoryCreatedByUs = TRUE;

        // Empty + identity-matched + we claimed ownership -> removal happens.
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointReleaseIfSafe(mp.c_str(), &prep));
        Assert::IsFalse(std::filesystem::exists(mp));
    }

    TEST_METHOD(RoundTrip_NonEmpty_LeavesDirectory) {
        TempScratchDir scratch;
        const std::wstring mp = scratch.Sub(L"mnt");

        LM_MOUNT_POINT_PREP prep{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointPrepareDirectory(mp.c_str(), &prep));
        std::filesystem::create_directory(mp);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointCaptureIdentity(mp.c_str(), &prep));
        prep.directoryCreatedByUs = TRUE;

        // Drop a file inside; ReleaseIfSafe must refuse to delete a non-empty dir.
        const std::wstring child = mp + L"\\inhabitant.txt";
        { std::ofstream f(child, std::ios::binary); f << "stay"; }

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointReleaseIfSafe(mp.c_str(), &prep));
        Assert::IsTrue(std::filesystem::exists(mp));
        Assert::IsTrue(std::filesystem::exists(child));
    }

    TEST_METHOD(RoundTrip_IdentityMismatch_LeavesDirectory) {
        TempScratchDir scratch;
        const std::wstring mp = scratch.Sub(L"mnt");

        LM_MOUNT_POINT_PREP prep{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointPrepareDirectory(mp.c_str(), &prep));
        std::filesystem::create_directory(mp);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointCaptureIdentity(mp.c_str(), &prep));
        prep.directoryCreatedByUs = TRUE;

        // Replace the directory between Capture and Release: same name,
        // different file-id. ReleaseIfSafe must refuse to remove it.
        std::filesystem::remove(mp);
        std::filesystem::create_directory(mp);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointReleaseIfSafe(mp.c_str(), &prep));
        Assert::IsTrue(std::filesystem::exists(mp),
            L"Identity-mismatched replacement directory must not be removed");
    }
};

} // namespace LayerMountAbiTests
