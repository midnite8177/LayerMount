// Audit-driven coverage of properties that must survive the two-stage
// metacopy → lazy-completion flow. CopyUp::CopyUpMetadataOnly stages a
// sparse shell that preserves timestamps and size. CopyUp::CompleteLazyCopyUp
// then streams data in on first read/write. The full-copy-up path
// (CopyUp::CopyUpFile) takes care to copy ADS and re-apply timestamps after
// the data write. The lazy path does not — three gaps result:
//
//   Gap #7 (timestamps): CompleteLazyCopyUp ends with
//   MetadataADS::WriteLayerMountMetadata (CopyUp.cpp:593-594) and does NOT
//   restore source timestamps afterwards. LastWriteTime is stomped by the
//   data write plus the subsequent ADS write.
//
//   Gap #8 (ADS): CopyUpMetadataOnly never calls
//   CopyUserAlternateDataStreams, and CompleteLazyCopyUp doesn't either, so
//   a lower file that carried user ADS loses them entirely through the
//   lazy path while the eager path preserves them (CopyUp.cpp:401).
//
//   Gap #9 (compression): CopyUpMetadataOnly creates a sparse shell with
//   CreateFileW but never applies FSCTL_SET_COMPRESSION, so a compressed
//   lower file ends up uncompressed in upper.
//
// All three tests call CopyUp directly (like CopyUpFailureTests) for
// deterministic probing of these known-buggy paths.

#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataADS.h"
#include "LayerMount.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

namespace {

// Force three distinct timestamps so a bug that zeroes ANY of them is
// caught. Year 2015 for creation, 2016 for access, 2017 for write.
// Cast to 64-bit dwHighDateTime explicitly to dodge warning.
void StampFile(const std::wstring& path,
                const FILETIME& creation,
                const FILETIME& access,
                const FILETIME& write) {
    HANDLE h = ::CreateFileW(path.c_str(),
                              FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, h,
        L"StampFile: CreateFileW must succeed");
    ::SetFileTime(h, &creation, &access, &write);
    ::CloseHandle(h);
}

void GetTimes(const std::wstring& path,
              FILETIME* creation, FILETIME* access, FILETIME* write) {
    HANDLE h = ::CreateFileW(path.c_str(),
                              FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, h);
    ::GetFileTime(h, creation, access, write);
    ::CloseHandle(h);
}

bool FileTimesEqual(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime &&
           a.dwHighDateTime == b.dwHighDateTime;
}

void WriteADS(const std::wstring& basePath, const std::wstring& streamName,
              const std::string& content) {
    const std::wstring adsPath = basePath + L":" + streamName;
    HANDLE h = ::CreateFileW(adsPath.c_str(),
                              GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, h,
        L"WriteADS: CreateFileW on :stream must succeed");
    DWORD w = 0;
    ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()),
                &w, nullptr);
    ::CloseHandle(h);
}

bool ADSExists(const std::wstring& basePath, const std::wstring& streamName) {
    const std::wstring adsPath = basePath + L":" + streamName;
    HANDLE h = ::CreateFileW(adsPath.c_str(),
                              GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

bool IsCompressed(const std::wstring& path) {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_COMPRESSED) != 0;
}

bool EnableCompression(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    USHORT fmt = COMPRESSION_FORMAT_DEFAULT;
    DWORD br = 0;
    const BOOL ok = ::DeviceIoControl(h, FSCTL_SET_COMPRESSION,
                                       &fmt, sizeof(fmt),
                                       nullptr, 0, &br, nullptr);
    ::CloseHandle(h);
    return ok != FALSE;
}

FILETIME MakeFileTime(WORD year, WORD month, WORD day) {
    SYSTEMTIME st{};
    st.wYear = year;
    st.wMonth = month;
    st.wDay = day;
    st.wHour = 12;
    FILETIME ft{};
    ::SystemTimeToFileTime(&st, &ft);
    return ft;
}

} // namespace

// ============================================================================
// LazyMetacopyFidelityTests — properties that must survive metacopy + lazy
// completion on oversized files.
// ============================================================================

