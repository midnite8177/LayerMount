#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// File-primitive round-trip (Create/Write/Read/Close) plus one error
// path (Open a nonexistent file).
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
        Assert::IsNotNull(fh, L"file handle should be non-null");

        const char     payload[] = "hello overlay";
        const UINT32   payloadLen = static_cast<UINT32>(sizeof(payload) - 1);
        UINT32         written    = 0;
        LM_FILE_INFO  postWrite{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(fh, payload, /*offset*/ 0, payloadLen,
                               /*writeToEnd*/ FALSE, /*constrainedIo*/ FALSE,
                               /*originatorPid*/ 0u, &written, &postWrite));
        Assert::AreEqual<UINT32>(payloadLen, written);

        // Rewind-ish: LayerMountReadFile takes an explicit offset.
        char     readBuf[32] = {};
        UINT32   readCount   = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountReadFile(fh, readBuf, /*offset*/ 0, sizeof(readBuf),
                              /*originatorPid*/ 0u, &readCount));
        Assert::AreEqual<UINT32>(payloadLen, readCount);
        Assert::IsTrue(std::memcmp(readBuf, payload, payloadLen) == 0,
                       L"Readback should match writeback byte-for-byte");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        // File should now be visible via ResolvePath in the upper layer.
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
            L"Opening a non-existent file should surface as ERROR_FILE_NOT_FOUND "
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

        // Subsequent Open should fail.
        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        HRESULT hrOpen = ::LayerMountOpenFile(
            mount.Get(), path, GENERIC_READ, 0u, 0u, &fh, &info);
        Assert::IsTrue(IsFileNotFoundHr(hrOpen),
            L"Opening a deleted file must surface as FileNotFound");
    }

    // Regression: SetFileInfo via a still-valid LayerMount handle must
    // continue to work even when the upper-layer NTFS file has been
    // placed in DELETE_PENDING by a separate handle. Path-based
    // existence checks inside EnsureInUpperLayer otherwise report the
    // file as missing the instant any handle sets
    // FILE_DISPOSITION_INFORMATION::DeleteFile = TRUE, surfacing as
    // HRESULT 0xD0000034 (STATUS_OBJECT_NAME_NOT_FOUND). Handles that
    // were opened before the disposition was set remain valid kernel-
    // side and should be usable.
    TEST_METHOD(SetInfo_OnDeletePendingFile_DoesNotFailWithObjectNotFound) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        // Create \\a.txt and close.
        LM_FILE_HANDLE fhA = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\a.txt", 0u,
                GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fhA, &info));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fhA));

        // Rename a.txt -> b.txt so we're operating on a renamed file.
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(),
                L"\\a.txt", L"\\b.txt", FALSE));

        // Open b.txt via the engine and keep the handle live for the
        // duration of the test. This is the handle SetInfo will run on.
        LM_FILE_HANDLE fhB = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\b.txt",
                GENERIC_READ | GENERIC_WRITE | DELETE,
                0u, 0u, &fhB, &info));

        // Open a separate raw-Win32 handle and mark the upper-layer
        // file DELETE_PENDING. Any path-based existence check from
        // here on returns "not found" while this handle stays open.
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

        FILE_DISPOSITION_INFO disp{};
        disp.DeleteFile = TRUE;
        Assert::IsTrue(
            ::SetFileInformationByHandle(killHandle, FileDispositionInfo,
                                          &disp, sizeof(disp)) != FALSE,
            L"mark b.txt DELETE_PENDING");

        // SetFileInfo through fhB must succeed even though the
        // path-based check would say b.txt no longer exists.
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
            L"SetFileInfo on a DELETE_PENDING file via an already-open "
            L"handle should not return STATUS_OBJECT_NAME_NOT_FOUND");

        ::LayerMountCloseFile(fhB);
        ::CloseHandle(killHandle); // commits the actual delete
    }

    // Regression: SetFileInfo against a handle that does NOT carry
    // FILE_WRITE_ATTRIBUTES access right must still be able to update
    // timestamps. The kernel routes SET_INFORMATION calls through
    // whichever open handle exists for the file, regardless of the
    // access mask granted at open time. When a caller opens a file
    // with only DELETE access (typical for a del-style operation),
    // LayerMount's internal kernel handle inherits that access mask;
    // the subsequent ::SetFileTime call inside SetInfo then fails with
    // ERROR_ACCESS_DENIED (Win32 err 5 -> STATUS_ACCESS_DENIED ->
    // HRESULT 0xD0000022). LayerMount must work around this so that
    // attribute / timestamp updates work even for handles opened with
    // minimal access.
    TEST_METHOD(SetInfo_TimestampsOnDeleteOnlyHandle_DoesNotFailWithAccessDenied) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        // Seed: create the file with full access so the upper-layer
        // entry exists, then close.
        LM_FILE_HANDLE fhSeed = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\target.txt", 0u,
                GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fhSeed, &info));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fhSeed));

        // Reopen with DELETE only — no read, no write, and crucially no
        // FILE_WRITE_ATTRIBUTES. This is the access mask a "del" path
        // typically carries.
        LM_FILE_HANDLE fhDel = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\target.txt",
                DELETE, 0u, 0u, &fhDel, &info));

        // Try SetFileInfo with a non-zero LastWriteTime. The kernel
        // would issue this as part of a SET_INFORMATION pass with
        // FILE_BASIC_INFORMATION carrying a real timestamp; FromDateTime
        // in the caller converts a non-default DateTime to a non-zero
        // FILETIME tick value, taking us into the SetFileTime branch
        // of SetInfo.
        const UINT64 someValidFileTime = 132000000000000000ULL; // ~2019
        LM_FILE_INFO postSet{};
        HRESULT hr = ::LayerMountSetFileInfo(
            fhDel,
            INVALID_FILE_ATTRIBUTES,   // don't touch attributes
            /*creationTime*/   0,
            /*lastAccessTime*/ 0,
            /*lastWriteTime*/  someValidFileTime,
            /*changeTime*/     0,
            /*allocationSize*/ UINT64_MAX,
            /*fileSize*/       UINT64_MAX,
            &postSet);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"SetFileInfo timestamps on a DELETE-only handle should not "
            L"return ACCESS_DENIED (0xD0000022)");

        ::LayerMountCloseFile(fhDel);
    }

    // Regression: Overwrite issues SetFileInformationByHandle(FileEndOfFileInfo
    // / FileAllocationInfo) against ctx->handle. As with SetInfo, the kernel
    // routes those through whichever handle is open, so an Overwrite arriving
    // via a handle without FILE_WRITE_DATA would fail the bare call with
    // ERROR_ACCESS_DENIED (HRESULT 0xD0000022). The engine must transparently
    // fall back to a transient FILE_WRITE_DATA handle.
    TEST_METHOD(Overwrite_OnInsufficientAccessHandle_DoesNotFailWithAccessDenied) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        // Seed: create the file with full access so the upper-layer entry
        // exists, then close.
        LM_FILE_HANDLE fhSeed = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\overwrite.bin", 0u,
                GENERIC_READ | GENERIC_WRITE | DELETE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fhSeed, &info));

        // Put some bytes in so the truncation is observable.
        const char payload[] = "before-overwrite";
        UINT32 written = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(fhSeed, payload, 0,
                                  static_cast<UINT32>(sizeof(payload) - 1),
                                  FALSE, FALSE, 0u, &written, nullptr));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fhSeed));

        // Reopen with DELETE only — no FILE_WRITE_DATA. Same minimal-access
        // shape that triggered the SetInfo regression; the kernel routes
        // Overwrite's SET_INFORMATION through this handle.
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
            L"Overwrite via a handle without FILE_WRITE_DATA should not "
            L"return ACCESS_DENIED (0xD0000022)");

        ::LayerMountCloseFile(fhDel);
    }

    // -----------------------------------------------------------------
    // Stream enumeration (LayerMountEnumerateStreams)
    // -----------------------------------------------------------------

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
            L"A file with only ::$DATA should report zero user-visible streams");
    }

    TEST_METHOD(EnumerateStreams_NonexistentFile_ReturnsNotFound) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        UINT32 count = 0;
        HRESULT hr = ::LayerMountEnumerateStreams(
            mount.Get(), L"\\nope.txt", nullptr, 0, &count);
        Assert::IsTrue(IsFileNotFoundHr(hr),
            L"Enumerate on a missing file should surface as NOT_FOUND");
        Assert::AreEqual<UINT32>(0u, count);
    }

    TEST_METHOD(EnumerateStreams_FileWithAds_ReturnsAdsEntries) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        // Create the host file via the engine.
        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\host.txt", 0u,
                GENERIC_READ | GENERIC_WRITE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fh, &info));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        // Write two ADS via raw Win32 directly to the upper layer.
        // Stream syntax is `<path>:<streamName>`.
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

        // Size probe.
        UINT32 required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnumerateStreams(mount.Get(), L"\\host.txt",
                nullptr, 0, &required));
        Assert::AreEqual<UINT32>(2u, required,
            L"two user-visible ADS expected (::$DATA filtered)");

        // Fill.
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
            // FindFirstStreamW returns names in the `:name:$DATA` form.
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

        // Buffer of size 1 against required 3 -> ERROR_MORE_DATA, but
        // outCount still reports the required size so the caller can
        // resize and retry.
        LM_STREAM_INFO oneSlot{};
        UINT32 count = 0;
        HRESULT hr = ::LayerMountEnumerateStreams(
            mount.Get(), L"\\multi.txt", &oneSlot, 1, &count);
        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_MORE_DATA), hr);
        Assert::AreEqual<UINT32>(3u, count);
    }
};

} // namespace LayerMountAbiTests
