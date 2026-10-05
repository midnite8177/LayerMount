#include "pch.h"
#include "AbiTestFixture.h"
#include "FileTimeTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using LayerMountTestShared::GetTimes;
using LayerMountTestShared::MakeFileTime;
using LayerMountTestShared::StampTimes;

namespace LayerMountAbiTests {

// Path/volume primitives: ResolvePath two-call pattern, volume label
// readback, EnsureInUpperLayer copy-up smoke.
TEST_CLASS(AbiPathTests) {
public:
    TEST_METHOD(ResolvePath_LowerLayerFile_ResolvesToLower) {
        TempLayerEnv  env(1);
        env.WriteLowerFile(0, L"data.bin", "xyz");
        LayerMountHolder mount = CreateLayerMount(env);

        std::vector<wchar_t> buf(MAX_PATH);
        LM_RESOLVED_PATH rp{};
        rp.absolutePath      = buf.data();
        rp.absolutePathChars = buf.size();

        HRESULT hr = ::LayerMountResolvePath(mount.Get(), L"\\data.bin", &rp);
        Assert::AreEqual<HRESULT>(S_OK, hr);
        Assert::AreEqual<int>(LM_LAYER_LOWER, static_cast<int>(rp.source));
        Assert::AreEqual<INT32>(0, rp.lowerIndex);
    }

    TEST_METHOD(CreateWhiteout_MarkerSegment_ReturnsInvalidArg) {
        TempLayerEnv  env(1);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(E_INVALIDARG,
            ::LayerMountCreateWhiteout(mount.Get(), L"\\sub\\.wh.x", FALSE),
            L"A whiteout for a path with a marker segment must be rejected");
        Assert::IsFalse(std::filesystem::exists(env.Upper() + L"\\sub\\.wh..wh.x"),
            L"A rejected whiteout must write no marker in the upper");
    }

    TEST_METHOD(SetOpaque_MarkerSegment_ReturnsInvalidArg) {
        TempLayerEnv  env(1);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(E_INVALIDARG,
            ::LayerMountSetOpaque(mount.Get(), L"\\sub\\.wh.x"),
            L"An opaque mark on a path with a marker segment must be rejected");
    }

    TEST_METHOD(SetOpaque_UpperDirectory_KeepsItsLastWriteTime) {
        TempLayerEnv  env(1);
        env.WriteUpperFile(L"sub\\a.txt", "a");
        const std::wstring dir = env.Upper() + L"\\sub";
        const FILETIME stamped = MakeFileTime(2016, 4, 5);
        StampTimes(dir, MakeFileTime(2016, 1, 2), MakeFileTime(2016, 3, 4), stamped);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountSetOpaque(mount.Get(), L"sub"),
            L"The opaque mark on the upper directory must succeed");

        FILETIME creation{}, access{}, write{};
        GetTimes(dir, &creation, &access, &write);
        Assert::AreEqual(0L, ::CompareFileTime(&stamped, &write),
            L"The opaque mark must leave the directory's last-write time as it was");
    }

