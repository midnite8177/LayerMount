#include "pch.h"
#include "AbiTestFixture.h"

#include <algorithm>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

TEST_CLASS(AbiFileOpsTests) {
public:
    TEST_METHOD(CreateWriteReadClose_RoundTrip) {
        TempLayerEnv   env(0);
        LayerMountHolder  mount = CreateLayerMount(env);

        const wchar_t* path      = L"\\hello.txt";
        OpenedFile     opened;
        HRESULT hr = CreateOverlayFile(
            mount.Get(), path, GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
            FILE_ATTRIBUTE_NORMAL, opened);
        Assert::AreEqual<HRESULT>(S_OK, hr, L"LayerMountCreateFile");
        Assert::IsNotNull(opened.handle, L"file handle must be non-null");

        const char     payload[] = "hello overlay";
        const UINT32   payloadLen = static_cast<UINT32>(sizeof(payload) - 1);
        UINT32         written    = 0;
        LM_FILE_INFO  postWrite{};
        Assert::AreEqual<HRESULT>(S_OK,
            WriteFromStart(opened.handle, payload, payloadLen, &written, &postWrite));
        Assert::AreEqual<UINT32>(payloadLen, written);

        char     readBuf[32] = {};
        UINT32   readCount   = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ReadFromStart(opened.handle, readBuf, sizeof(readBuf), &readCount));
        Assert::AreEqual<UINT32>(payloadLen, readCount);
        Assert::IsTrue(std::memcmp(readBuf, payload, payloadLen) == 0,
                       L"Readback must match writeback byte-for-byte");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        LM_RESOLVED_PATH rp{};
        std::vector<wchar_t> absBuf(MAX_PATH);
        rp.absolutePath      = absBuf.data();
        rp.absolutePathChars = absBuf.size();
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountResolvePath(mount.Get(), path, &rp));
        Assert::AreEqual<int>(LM_LAYER_UPPER, static_cast<int>(rp.source));
    }

    TEST_METHOD(OpenFile_NonExistent_ReturnsFileNotFound) {
        TempLayerEnv   env(0);
        LayerMountHolder  mount = CreateLayerMount(env);

        OpenedFile opened;
        HRESULT hr = OpenOverlayFile(
            mount.Get(), L"\\nope.txt", GENERIC_READ, kNoCreateOptions, opened);
        Assert::IsTrue(IsFileNotFoundHr(hr),
            L"An open of a non-existent file must surface as ERROR_FILE_NOT_FOUND "
            L"(Win32 or NT-status encoding)");
        Assert::IsNull(opened.handle, L"out handle must remain null on failure");
    }

    TEST_METHOD(DeleteFile_AfterCreate_RemovesFile) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        const wchar_t* path = L"\\gone.txt";
        {
            OpenedFile created;
            Assert::AreEqual<HRESULT>(S_OK,
                CreateOverlayFile(mount.Get(), path,
                    GENERIC_READ | GENERIC_WRITE | DELETE, kNoCreateOptions,
                    FILE_ATTRIBUTE_NORMAL, created));
            Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(created.handle));
        }

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteFile(mount.Get(), path));

        OpenedFile opened;
        HRESULT hrOpen = OpenOverlayFile(
            mount.Get(), path, GENERIC_READ, kNoCreateOptions, opened);
        Assert::IsTrue(IsFileNotFoundHr(hrOpen),
            L"Opening a deleted file must surface as FileNotFound");
    }

    TEST_METHOD(SetInfo_OnDeletePendingRenamedFile_Succeeds) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile created;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\a.txt",
                GENERIC_READ | GENERIC_WRITE | DELETE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, created));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(created.handle));

        constexpr BOOL replaceIfExists = FALSE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(),
                L"\\a.txt", L"\\b.txt", replaceIfExists));

        OpenedFile renamed;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\b.txt",
                GENERIC_READ | GENERIC_WRITE | DELETE, kNoCreateOptions, renamed));

        const std::wstring upperPath = env.Upper() + L"\\b.txt";
        HANDLE killHandle = ::CreateFileW(
            upperPath.c_str(),
            DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        Assert::IsTrue(killHandle != INVALID_HANDLE_VALUE,
            L"open b.txt for delete-pending setup");

        // Until killHandle closes, a path-based existence check on b.txt
        // reports it as missing.
        FILE_DISPOSITION_INFO disp{};
        disp.DeleteFile = TRUE;
        Assert::IsTrue(
            ::SetFileInformationByHandle(killHandle, FileDispositionInfo,
                                          &disp, sizeof(disp)) != FALSE,
            L"mark b.txt DELETE_PENDING");

        FileInfoChange setNormal;
        setNormal.fileAttributes = FILE_ATTRIBUTE_NORMAL;
        LM_FILE_INFO   postSet{};
        HRESULT hr = SetFileInfo(renamed.handle, setNormal, &postSet);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"SetFileInfo through an open handle on a delete-pending file");

        ::LayerMountCloseFile(renamed.handle);
        ::CloseHandle(killHandle);
    }

    // Windows sends a set-information request, and an overwrite, through
    // whichever handle is open, regardless of the access mask granted at
    // open. This test and the next one open the file with DELETE only.
    TEST_METHOD(SetInfo_TimestampsOnDeleteOnlyHandle_Succeeds) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile seed;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\target.txt",
                GENERIC_READ | GENERIC_WRITE | DELETE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, seed));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(seed.handle));

        OpenedFile deleteOnly;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\target.txt",
                DELETE, kNoCreateOptions, deleteOnly));

        const UINT64 nonZeroLastWriteTime = 132000000000000000ULL;
        LM_FILE_INFO postSet{};
        HRESULT hr = SetFileTimes(deleteOnly.handle, 0u, nonZeroLastWriteTime, &postSet);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"SetFileInfo timestamps through a DELETE-only handle");

        ::LayerMountCloseFile(deleteOnly.handle);
    }

    TEST_METHOD(Overwrite_OnDeleteOnlyHandle_Truncates) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile seed;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\overwrite.bin",
                GENERIC_READ | GENERIC_WRITE | DELETE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, seed));

        const char payload[] = "before-overwrite";
        UINT32 written = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            WriteFromStart(seed.handle, payload, static_cast<UINT32>(sizeof(payload) - 1),
                           &written, nullptr));
        Assert::AreEqual<UINT32>(static_cast<UINT32>(sizeof(payload) - 1), written);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(seed.handle));

        OpenedFile deleteOnly;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\overwrite.bin",
                DELETE, kNoCreateOptions, deleteOnly));

        constexpr UINT64 allocationSize = 4096u;
        LM_FILE_INFO     postOverwrite{};
        HRESULT hr = OverwriteAddingAttributes(deleteOnly.handle, FILE_ATTRIBUTE_NORMAL,
                                               allocationSize, &postOverwrite);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"Overwrite through a handle without FILE_WRITE_DATA");
        Assert::AreEqual<UINT64>(0u, postOverwrite.fileSize,
            L"Overwrite must truncate the file");

        ::LayerMountCloseFile(deleteOnly.handle);
    }

    TEST_METHOD(EnumerateStreams_FileWithNoAds_ReturnsEmpty) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\plain.txt",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, opened));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        UINT32 count = 0xDEADBEEF;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnumerateStreams(mount.Get(), L"\\plain.txt",
                nullptr, 0, &count));
        Assert::AreEqual<UINT32>(0u, count,
            L"A file with only ::$DATA must report zero user-visible streams");
    }

    TEST_METHOD(EnumerateStreams_NonexistentFile_ReturnsNotFound) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        UINT32 count = 0;
        HRESULT hr = ::LayerMountEnumerateStreams(
            mount.Get(), L"\\nope.txt", nullptr, 0, &count);
        Assert::IsTrue(IsFileNotFoundHr(hr),
            L"Enumerate on a missing file must surface as NOT_FOUND");
        Assert::AreEqual<UINT32>(0u, count);
    }

    TEST_METHOD(EnumerateStreams_FileWithAds_ReturnsAdsEntries) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\host.txt",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, opened));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        const std::wstring upper = env.Upper() + L"\\host.txt";
        WriteRawStream(upper + L":secret",  "hush",   4);
        WriteRawStream(upper + L":payload", "abcdef", 6);

        UINT32 required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnumerateStreams(mount.Get(), L"\\host.txt",
                nullptr, 0, &required));
        Assert::AreEqual<UINT32>(2u, required,
            L"two user-visible ADS expected (::$DATA filtered)");

        std::vector<LM_STREAM_INFO> buf(required);
        UINT32 written = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnumerateStreams(mount.Get(), L"\\host.txt",
                buf.data(), required, &written));
        Assert::AreEqual<UINT32>(2u, written);

        bool sawSecret = false;
        bool sawPayload = false;
        for (UINT32 i = 0; i < written; ++i) {
            std::wstring name = buf[i].streamName;
            if (name == L":secret:$DATA") {
                sawSecret = true;
                Assert::AreEqual<UINT64>(4u, buf[i].streamSize);
            } else if (name == L":payload:$DATA") {
                sawPayload = true;
                Assert::AreEqual<UINT64>(6u, buf[i].streamSize);
            } else if (::_wcsicmp(name.c_str(), L"::$DATA") == 0) {
                Assert::Fail(L"main ::$DATA stream must be filtered out");
            } else if (::_wcsicmp(name.c_str(), L":overlay:$DATA") == 0 ||
                       ::_wcsicmp(name.c_str(), L":overlay.opaque:$DATA") == 0) {
                Assert::Fail(L"reserved LayerMount streams must be filtered");
            }
        }
        Assert::IsTrue(sawSecret,  L":secret stream missing from results");
        Assert::IsTrue(sawPayload, L":payload stream missing from results");
    }

    TEST_METHOD(EnumerateStreams_Allocation_IsStreamSizeRoundedUp) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\host.txt",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, opened));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        const std::wstring upper = env.Upper() + L"\\host.txt";
        const std::vector<char> onePage(4096, 'x');
        WriteRawStream(upper + L":secret", "hush", 4);
        WriteRawStream(upper + L":page",   onePage.data(), static_cast<DWORD>(onePage.size()));

        UINT32 required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnumerateStreams(mount.Get(), L"\\host.txt",
                nullptr, 0, &required));
        Assert::AreEqual<UINT32>(2u, required);

        std::vector<LM_STREAM_INFO> buf(required);
        UINT32 written = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnumerateStreams(mount.Get(), L"\\host.txt",
                buf.data(), required, &written));
        Assert::AreEqual<UINT32>(2u, written);

        auto findStream = [&](const wchar_t* name) -> const LM_STREAM_INFO& {
            auto it = std::find_if(buf.begin(), buf.end(),
                [&](const LM_STREAM_INFO& s) { return std::wstring(s.streamName) == name; });
            Assert::IsTrue(it != buf.end(), L"stream missing from results");
            return *it;
        };
        const LM_STREAM_INFO& secret = findStream(L":secret:$DATA");
        Assert::AreEqual<UINT64>(4u, secret.streamSize);
        Assert::AreEqual<UINT64>(4096u, secret.allocationSize,
            L"a 4-byte stream reports one 4 KiB allocation unit");
        const LM_STREAM_INFO& page = findStream(L":page:$DATA");
        Assert::AreEqual<UINT64>(4096u, page.streamSize);
        Assert::AreEqual<UINT64>(4096u, page.allocationSize,
            L"a stream of exactly 4 KiB reports 4 KiB, not a second unit");
    }

    TEST_METHOD(EnumerateStreams_BufferTooSmall_ReturnsMoreData) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\multi.txt",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, opened));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        const std::wstring upper = env.Upper() + L"\\multi.txt";
        for (const wchar_t* s : { L":a", L":b", L":c" }) {
            WriteRawStream(upper + s, "x", 1);
        }

        LM_STREAM_INFO oneSlot{};
        UINT32 count = 0;
        HRESULT hr = ::LayerMountEnumerateStreams(
            mount.Get(), L"\\multi.txt", &oneSlot, 1, &count);
        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_MORE_DATA), hr);
        Assert::AreEqual<UINT32>(3u, count);
    }
};

}