TEST_CLASS(LazyMetacopyFidelityTests) {
public:
    // ------------------------------------------------------------------------
    // Guards the CompleteLazyCopyUp timestamp-preservation fix. After the
    // completer writes data and rewrites :overlay, it re-applies the
    // captured source timestamps so the upper file looks byte-for-byte
    // equivalent to the lower source — same invariant CopyUpFile provides
    // for the eager path.
    // ------------------------------------------------------------------------
    TEST_METHOD(LazyCompletion_PreservesSourceTimestamps) {
        // 2 MiB: above the 1 MiB metacopy threshold.
        LayerMountTests::TempLayerEnvironment env(1);
        const std::string payload(2 * 1024 * 1024, 'Z');
        env.WriteFile(env.Lower(0), L"ts.bin", payload);

        const std::wstring srcPath = env.Lower(0) + L"\\ts.bin";
        const FILETIME srcCreation = MakeFileTime(2015, 6, 15);
        const FILETIME srcAccess   = MakeFileTime(2016, 6, 15);
        const FILETIME srcWrite    = MakeFileTime(2017, 6, 15);
        StampFile(srcPath, srcCreation, srcAccess, srcWrite);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpMetadataOnly(L"ts.bin")));

        const std::wstring upperPath = env.Upper() + L"\\ts.bin";

        // Pre-completion: the metacopy shell has the source timestamps.
        {
            FILETIME c{}, a{}, w{};
            GetTimes(upperPath, &c, &a, &w);
            Assert::IsTrue(FileTimesEqual(c, srcCreation),
                L"Metacopy shell must preserve source creation time");
            Assert::IsTrue(FileTimesEqual(w, srcWrite),
                L"Metacopy shell must preserve source write time");
        }

        // Trigger lazy completion.
        Assert::IsTrue(NT_SUCCESS(cu.CompleteLazyCopyUp(L"ts.bin")));

        FILETIME c{}, a{}, w{};
        GetTimes(upperPath, &c, &a, &w);

        // LazyCompletion restores all three source timestamps after the
        // data write + ADS rewrite. Compare write time as the canonical
        // check — it's the one most-often bumped by intermediate writes.
        Assert::IsTrue(FileTimesEqual(w, srcWrite),
            L"Upper LastWriteTime must match the source after lazy "
            L"completion, matching the eager-path invariant");
        Assert::IsTrue(FileTimesEqual(c, srcCreation),
            L"Upper creation time must match the source after lazy "
            L"completion");
    }

    // ------------------------------------------------------------------------
    // Guards the CompleteLazyCopyUp ADS-preservation fix. After data copy,
    // the completer calls CopyUserAlternateDataStreams(origin, upper) so
    // Zone.Identifier and custom ADS survive — matching the eager path.
    // ------------------------------------------------------------------------
    TEST_METHOD(LazyCompletion_PreservesUserADS) {
        UNIT_SKIP_IF_NOT_NTFS();

        LayerMountTests::TempLayerEnvironment env(1);
        const std::string payload(2 * 1024 * 1024, 'L');
        env.WriteFile(env.Lower(0), L"ads.bin", payload);

        const std::wstring srcPath = env.Lower(0) + L"\\ads.bin";
        WriteADS(srcPath, L"Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        WriteADS(srcPath, L"custom",          "user-metadata-payload");

        Assert::IsTrue(ADSExists(srcPath, L"Zone.Identifier"),
            L"Preconditions: source ADS must be present");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpMetadataOnly(L"ads.bin")));
        Assert::IsTrue(NT_SUCCESS(cu.CompleteLazyCopyUp(L"ads.bin")));

        const std::wstring upperPath = env.Upper() + L"\\ads.bin";

        // The overlay's own :overlay stream should exist (metacopy cleared).
        LayerMountMetadata md = MetadataADS::ReadLayerMountMetadata(upperPath);
        Assert::IsFalse(md.metacopy,
            L"metacopy flag must clear after successful lazy completion");

        // User ADS carried through the lazy path.
        Assert::IsTrue(ADSExists(upperPath, L"Zone.Identifier"),
            L"Zone.Identifier must survive lazy metacopy + completion");
        Assert::IsTrue(ADSExists(upperPath, L"custom"),
            L"Custom user ADS must survive lazy metacopy + completion");
    }

    // ------------------------------------------------------------------------
    // Guards the CopyUpMetadataOnly compression fix. The metacopy shell is
    // created with FSCTL_SET_COMPRESSION when the source was compressed, so
    // the compression attribute survives lazy completion.
    // ------------------------------------------------------------------------
    TEST_METHOD(LazyCompletion_PreservesCompression) {
        UNIT_SKIP_IF_NOT_NTFS();

        LayerMountTests::TempLayerEnvironment env(1);
        // Highly-compressible payload so the attribute actually applies
        // meaningful compression (avoids NTFS deciding it can't help).
        const std::string payload(2 * 1024 * 1024, 'c');
        env.WriteFile(env.Lower(0), L"cmp.bin", payload);

        const std::wstring srcPath = env.Lower(0) + L"\\cmp.bin";
        if (!EnableCompression(srcPath)) {
            Logger::WriteMessage(
                L"[SKIP] NTFS refused FSCTL_SET_COMPRESSION on source — "
                L"volume likely does not support compression");
            return;
        }
        Assert::IsTrue(IsCompressed(srcPath),
            L"Preconditions: source file must report FILE_ATTRIBUTE_COMPRESSED");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpMetadataOnly(L"cmp.bin")));
        Assert::IsTrue(NT_SUCCESS(cu.CompleteLazyCopyUp(L"cmp.bin")));

        const std::wstring upperPath = env.Upper() + L"\\cmp.bin";
        Assert::IsTrue(IsCompressed(upperPath),
            L"Compressed lower file must remain compressed in upper after "
            L"metacopy + lazy completion");
    }

    // ------------------------------------------------------------------------
    // Control case: the EAGER path (CopyUpFile) preserves all three
    // properties. Demonstrates the gaps are LAZY-specific, not a general
    // copy-up deficiency — and guards against regression in the eager path.
    // ------------------------------------------------------------------------
    TEST_METHOD(EagerCopyUp_PreservesTimestampsADSAndCompression) {
        UNIT_SKIP_IF_NOT_NTFS();

        LayerMountTests::TempLayerEnvironment env(1);
        const std::string payload(64 * 1024, 'E'); // below metacopy threshold
        env.WriteFile(env.Lower(0), L"eager.bin", payload);

        const std::wstring srcPath = env.Lower(0) + L"\\eager.bin";
        WriteADS(srcPath, L"Zone.Identifier",
                 "[ZoneTransfer]\r\nZoneId=3\r\n");
        const bool compressed = EnableCompression(srcPath);
        // Stamp LAST, after all ADS / compression writes have bumped times.
        const FILETIME srcWrite = MakeFileTime(2017, 6, 15);
        StampFile(srcPath, srcWrite, srcWrite, srcWrite);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"eager.bin")));

        const std::wstring upperPath = env.Upper() + L"\\eager.bin";

        FILETIME c{}, a{}, w{};
        GetTimes(upperPath, &c, &a, &w);
        Assert::IsTrue(FileTimesEqual(w, srcWrite),
            L"Eager copy-up must preserve LastWriteTime");
        Assert::IsTrue(ADSExists(upperPath, L"Zone.Identifier"),
            L"Eager copy-up must preserve user ADS");

        if (compressed) {
            // Surprising finding: the EAGER path DOES preserve compression
            // (CopyUpFile's commit stage applies FSCTL_SET_COMPRESSION on
            // the upper before data flows), which makes the lazy path's
            // loss of compression a pure asymmetry rather than a
            // codebase-wide miss. Document that and guard against a
            // regression.
            Assert::IsTrue(IsCompressed(upperPath),
                L"Eager copy-up must preserve FILE_ATTRIBUTE_COMPRESSED. "
                L"This is the PRESERVED behavior the lazy path currently "
                L"fails to match — see "
                L"LazyCompletion_CompressionAttributeLost_DocumentedGap.");
        }
    }
};

} // namespace LayerMountTests
