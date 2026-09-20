#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

TEST_CLASS(AbiFileOpsTests) {
public:
    TEST_METHOD(CreateWriteReadClose_RoundTrip) {
        TempLayerEnv   env(0);
        LayerMountHolder  mount = CreateLayerMount(env);

        const wchar_t* path      = L"\\hello.txt";
        LM_FILE_HANDLE fh       = nullptr;
        LM_FILE_INFO   info{};
        HRESULT hr = ::LayerMountCreateFile(
            mount.Get(), path,
            /*createOptions*/ 0u,
            /*grantedAccess*/ GENERIC_READ | GENERIC_WRITE,
            /*fileAttributes*/ FILE_ATTRIBUTE_NORMAL,
            /*securityDescriptor*/ nullptr, 0u,
            /*allocationSize*/ 0u,
            /*originatorPid*/ 0u,
            &fh, &info);
        Assert::AreEqual<HRESULT>(S_OK, hr, L"LayerMountCreateFile");
        Assert::IsNotNull(fh, L"file handle must be non-null");

        const char     payload[] = "hello overlay";
        const UINT32   payloadLen = static_cast<UINT32>(sizeof(payload) - 1);
        UINT32         written    = 0;
        LM_FILE_INFO  postWrite{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(fh, payload, /*offset*/ 0, payloadLen,
                               /*writeToEnd*/ FALSE, /*constrainedIo*/ FALSE,
                               /*originatorPid*/ 0u, &written, &postWrite));
        Assert::AreEqual<UINT32>(payloadLen, written);

        char     readBuf[32] = {};
        UINT32   readCount   = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountReadFile(fh, readBuf, /*offset*/ 0, sizeof(readBuf),
                              /*originatorPid*/ 0u, &readCount));
        Assert::AreEqual<UINT32>(payloadLen, readCount);
        Assert::IsTrue(std::memcmp(readBuf, payload, payloadLen) == 0,
                       L"Readback must match writeback byte-for-byte");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

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

        LM_FILE_HANDLE fh   = nullptr;
        LM_FILE_INFO   info{};
        HRESULT hr = ::LayerMountOpenFile(
            mount.Get(), L"\\nope.txt",
            /*grantedAccess*/ GENERIC_READ, /*createOptions*/ 0u,
            /*originatorPid*/ 0u, &fh, &info);
        Assert::IsTrue(IsFileNotFoundHr(hr),
            L"An open of a non-existent file must surface as ERROR_FILE_NOT_FOUND "
            L"(Win32 or NT-status encoding)");
        Assert::IsNull(fh, L"out handle must remain null on failure");
    }

    TEST_METHOD(DeleteFile_AfterCreate_RemovesFile) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        const wchar_t* path = L"\\gone.txt";
        {
            LM_FILE_HANDLE fh = nullptr;
            LM_FILE_INFO   info{};
            Assert::AreEqual<HRESULT>(S_OK,
                ::LayerMountCreateFile(mount.Get(), path, 0u,
                    GENERIC_READ | GENERIC_WRITE | DELETE,
                    FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u,
                    &fh, &info));
            Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
        }

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteFile(mount.Get(), path));

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        HRESULT hrOpen = ::LayerMountOpenFile(
            mount.Get(), path, GENERIC_READ, 0u, 0u, &fh, &info);
        Assert::IsTrue(IsFileNotFoundHr(hrOpen),
            L"Opening a deleted file must surface as FileNotFound");
    }

    TEST_METHOD(SetInfo_OnDeletePendingRenamedFile_Succeeds) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fhA = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\a.txt", 0u,
                GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fhA, &info));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fhA));

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(),
                L"\\a.txt", L"\\b.txt", FALSE));

        LM_FILE_HANDLE fhB = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\b.txt",
                GENERIC_READ | GENERIC_WRITE | DELETE,
                0u, 0u, &fhB, &info));

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

        LM_FILE_INFO postSet{};
        HRESULT hr = ::LayerMountSetFileInfo(
            fhB,
            FILE_ATTRIBUTE_NORMAL,
            /*creationTime*/   0,
            /*lastAccessTime*/ 0,
            /*lastWriteTime*/  0,
            /*changeTime*/     0,
            /*allocationSize*/ UINT64_MAX,
            /*fileSize*/       UINT64_MAX,
            &postSet);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"SetFileInfo through an open handle on a delete-pending file");

        ::LayerMountCloseFile(fhB);
        ::CloseHandle(killHandle);
    }

    // Windows sends a set-information request, and an overwrite, through
    // whichever handle is open, regardless of the access mask granted at
    // open. This test and the next one open the file with DELETE only.
    TEST_METHOD(SetInfo_TimestampsOnDeleteOnlyHandle_Succeeds) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fhSeed = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\target.txt", 0u,
                GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fhSeed, &info));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fhSeed));

        LM_FILE_HANDLE fhDel = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\target.txt",
                DELETE, 0u, 0u, &fhDel, &info));

        const UINT64 nonZeroLastWriteTime = 132000000000000000ULL;
        LM_FILE_INFO postSet{};
        HRESULT hr = SetLastWriteTime(fhDel, nonZeroLastWriteTime, &postSet);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"SetFileInfo timestamps through a DELETE-only handle");

        ::LayerMountCloseFile(fhDel);
    }

    TEST_METHOD(Overwrite_OnDeleteOnlyHandle_Truncates) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fhSeed = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\overwrite.bin", 0u,
                GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fhSeed, &info));

        const char payload[] = "before-overwrite";
        UINT32 written = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(fhSeed, payload, 0,
                                  static_cast<UINT32>(sizeof(payload) - 1),
                                  FALSE, FALSE, 0u, &written, nullptr));
        Assert::AreEqual<UINT32>(static_cast<UINT32>(sizeof(payload) - 1), written);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fhSeed));

        LM_FILE_HANDLE fhDel = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\overwrite.bin",
                DELETE, 0u, 0u, &fhDel, &info));

        LM_FILE_INFO postOverwrite{};
        HRESULT hr = ::LayerMountOverwriteFile(
            fhDel,
            /*fileAttributes*/   FILE_ATTRIBUTE_NORMAL,
            /*replaceAttributes*/ FALSE,
            /*allocationSize*/   4096u,
            /*originatorPid*/    0u,
            &postOverwrite);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"Overwrite through a handle without FILE_WRITE_DATA");
        Assert::AreEqual<UINT64>(0u, postOverwrite.fileSize,
            L"Overwrite must truncate the file");

        ::LayerMountCloseFile(fhDel);
    }

    TEST_METHOD(EnumerateStreams_FileWithNoAds_ReturnsEmpty) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\plain.txt", 0u,
                GENERIC_READ | GENERIC_WRITE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fh, &info));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

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

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\host.txt", 0u,
                GENERIC_READ | GENERIC_WRITE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fh, &info));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        const std::wstring upper = env.Upper() + L"\\host.txt";
        auto writeStream = [](const std::wstring& path, const char* data, DWORD len) {
            HANDLE h = ::CreateFileW(path.c_str(),
                GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            Assert::IsTrue(h != INVALID_HANDLE_VALUE, L"open ADS for write");
            DWORD written = 0;
            BOOL ok = ::WriteFile(h, data, len, &written, nullptr);
            ::CloseHandle(h);
            Assert::IsTrue(ok != FALSE,        L"WriteFile to ADS succeeded");
            Assert::AreEqual<DWORD>(len, written, L"WriteFile wrote full payload");
        };
        writeStream(upper + L":secret",  "hush",   4);
        writeStream(upper + L":payload", "abcdef", 6);

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

    TEST_METHOD(EnumerateStreams_BufferTooSmall_ReturnsMoreData) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\multi.txt", 0u,
                GENERIC_READ | GENERIC_WRITE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fh, &info));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        const std::wstring upper = env.Upper() + L"\\multi.txt";
        for (const wchar_t* s : { L":a", L":b", L":c" }) {
            HANDLE h = ::CreateFileW((upper + s).c_str(),
                GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            Assert::IsTrue(h != INVALID_HANDLE_VALUE, L"open ADS for write");
            DWORD written = 0;
            BOOL ok = ::WriteFile(h, "x", 1, &written, nullptr);
            ::CloseHandle(h);
            Assert::IsTrue(ok != FALSE,        L"WriteFile to ADS succeeded");
            Assert::AreEqual<DWORD>(1u, written, L"WriteFile wrote single byte");
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