    TEST_METHOD(SetOpaque_UpperJunction_FailsWithNotADirectoryAndWritesNoMarkerIntoTheTarget) {
        TempLayerEnv  env(1);
        const std::wstring target = env.Root() + L"\\target";
        std::filesystem::create_directories(target);
        if (!CreateDirectoryJunction(env.Upper() + L"\\link", target)) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the upper junction");
            return;
        }
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_NOT_A_DIRECTORY),
            ::LayerMountSetOpaque(mount.Get(), L"link"),
            L"The opaque mark on an upper junction must fail");
        Assert::IsFalse(std::filesystem::exists(target + L"\\.wh..wh..opq"),
            L"The failed opaque mark must write no marker file into the junction target");
    }

    TEST_METHOD(CreateWhiteout_UnderUpperJunction_FailsWithNotADirectoryAndWritesNoMarkerIntoTheTarget) {
        TempLayerEnv  env(1);
        const std::wstring target = env.Root() + L"\\target";
        std::filesystem::create_directories(target);
        if (!CreateDirectoryJunction(env.Upper() + L"\\link", target)) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the upper junction");
            return;
        }
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_NOT_A_DIRECTORY),
            ::LayerMountCreateWhiteout(mount.Get(), L"link\\foo", FALSE),
            L"A whiteout in an upper junction must fail");
        Assert::AreEqual<HRESULT>(E_INVALIDARG,
            ::LayerMountCreateWhiteout(mount.Get(), L"link\\.wh.foo", TRUE),
            L"A whiteout of a .wh. name in an upper junction must be rejected");
        Assert::IsFalse(std::filesystem::exists(target + L"\\.wh.foo"),
            L"The failed whiteout must write no marker file into the junction target");
        Assert::IsFalse(std::filesystem::exists(target + L"\\.wh..wh.foo"),
            L"The failed whiteout of a .wh. name must write no marker file into the junction target");
    }

    TEST_METHOD(CreateWhiteoutAndSetOpaque_MarkerSegmentInLowerJunction_ReturnInvalidArgAndWriteNothing) {
        TempLayerEnv  env(1);
        const std::wstring target = env.Root() + L"\\target";
        std::filesystem::create_directories(target);
        std::ofstream(target + L"\\inside.txt") << "inside";
        if (!CreateDirectoryJunction(env.Lower(0) + L"\\link", target)) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the lower junction");
            return;
        }
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(E_INVALIDARG,
            ::LayerMountCreateWhiteout(mount.Get(), L"link\\.wh.x", FALSE),
            L"A whiteout of a .wh. name in a lower junction must be rejected");
        Assert::AreEqual<HRESULT>(E_INVALIDARG,
            ::LayerMountSetOpaque(mount.Get(), L"link\\.wh.x"),
            L"An opaque mark on a .wh. name in a lower junction must be rejected");

        Assert::IsFalse(std::filesystem::exists(env.Upper() + L"\\link"),
            L"The rejected calls must make no link directory in the upper");
        const auto targetEntries = std::distance(std::filesystem::directory_iterator(target),
                                                 std::filesystem::directory_iterator());
        Assert::AreEqual<std::ptrdiff_t>(1, targetEntries,
            L"The rejected calls must write nothing in the junction target");
    }

    TEST_METHOD(CreateWhiteoutAndSetOpaque_UnderLowerJunction_FailWithNotADirectoryAndWriteNothing) {
        TempLayerEnv  env(1);
        const std::wstring target = env.Root() + L"\\target";
        std::filesystem::create_directories(target + L"\\sub");
        if (!CreateDirectoryJunction(env.Lower(0) + L"\\link", target)) {
            Logger::WriteMessage(L"[SKIP] mklink /J could not create the lower junction");
            return;
        }
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_NOT_A_DIRECTORY),
            ::LayerMountCreateWhiteout(mount.Get(), L"link\\foo", FALSE),
            L"A whiteout in a lower junction must fail");
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_NOT_A_DIRECTORY),
            ::LayerMountSetOpaque(mount.Get(), L"link\\sub"),
            L"An opaque mark on a directory in a lower junction must fail");

        Assert::IsFalse(std::filesystem::exists(env.Upper() + L"\\link"),
            L"The failed calls must make no link directory in the upper");
        Assert::IsFalse(std::filesystem::exists(target + L"\\.wh.foo"),
            L"The failed whiteout must write no marker file into the junction target");
        Assert::IsFalse(std::filesystem::exists(target + L"\\sub\\.wh..wh..opq"),
            L"The failed opaque mark must write no marker file into the junction target");
    }

    TEST_METHOD(ResolvePath_ShortBuffer_ReturnsMoreDataAndRequired) {
        TempLayerEnv  env(1);
        env.WriteLowerFile(0, L"long_name_file.dat", "x");
        LayerMountHolder mount = CreateLayerMount(env);

        wchar_t tiny[4] = {};
        LM_RESOLVED_PATH rp{};
        rp.absolutePath      = tiny;
        rp.absolutePathChars = 4;

        HRESULT hr = ::LayerMountResolvePath(mount.Get(),
                                          L"\\long_name_file.dat", &rp);
        Assert::AreEqual<HRESULT>(
            HRESULT_FROM_WIN32(ERROR_MORE_DATA), hr,
            L"A short absolutePath buffer must surface ERROR_MORE_DATA");
        Assert::IsTrue(rp.absolutePathRequired > 4,
            L"required chars must exceed the 4-char tiny buffer");
    }

    TEST_METHOD(GetVolumeInfo_ReturnslayermountLabel) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_VOLUME_INFO vi{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetVolumeInfo(mount.Get(), &vi));

        // volumeLabelLength is in BYTES; convert to chars.
        const UINT32 labelChars = vi.volumeLabelLength / sizeof(WCHAR);
        Assert::IsTrue(labelChars > 0 && labelChars < 32,
            L"volume label should be a sensible short string");
        std::wstring label(vi.volumeLabel, labelChars);
        Assert::AreEqual<std::wstring>(L"LayerMount", label);
    }

    TEST_METHOD(EnsureInUpperLayer_LowerFile_PromotesToUpper) {
        TempLayerEnv  env(1);
        env.WriteLowerFile(0, L"promote.txt", "lower contents");
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\promote.txt"));

        // Upper must now contain the physical file.
        const std::wstring upperPath = env.Upper() + L"\\promote.txt";
        DWORD attrs = ::GetFileAttributesW(upperPath.c_str());
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES, attrs,
            L"After EnsureInUpperLayer, file must exist in the upper layer");
    }
};

}
