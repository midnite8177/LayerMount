#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// ADS write-path coverage. These tests exercise the stream-aware code paths
// added to Create / Open / Overwrite / Delete / Rename / UpdateContextPath
// after the EnumerateStreams (read-side) work shipped. The parser tests
// pin TryParseStreamPath's rejects so future loosening of the validator is
// caught at the ABI surface.

namespace {

// HRESULT_FROM_NT(STATUS_OBJECT_NAME_INVALID) == 0xD0000033
constexpr HRESULT kHrObjectNameInvalid =
    static_cast<HRESULT>(0xD0000033L);
// HRESULT_FROM_NT(STATUS_INVALID_PARAMETER) == 0xD000000D
constexpr HRESULT kHrInvalidParameter =
    static_cast<HRESULT>(0xD000000DL);
// HRESULT_FROM_NT(STATUS_FILE_IS_A_DIRECTORY) == 0xD00000BA
constexpr HRESULT kHrFileIsADirectory =
    static_cast<HRESULT>(0xD00000BAL);

HRESULT CreateThroughEngine(LM_HANDLE mount, PCWSTR path,
                            UINT32 access = GENERIC_READ | GENERIC_WRITE,
                            UINT32 createOptions = 0u,
                            UINT32 attrs = FILE_ATTRIBUTE_NORMAL) {
    LM_FILE_HANDLE fh = nullptr;
    LM_FILE_INFO   info{};
    HRESULT hr = ::LayerMountCreateFile(mount, path,
        createOptions, access, attrs,
        /*sd*/ nullptr, /*sdBytes*/ 0u, /*allocationSize*/ 0u,
        /*originatorPid*/ 0u, &fh, &info);
    if (SUCCEEDED(hr)) {
        ::LayerMountCloseFile(fh);
    }
    return hr;
}

void WriteRawStream(const std::wstring& path, const char* data, DWORD len) {
    HANDLE h = ::CreateFileW(path.c_str(),
        GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Assert::IsTrue(h != INVALID_HANDLE_VALUE,
        L"raw CreateFileW for stream write should succeed");
    DWORD written = 0;
    ::WriteFile(h, data, len, &written, nullptr);
    ::CloseHandle(h);
    Assert::AreEqual<DWORD>(len, written, L"WriteFile short-write");
}

std::string ReadRawStream(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Assert::IsTrue(h != INVALID_HANDLE_VALUE,
        L"raw CreateFileW for stream read should succeed");
    char buf[1024]{};
    DWORD read = 0;
    ::ReadFile(h, buf, sizeof(buf), &read, nullptr);
    ::CloseHandle(h);
    return std::string(buf, read);
}

bool StreamExistsOnDisk(const std::wstring& fullStreamPath) {
    HANDLE h = ::CreateFileW(fullStreamPath.c_str(),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

} // namespace

TEST_CLASS(AbiAdsWriteTests) {
public:
    // -----------------------------------------------------------------
    // Parser rejects -- single-pass, no FS mutation needed.
    // -----------------------------------------------------------------

    TEST_METHOD(Parse_EmptyHostBeforeColon_Rejected) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(kHrObjectNameInvalid,
            CreateThroughEngine(mount.Get(), L"\\:rogue"),
            L"path with empty host before colon must be rejected");
    }

    TEST_METHOD(Parse_DotDotEscapeInHost_Rejected) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(kHrObjectNameInvalid,
            CreateThroughEngine(mount.Get(), L"\\..\\escape:secret"),
            L"traversal in host portion must be rejected");
    }

    TEST_METHOD(Parse_ReservedStreamName_Rejected) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(kHrObjectNameInvalid,
            CreateThroughEngine(mount.Get(), L"\\host:overlay"),
            L"reserved stream name 'overlay' must be rejected");
        Assert::AreEqual<HRESULT>(kHrObjectNameInvalid,
            CreateThroughEngine(mount.Get(), L"\\host:overlay.opaque"),
            L"reserved stream name 'overlay.opaque' must be rejected");
    }

