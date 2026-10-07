#include "pch.h"
#include "AbiTestFixture.h"
#include "StreamTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using LayerMountTestShared::ExtendedPathUnder;
using LayerMountTestShared::HasStream;
using LayerMountTestShared::kMaxNtfsNameLength;

namespace LayerMountAbiTests {

// The stream paths of Create, Open, Overwrite, Delete, Rename and
// UpdateContextPath. The parser tests pin the names TryParseStreamPath rejects.

namespace {

constexpr HRESULT kHrObjectNameInvalid = HRESULT_FROM_NT(STATUS_OBJECT_NAME_INVALID);
constexpr HRESULT kHrInvalidParameter = HRESULT_FROM_NT(STATUS_INVALID_PARAMETER);
constexpr HRESULT kHrFileIsADirectory = HRESULT_FROM_NT(STATUS_FILE_IS_A_DIRECTORY);

HRESULT CreateThroughEngine(LM_HANDLE mount, PCWSTR path) {
    constexpr UINT32 readWriteAccess = GENERIC_READ | GENERIC_WRITE;
    constexpr UINT32 normalFile      = FILE_ATTRIBUTE_NORMAL;
    OpenedFile opened;
    HRESULT hr = CreateOverlayFile(mount, path, readWriteAccess, kNoCreateOptions,
                                   normalFile, opened);
    if (SUCCEEDED(hr)) {
        ::LayerMountCloseFile(opened.handle);
    }
    return hr;
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

constexpr size_t kLongStreamNameLength = 200;
static_assert(kLongStreamNameLength <= kMaxNtfsNameLength);
const std::wstring kLongStreamRelativePath =
    L"host.txt:" + std::wstring(kLongStreamNameLength, L'n');

std::wstring MountPathOf(const std::wstring& relativePath) {
    return L"\\" + relativePath;
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

    TEST_METHOD(Parse_StreamUnderReservedNamespace_RejectedAndOverlayPrefixAccepted) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));

        Assert::AreEqual<HRESULT>(kHrObjectNameInvalid,
            CreateThroughEngine(mount.Get(), L"\\host.txt:overlay.foo"),
            L"a stream under 'overlay.' is reserved, as 'overlay' is");
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt:overlayNotes"),
            L"a stream that only starts with 'overlay' is a user stream");
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

        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\host.txt:secret",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, opened));

        const char payload[] = "stream-content";
        const UINT32 payloadLen = static_cast<UINT32>(sizeof(payload) - 1);
        UINT32 written = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            WriteFromStart(opened.handle, payload, payloadLen, &written, nullptr));
        Assert::AreEqual<UINT32>(payloadLen, written);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

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

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\host.txt:already",
                GENERIC_READ, kNoCreateOptions, opened));

        char buf[16] = {};
        UINT32 read = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ReadFromStart(opened.handle, buf, sizeof(buf), &read));
        Assert::AreEqual<UINT32>(6u, read);
        Assert::IsTrue(std::memcmp(buf, "preset", 6) == 0,
            L"pre-existing stream content must read back");
        ::LayerMountCloseFile(opened.handle);
    }

    TEST_METHOD(CreateFile_StreamTypeSuffix_Accepted) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));

        // host.txt:secret:$DATA is equivalent to host.txt:secret per NTFS.
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt:secret:$DATA"));
        Assert::IsTrue(HasStream(env.Upper() + L"\\host.txt:secret"),
            L":$DATA suffix must round-trip to a plain :secret stream on disk");
    }

    TEST_METHOD(OpenFile_ReadOnly_LowerOnlyHost_NoCopyUp) {
        TempLayerEnv env(1);
        env.WriteLowerFile(0, L"host.txt", "lower content");
        // Pre-create the ADS in the lower layer too.
        WriteRawStream(env.Lower(0) + L"\\host.txt:hidden", "lower-ads", 9);

        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\host.txt:hidden",
                GENERIC_READ, kNoCreateOptions, opened));

        char buf[16] = {};
        UINT32 read = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ReadFromStart(opened.handle, buf, sizeof(buf), &read));
        Assert::AreEqual<UINT32>(9u, read);
        Assert::IsTrue(std::memcmp(buf, "lower-ads", 9) == 0);
        ::LayerMountCloseFile(opened.handle);

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
        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\host.txt:new-stream",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, opened));
        UINT32 addedWritten = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            WriteFromStart(opened.handle, "added", 5, &addedWritten, nullptr));
        Assert::AreEqual<UINT32>(5u, addedWritten);
        ::LayerMountCloseFile(opened.handle);

        // Upper now has the host with BOTH streams.
        DWORD upperAttrs = ::GetFileAttributesW(
            (env.Upper() + L"\\host.txt").c_str());
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES, upperAttrs,
            L"writable stream create must have copied the host up");
        Assert::IsTrue(HasStream(
            env.Upper() + L"\\host.txt:pre-existing"),
            L"existing lower ADS must be preserved through copy-up (no metacopy ADS-drop)");
        Assert::IsTrue(HasStream(
            env.Upper() + L"\\host.txt:new-stream"),
            L"newly created stream must land on the upper host");
    }

    TEST_METHOD(OpenFile_WriteOnLowerFile_CopiesUserStreamNamedOverlayNotes) {
        TempLayerEnv env(1);
        env.WriteLowerFile(0, L"host.txt", "lower content");
        WriteRawStream(env.Lower(0) + L"\\host.txt:overlayNotes", "user notes", 10);

        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\host.txt",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, opened));
        UINT32 written = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            WriteFromStart(opened.handle, "upper", 5, &written, nullptr));
        ::LayerMountCloseFile(opened.handle);

        Assert::AreEqual<std::string>("user notes",
            ReadRawStream(env.Upper() + L"\\host.txt:overlayNotes"),
            L"a write copies up a user stream whose name starts with overlay");
    }

    TEST_METHOD(CreateFile_AdsOnLargeLowerFileWithAStream_SurvivesLaterDataOpen) {
        TempLayerEnv env(1);
        const std::string bigContent(
            static_cast<size_t>(kAboveMetacopyThresholdBytes), 'L');
        env.WriteLowerFile(0, L"big.bin", bigContent);
        WriteRawStream(env.Lower(0) + L"\\big.bin:keep-me",
                       "lower-stream-content", 20);

        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile host;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\big.bin",
                kAttributeOnlyAccess, kNoCreateOptions, host));

        const std::wstring upperHost = env.Upper() + L"\\big.bin";
        Assert::IsTrue(HasStream(upperHost + L":keep-me"),
            L"an attribute-only Open of a lower file with a stream must copy "
            L"the stream up with the file");
        ::LayerMountCloseFile(host.handle);

        OpenedFile userStream;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\big.bin:user-stream",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, userStream));
        UINT32 userWritten = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            WriteFromStart(userStream.handle, "USER-DATA", 9, &userWritten, nullptr));
        Assert::AreEqual<UINT32>(9u, userWritten);
        ::LayerMountCloseFile(userStream.handle);

        Assert::IsTrue(HasStream(upperHost + L":keep-me"),
            L"the lower stream must stay on the upper host after the stream Create");
        Assert::IsTrue(HasStream(upperHost + L":user-stream"),
            L"newly created user stream must be on the upper host");
        Assert::AreEqual<std::string>("USER-DATA",
            ReadRawStream(upperHost + L":user-stream"));

        OpenedFile mainStream;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\big.bin",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, mainStream));
        UINT32 mainWritten = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            WriteFromStart(mainStream.handle, "MAIN", 4, &mainWritten, nullptr));
        Assert::AreEqual<UINT32>(4u, mainWritten);
        ::LayerMountCloseFile(mainStream.handle);

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

        // Open :a with FILE_OVERWRITE_IF semantics. A host adapter can
        // chain this into an Open followed by an Overwrite; this test
        // drives that sequence explicitly to mirror that flow.
        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\host.txt:a",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, opened));
        constexpr UINT64 allocationSize = 0u;
        Assert::AreEqual<HRESULT>(S_OK,
            OverwriteAddingAttributes(opened.handle, FILE_ATTRIBUTE_NORMAL,
                                      allocationSize, nullptr));
        UINT32 tinyWritten = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            WriteFromStart(opened.handle, "tiny", 4, &tinyWritten, nullptr));
        Assert::AreEqual<UINT32>(4u, tinyWritten);
        ::LayerMountCloseFile(opened.handle);

        // :a should now contain "tiny"; :b must be UNTOUCHED.
        Assert::AreEqual<std::string>("tiny",
            ReadRawStream(env.Upper() + L"\\host.txt:a"));
        Assert::AreEqual<std::string>("BBBBBBBBBB",
            ReadRawStream(env.Upper() + L"\\host.txt:b"));
    }

    TEST_METHOD(Overwrite_DeletesUserStreamNamedOverlayNotesAndKeepsReservedStreams) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));
        const std::wstring upperHost = env.Upper() + L"\\host.txt";
        const std::string record = R"({"originLayer":"kept"})";
        ::DeleteFileW((upperHost + L":overlay").c_str());
        WriteRawStream(upperHost + L":overlay", record.data(),
                       static_cast<DWORD>(record.size()));
        WriteRawStream(upperHost + L":Overlay.Extra", "reserved", 8);
        WriteRawStream(upperHost + L":overlayNotes", "user notes", 10);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\host.txt",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, opened));
        constexpr UINT64 allocationSize = 0u;
        Assert::AreEqual<HRESULT>(S_OK,
            OverwriteAddingAttributes(opened.handle, FILE_ATTRIBUTE_NORMAL,
                                      allocationSize, nullptr));
        ::LayerMountCloseFile(opened.handle);

        Assert::IsFalse(HasStream(upperHost + L":overlayNotes"),
            L"Overwrite deletes a user stream whose name starts with overlay");
        Assert::AreEqual<std::string>(record,
            ReadRawStream(upperHost + L":overlay"),
            L"Overwrite keeps the reserved record");
        Assert::AreEqual<std::string>("reserved",
            ReadRawStream(upperHost + L":Overlay.Extra"),
            L"Overwrite keeps a stream under the reserved namespace in any case");
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

        Assert::IsFalse(HasStream(env.Upper() + L"\\host.txt:s1"),
            L":s1 should have been removed");
        Assert::IsTrue(HasStream(env.Upper() + L"\\host.txt:s2"),
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
        Assert::IsTrue(HasStream(env.Lower(0) + L"\\host.txt:hidden"),
            L"lower stream must be untouched by the rejected delete");
    }

    TEST_METHOD(OpenFile_StreamPathLongerThanMaxPath_ReadsContent) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));
        WriteRawStream(ExtendedPathUnder(env.Upper(), kLongStreamRelativePath), "preset", 6);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), MountPathOf(kLongStreamRelativePath).c_str(),
                GENERIC_READ, kNoCreateOptions, opened));
        char buf[16] = {};
        UINT32 read = 0;
        const HRESULT readHr = ReadFromStart(opened.handle, buf, sizeof(buf), &read);
        ::LayerMountCloseFile(opened.handle);

        Assert::AreEqual<HRESULT>(S_OK, readHr);
        Assert::AreEqual<std::string>("preset", std::string(buf, read),
            L"The open of a long stream path reads the stream");
    }

    TEST_METHOD(CreateFile_StreamPathLongerThanMaxPath_RoundTrip) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), MountPathOf(kLongStreamRelativePath).c_str(),
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, opened));
        UINT32 written = 0;
        const HRESULT writeHr = WriteFromStart(opened.handle, "created", 7, &written, nullptr);
        ::LayerMountCloseFile(opened.handle);

        Assert::AreEqual<HRESULT>(S_OK, writeHr);
        Assert::AreEqual<std::string>("created",
            ReadRawStream(ExtendedPathUnder(env.Upper(), kLongStreamRelativePath)),
            L"The create of a long stream path writes the stream on the upper host");
    }

    TEST_METHOD(CreateFile_ExistingLongStreamOfLowerOnlyHost_CollidesWithoutCopyUp) {
        TempLayerEnv env(1);
        env.WriteLowerFile(0, L"host.txt", "lower content");
        WriteRawStream(ExtendedPathUnder(env.Lower(0), kLongStreamRelativePath), "lower", 5);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_OBJECT_NAME_COLLISION),
            CreateThroughEngine(mount.Get(), MountPathOf(kLongStreamRelativePath).c_str()),
            L"The create of a stream that the lower host has collides");
        Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW((env.Upper() + L"\\host.txt").c_str()),
            L"The create that collides copies nothing up");
    }

    TEST_METHOD(OpenFile_WriteToLongStreamOfLowerOnlyHost_WritesTheUpperCopy) {
        TempLayerEnv env(1);
        env.WriteLowerFile(0, L"host.txt", "lower content");
        const std::wstring lowerStreamPath =
            ExtendedPathUnder(env.Lower(0), kLongStreamRelativePath);
        WriteRawStream(lowerStreamPath, "lower", 5);
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), MountPathOf(kLongStreamRelativePath).c_str(),
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, opened));
        UINT32 written = 0;
        const HRESULT writeHr = WriteFromStart(opened.handle, "upper!", 6, &written, nullptr);
        ::LayerMountCloseFile(opened.handle);

        Assert::AreEqual<HRESULT>(S_OK, writeHr);
        Assert::AreEqual<std::string>("upper!",
            ReadRawStream(ExtendedPathUnder(env.Upper(), kLongStreamRelativePath)),
            L"The write through a long stream of a lower file lands in the upper copy");
        Assert::AreEqual<std::string>("lower", ReadRawStream(lowerStreamPath),
            L"The write through a long stream of a lower file leaves the lower stream");
    }

    TEST_METHOD(Overwrite_OnHostWithStreamPathLongerThanMaxPath_DeletesTheStream) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));
        const std::wstring streamPath = ExtendedPathUnder(env.Upper(), kLongStreamRelativePath);
        WriteRawStream(streamPath, "user", 4);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\host.txt",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, opened));
        constexpr UINT64 allocationSize = 0u;
        const HRESULT overwriteHr = OverwriteAddingAttributes(
            opened.handle, FILE_ATTRIBUTE_NORMAL, allocationSize, nullptr);
        ::LayerMountCloseFile(opened.handle);

        Assert::AreEqual<HRESULT>(S_OK, overwriteHr);
        Assert::IsFalse(HasStream(streamPath),
            L"The overwrite of the host deletes its long stream");
    }

    TEST_METHOD(Overwrite_OnStreamHandleWithPathLongerThanMaxPath_TruncatesOnlyThatStream) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));
        WriteRawStream(ExtendedPathUnder(env.Upper(), kLongStreamRelativePath), "AAAAAAAA", 8);
        WriteRawStream(env.Upper() + L"\\host.txt:b", "BBBBBBBBBB", 10);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), MountPathOf(kLongStreamRelativePath).c_str(),
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, opened));
        constexpr UINT64 allocationSize = 0u;
        const HRESULT overwriteHr = OverwriteAddingAttributes(
            opened.handle, FILE_ATTRIBUTE_NORMAL, allocationSize, nullptr);
        ::LayerMountCloseFile(opened.handle);

        Assert::AreEqual<HRESULT>(S_OK, overwriteHr);
        Assert::AreEqual<std::string>("",
            ReadRawStream(ExtendedPathUnder(env.Upper(), kLongStreamRelativePath)),
            L"The overwrite of the long stream truncates it");
        Assert::AreEqual<std::string>("BBBBBBBBBB",
            ReadRawStream(env.Upper() + L"\\host.txt:b"),
            L"The overwrite of the long stream keeps the other stream");
    }

    TEST_METHOD(DeleteFile_StreamPathLongerThanMaxPath_RemovesOnlyTheStream) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));
        const std::wstring streamPath = ExtendedPathUnder(env.Upper(), kLongStreamRelativePath);
        WriteRawStream(streamPath, "user", 4);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteFile(mount.Get(), MountPathOf(kLongStreamRelativePath).c_str()));

        Assert::IsFalse(HasStream(streamPath),
            L"The delete by name removes the long stream");
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW((env.Upper() + L"\\host.txt").c_str()),
            L"The delete of a stream keeps its host file");
    }

    TEST_METHOD(Rename_StreamQualifiedSrcOrDst_ReturnsInvalidParameter) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        Assert::AreEqual<HRESULT>(S_OK,
            CreateThroughEngine(mount.Get(), L"\\host.txt"));
        WriteRawStream(env.Upper() + L"\\host.txt:s1", "one", 3);
        constexpr BOOL replaceIfExists = FALSE;

        // Stream-qualified source.
        Assert::AreEqual<HRESULT>(kHrInvalidParameter,
            ::LayerMountRenameFile(mount.Get(),
                L"\\host.txt:s1", L"\\host.txt:s2", replaceIfExists),
            L"rename with stream-qualified source must return INVALID_PARAMETER");

        // Stream-qualified destination.
        Assert::AreEqual<HRESULT>(kHrInvalidParameter,
            ::LayerMountRenameFile(mount.Get(),
                L"\\host.txt", L"\\host.txt:streamname", replaceIfExists),
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
        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\host.txt:s1",
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, opened));

        // Engine-level rename of the host file. (We close the stream
        // handle's underlying NT handle here by going through
        // UpdateOpenFilePath, which closes + marks for reopen, so the
        // rename below isn't blocked by a sharing conflict on s1.)
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountUpdateOpenFilePath(opened.handle, L"\\host2.txt:s1"),
            L"UpdateOpenFilePath must accept a stream-qualified rebind");
        constexpr BOOL replaceIfExists = FALSE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(),
                L"\\host.txt", L"\\host2.txt", replaceIfExists),
            L"host rename must succeed even with an open stream handle");

        // After the rename + rebind, write to the open handle and confirm
        // the new physical path carries the content.
        UINT32 postWritten = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            WriteFromStart(opened.handle, "post", 4, &postWritten, nullptr));
        Assert::AreEqual<UINT32>(4u, postWritten);
        ::LayerMountCloseFile(opened.handle);

        Assert::AreEqual<std::string>("post",
            ReadRawStream(env.Upper() + L"\\host2.txt:s1"),
            L"open stream handle must follow the host through the rename");
        Assert::IsFalse(HasStream(env.Upper() + L"\\host.txt:s1"),
            L"old path must no longer carry the stream");
    }

    TEST_METHOD(CreateFile_AdsOnDirectory_Rejected) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        // Create a directory through the engine.
        OpenedFile directory;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), L"\\subdir",
                GENERIC_READ | GENERIC_WRITE, FILE_DIRECTORY_FILE,
                FILE_ATTRIBUTE_DIRECTORY, directory));
        ::LayerMountCloseFile(directory.handle);

        Assert::AreEqual<HRESULT>(kHrFileIsADirectory,
            CreateThroughEngine(mount.Get(), L"\\subdir:s"),
            L"ADS on a directory must be rejected with STATUS_FILE_IS_A_DIRECTORY");

        // OpenFile on the same path takes the equivalent reject branch.
        OpenedFile opened;
        HRESULT hrOpen = OpenOverlayFile(mount.Get(), L"\\subdir:s",
            GENERIC_READ, kNoCreateOptions, opened);
        Assert::AreEqual<HRESULT>(kHrFileIsADirectory, hrOpen,
            L"Open of ADS on a directory must also be rejected");
    }
};

} // namespace LayerMountAbiTests