    TEST_METHOD(Parse_BackslashInStreamName_Rejected) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(kHrObjectNameInvalid,
            CreateThroughEngine(mount.Get(), L"\\host:stream\\nested"),
            L"backslash inside stream name must be rejected");
    }

    TEST_METHOD(Parse_EmptyStreamName_Rejected) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(kHrObjectNameInvalid,
            CreateThroughEngine(mount.Get(), L"\\host:"),
            L"empty stream name (`host:`) must be rejected");
    }

    TEST_METHOD(Parse_EmptyTypeName_Rejected) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(kHrObjectNameInvalid,
            CreateThroughEngine(mount.Get(), L"\\host:stream:"),
            L"empty stream-type after second colon must be rejected");
    }

    TEST_METHOD(Parse_NonDataStreamType_Rejected) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(kHrObjectNameInvalid,
            CreateThroughEngine(mount.Get(), L"\\host:stream:$INDEX_ALLOCATION"),
            L"only :$DATA stream-type is accepted");
    }

    TEST_METHOD(Parse_ExtraColons_Rejected) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(kHrObjectNameInvalid,
            CreateThroughEngine(mount.Get(), L"\\host:stream:$DATA:extra"),
            L"a third colon must be rejected");
    }

    // -----------------------------------------------------------------
    // Functional: write / read / delete an ADS through the engine.
    // -----------------------------------------------------------------

    TEST_METHOD(CreateFile_AdsOnExistingHost_RoundTrip) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        // Host first.
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));

        // Stream via the engine.
        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\host.txt:secret", 0u,
                GENERIC_READ | GENERIC_WRITE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fh, &info));

        const char payload[] = "stream-content";
        const UINT32 payloadLen = static_cast<UINT32>(sizeof(payload) - 1);
        UINT32 written = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(fh, payload, 0, payloadLen,
                FALSE, FALSE, 0u, &written, nullptr));
        Assert::AreEqual<UINT32>(payloadLen, written);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        // Read back via raw Win32 to confirm the stream landed on the
        // upper-layer host file.
        const std::string got =
            ReadRawStream(env.Upper() + L"\\host.txt:secret");
        Assert::AreEqual<size_t>(payloadLen, got.size());
        Assert::IsTrue(std::memcmp(got.data(), payload, payloadLen) == 0,
            L"stream content must match");
    }

    TEST_METHOD(OpenFile_PreExistingAds_ReadsContent) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));
        WriteRawStream(env.Upper() + L"\\host.txt:already", "preset", 6);

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\host.txt:already",
                GENERIC_READ, 0u, 0u, &fh, &info));

        char buf[16] = {};
        UINT32 read = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountReadFile(fh, buf, 0, sizeof(buf), 0u, &read));
        Assert::AreEqual<UINT32>(6u, read);
        Assert::IsTrue(std::memcmp(buf, "preset", 6) == 0,
            L"pre-existing stream content must read back");
        ::LayerMountCloseFile(fh);
    }

    TEST_METHOD(CreateFile_StreamTypeSuffix_Accepted) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));

        // host.txt:secret:$DATA is equivalent to host.txt:secret per NTFS.
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt:secret:$DATA"));
        Assert::IsTrue(StreamExistsOnDisk(env.Upper() + L"\\host.txt:secret"),
            L":$DATA suffix must round-trip to a plain :secret stream on disk");
    }

    TEST_METHOD(OpenFile_ReadOnly_LowerOnlyHost_NoCopyUp) {
        TempLayerEnv env(1);
        env.WriteLowerFile(0, L"host.txt", "lower content");
        // Pre-create the ADS in the lower layer too.
        WriteRawStream(env.Lower(0) + L"\\host.txt:hidden", "lower-ads", 9);

        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\host.txt:hidden",
                GENERIC_READ, 0u, 0u, &fh, &info));

        char buf[16] = {};
        UINT32 read = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountReadFile(fh, buf, 0, sizeof(buf), 0u, &read));
        Assert::AreEqual<UINT32>(9u, read);
        Assert::IsTrue(std::memcmp(buf, "lower-ads", 9) == 0);
        ::LayerMountCloseFile(fh);

        // Read-only opens against a lower-only host must NOT trigger
        // copy-up. The host must still live only in lower.
        DWORD upperAttrs = ::GetFileAttributesW(
            (env.Upper() + L"\\host.txt").c_str());
        Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES, upperAttrs,
            L"read-only stream open on lower-only host must not copy-up");
    }

    TEST_METHOD(CreateFile_AdsOnLowerOnlyHost_TriggersFullCopyUp) {
        TempLayerEnv env(1);
        env.WriteLowerFile(0, L"host.txt", "lower content");
        // Pre-existing ADS in the lower layer that must survive copy-up.
        WriteRawStream(env.Lower(0) + L"\\host.txt:pre-existing",
                        "preserve-me", 11);

        LayerMountHolder mount = CreateLayerMount(env);

        // Writable stream create on a lower-only host: forces a full
        // copy-up (never metacopy) so the lower ADS is preserved.
        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\host.txt:new-stream", 0u,
                GENERIC_READ | GENERIC_WRITE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fh, &info));
        UINT32 addedWritten = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(fh, "added", 0, 5,
                FALSE, FALSE, 0u, &addedWritten, nullptr));
        Assert::AreEqual<UINT32>(5u, addedWritten);
        ::LayerMountCloseFile(fh);

        // Upper now has the host with BOTH streams.
        DWORD upperAttrs = ::GetFileAttributesW(
            (env.Upper() + L"\\host.txt").c_str());
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES, upperAttrs,
            L"writable stream create must have copied the host up");
        Assert::IsTrue(StreamExistsOnDisk(
            env.Upper() + L"\\host.txt:pre-existing"),
            L"existing lower ADS must be preserved through copy-up (no metacopy ADS-drop)");
        Assert::IsTrue(StreamExistsOnDisk(
            env.Upper() + L"\\host.txt:new-stream"),
            L"newly created stream must land on the upper host");
    }

    TEST_METHOD(CreateFile_AdsOnMetacopyShell_SurvivesLazyCompletion) {
        // A metacopy upper shell satisfies ExistsInUpper, so an unguarded
        // stream Create would attach to the sparse skeleton. Later, when
        // any main-stream read/write through the host handle triggers
        // CompleteLazyCopyUp, lower's ADS would be copied onto upper -- a
        // collision with the user's stream silently overwrites the user's
        // bytes with lower content. The fix is to finalize the metacopy
        // *before* attaching the new stream, so lower's ADS are visible
        // immediately and a colliding user stream surfaces a CREATE_NEW
        // collision instead of a silent overwrite.
        //
        // Test shape: lower host > 1 MB so writable Open stages a metacopy
        // shell on upper; pre-existing lower stream :keep-me; user creates
        // a non-colliding stream :user-stream. The load-bearing assertion
        // is that :keep-me appears on upper RIGHT AFTER the stream Create
        // (proving CompleteLazyCopyUp ran during Create, not deferred to
        // the later main-stream write).
        TempLayerEnv env(1);
        const std::string bigContent(2u * 1024u * 1024u, 'L');
        env.WriteLowerFile(0, L"big.bin", bigContent);
        WriteRawStream(env.Lower(0) + L"\\big.bin:keep-me",
                       "lower-stream-content", 20);

        LayerMountHolder mount = CreateLayerMount(env);

        // Open the host writable to stage a metacopy shell on upper. The
        // engine's metacopy threshold is 1 MB with sparse-file capability
        // on; the 2 MB lower file is comfortably over it.
        //
        // Use FILE_GENERIC_READ / FILE_GENERIC_WRITE rather than the
        // GENERIC_* aliases: the engine's HasWriteAccess() checks specific
        // bits (FILE_WRITE_DATA etc.), and GENERIC_WRITE alone wouldn't
        // trigger the lower-source copy-up path.
        LM_FILE_HANDLE hostFh = nullptr;
        LM_FILE_INFO   hostInfo{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\big.bin",
                FILE_GENERIC_READ | FILE_GENERIC_WRITE, 0u, 0u,
                &hostFh, &hostInfo));

        const std::wstring upperHost = env.Upper() + L"\\big.bin";
        Assert::IsTrue(std::filesystem::exists(upperHost),
            L"writable Open of a > 1 MB lower file should stage a metacopy "
            L"shell on upper");
        // Sanity: pre-stream-Create, the lower-only :keep-me should NOT
        // have been carried up yet (still a metacopy shell).
        Assert::IsFalse(StreamExistsOnDisk(upperHost + L":keep-me"),
            L":keep-me must not be on upper yet -- metacopy shell only");

        // Create a non-colliding stream on the metacopy upper. The fix
        // finalizes the metacopy synchronously here.
        LM_FILE_HANDLE streamFh = nullptr;
        LM_FILE_INFO   streamInfo{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\big.bin:user-stream", 0u,
                GENERIC_READ | GENERIC_WRITE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u,
                &streamFh, &streamInfo));
        UINT32 userWritten = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(streamFh, "USER-DATA", 0, 9,
                FALSE, FALSE, 0u, &userWritten, nullptr));
        Assert::AreEqual<UINT32>(9u, userWritten);
        ::LayerMountCloseFile(streamFh);

        // Load-bearing: :keep-me must now be on upper. Without the fix,
        // lower ADS would not be copied until the main-stream write below
        // triggers CompleteLazyCopyUp; with the fix, the Create above did
        // it. This is the assertion that pins the timing-change.
        Assert::IsTrue(StreamExistsOnDisk(upperHost + L":keep-me"),
            L"lower ADS must be carried up DURING the stream Create, not "
            L"deferred to a later main-stream write");
        Assert::IsTrue(StreamExistsOnDisk(upperHost + L":user-stream"),
            L"newly created user stream must be on the upper host");
        Assert::AreEqual<std::string>("USER-DATA",
            ReadRawStream(upperHost + L":user-stream"));

        // Drive a main-stream write through the original host handle. The
        // host context still has isMetacopyOnly = true from the original
        // Open; the engine's main-stream-write path will call
        // CompleteLazyCopyUp again, which short-circuits because the
        // metacopy flag has already been cleared. The user stream must
        // survive intact across this second pass.
        UINT32 mainWritten = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(hostFh, "MAIN", 0, 4,
                FALSE, FALSE, 0u, &mainWritten, nullptr));
        Assert::AreEqual<UINT32>(4u, mainWritten);
        ::LayerMountCloseFile(hostFh);

        Assert::AreEqual<std::string>("USER-DATA",
            ReadRawStream(upperHost + L":user-stream"),
            L":user-stream must still contain user data after main-stream write");
        Assert::AreEqual<std::string>("lower-stream-content",
            ReadRawStream(upperHost + L":keep-me"),
            L":keep-me must retain lower content (no double-completion overwrite)");
    }

    TEST_METHOD(Overwrite_OnStreamHandle_PreservesOtherStreams) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));
        WriteRawStream(env.Upper() + L"\\host.txt:a", "AAAAAAAA", 8);
        WriteRawStream(env.Upper() + L"\\host.txt:b", "BBBBBBBBBB", 10);

        // Open :a with FILE_OVERWRITE_IF semantics (CBFS would route this
        // to Open + Overwrite). The host adapter chains these; we drive
        // Open then Overwrite explicitly to mirror that flow.
        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\host.txt:a",
                GENERIC_READ | GENERIC_WRITE, 0u, 0u, &fh, &info));
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOverwriteFile(fh, FILE_ATTRIBUTE_NORMAL,
                /*replaceAttributes*/ FALSE, /*allocationSize*/ 0u,
                /*originatorPid*/ 0u, nullptr));
        UINT32 tinyWritten = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(fh, "tiny", 0, 4,
                FALSE, FALSE, 0u, &tinyWritten, nullptr));
        Assert::AreEqual<UINT32>(4u, tinyWritten);
        ::LayerMountCloseFile(fh);

        // :a should now contain "tiny"; :b must be UNTOUCHED.
        Assert::AreEqual<std::string>("tiny",
            ReadRawStream(env.Upper() + L"\\host.txt:a"));
        Assert::AreEqual<std::string>("BBBBBBBBBB",
            ReadRawStream(env.Upper() + L"\\host.txt:b"));
    }

    TEST_METHOD(DeleteFile_StreamOnly_LeavesHost) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));
        WriteRawStream(env.Upper() + L"\\host.txt:s1", "one", 3);
        WriteRawStream(env.Upper() + L"\\host.txt:s2", "two", 3);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteFile(mount.Get(), L"\\host.txt:s1"));

        Assert::IsFalse(StreamExistsOnDisk(env.Upper() + L"\\host.txt:s1"),
            L":s1 should have been removed");
        Assert::IsTrue(StreamExistsOnDisk(env.Upper() + L"\\host.txt:s2"),
            L":s2 must remain after deleting :s1");
        DWORD hostAttrs = ::GetFileAttributesW(
            (env.Upper() + L"\\host.txt").c_str());
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES, hostAttrs,
            L"host file must remain after deleting a single stream");
    }

    TEST_METHOD(DeleteFile_StreamOnLowerOnlyHost_NotFound) {
        TempLayerEnv env(1);
        env.WriteLowerFile(0, L"host.txt", "lower content");
        WriteRawStream(env.Lower(0) + L"\\host.txt:hidden", "ads", 3);
        LayerMountHolder mount = CreateLayerMount(env);

        HRESULT hr = ::LayerMountDeleteFile(mount.Get(), L"\\host.txt:hidden");
        Assert::IsTrue(IsFileNotFoundHr(hr),
            L"stream delete on a lower-only host should surface as NotFound");
        // Confirm the lower stream is untouched.
        Assert::IsTrue(StreamExistsOnDisk(env.Lower(0) + L"\\host.txt:hidden"),
            L"lower stream must be untouched by the rejected delete");
    }

    TEST_METHOD(Rename_StreamQualifiedSrcOrDst_ReturnsInvalidParameter) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));
        WriteRawStream(env.Upper() + L"\\host.txt:s1", "one", 3);

        // Stream-qualified source.
        Assert::AreEqual<HRESULT>(kHrInvalidParameter,
            ::LayerMountRenameFile(mount.Get(),
                L"\\host.txt:s1", L"\\host.txt:s2", FALSE),
            L"rename with stream-qualified source must return INVALID_PARAMETER");

        // Stream-qualified destination.
        Assert::AreEqual<HRESULT>(kHrInvalidParameter,
            ::LayerMountRenameFile(mount.Get(),
                L"\\host.txt", L"\\host.txt:streamname", FALSE),
            L"rename with stream-qualified destination must return INVALID_PARAMETER");
    }

    TEST_METHOD(Rename_HostWithOpenStreamHandle_Succeeds) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));
        WriteRawStream(env.Upper() + L"\\host.txt:s1", "open", 4);

        // Open the stream, then rebind its path through UpdateOpenFilePath
        // to simulate a host that just renamed `\host.txt -> \host2.txt`
        // and is walking its open-handle table.
        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\host.txt:s1",
                GENERIC_READ | GENERIC_WRITE, 0u, 0u, &fh, &info));

        // Engine-level rename of the host file. (We close the stream
        // handle's underlying NT handle here by going through
        // UpdateOpenFilePath, which closes + marks for reopen, so the
        // rename below isn't blocked by a sharing conflict on s1.)
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountUpdateOpenFilePath(fh, L"\\host2.txt:s1"),
            L"UpdateOpenFilePath must accept a stream-qualified rebind");
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(),
                L"\\host.txt", L"\\host2.txt", FALSE),
            L"host rename must succeed even with an open stream handle");

        // After the rename + rebind, write to the open handle and confirm
        // the new physical path carries the content.
        UINT32 postWritten = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(fh, "post", 0, 4,
                FALSE, FALSE, 0u, &postWritten, nullptr));
        Assert::AreEqual<UINT32>(4u, postWritten);
        ::LayerMountCloseFile(fh);

        Assert::AreEqual<std::string>("post",
            ReadRawStream(env.Upper() + L"\\host2.txt:s1"),
            L"open stream handle must follow the host through the rename");
        Assert::IsFalse(StreamExistsOnDisk(env.Upper() + L"\\host.txt:s1"),
            L"old path must no longer carry the stream");
    }

    TEST_METHOD(CreateFile_AdsOnDirectory_Rejected) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        // Create a directory through the engine.
        LM_FILE_HANDLE dh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\subdir",
                FILE_DIRECTORY_FILE,
                GENERIC_READ | GENERIC_WRITE,
                FILE_ATTRIBUTE_DIRECTORY,
                nullptr, 0u, 0u, 0u, &dh, &info));
        ::LayerMountCloseFile(dh);

        // CreateFile with a stream qualifier against a directory.
        Assert::AreEqual<HRESULT>(kHrFileIsADirectory,
            CreateThroughEngine(mount.Get(), L"\\subdir:s"),
            L"ADS on a directory must be rejected with STATUS_FILE_IS_A_DIRECTORY");

        // OpenFile on the same path takes the equivalent reject branch.
        LM_FILE_HANDLE fh = nullptr;
        HRESULT hrOpen = ::LayerMountOpenFile(mount.Get(), L"\\subdir:s",
            GENERIC_READ, 0u, 0u, &fh, &info);
        Assert::AreEqual<HRESULT>(kHrFileIsADirectory, hrOpen,
            L"Open of ADS on a directory must also be rejected");
    }
};

} // namespace LayerMountAbiTests
