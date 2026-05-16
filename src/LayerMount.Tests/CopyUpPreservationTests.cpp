// Unit tests for metadata preservation during copy-up. These exercise what
// CopyUp::CopyUpFile / CopyUpDirectory preserves beyond the basics already
// covered in CopyUpTests (file size, timestamps, READONLY, HIDDEN). Scenarios
// surfaced by a coverage-gap investigation:
//   - Sparse files (sparse attribute + allocation size semantics)
//   - User alternate data streams on regular files (file.txt:zone.identifier)
//   - Less common file attributes (SYSTEM, TEMPORARY)

#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataADS.h"

#include <winioctl.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

namespace {

// Mark a file as sparse and extend it so the logical size exceeds the allocated
// bytes. Returns the logical size after extension.
LONGLONG MakeSparse(const std::wstring& path, LONGLONG logicalBytes) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, h, L"MakeSparse: open failed");

    DWORD bytesReturned = 0;
    FILE_SET_SPARSE_BUFFER sparseBuf{TRUE};
    ::DeviceIoControl(h, FSCTL_SET_SPARSE, &sparseBuf, sizeof(sparseBuf),
                       nullptr, 0, &bytesReturned, nullptr);

    LARGE_INTEGER pos{};
    pos.QuadPart = logicalBytes;
    ::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    ::SetEndOfFile(h);
    ::CloseHandle(h);
    return logicalBytes;
}

bool HasSparseAttribute(const std::wstring& path) {
    DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
}

// Write an alternate data stream attached to the given file at layerPath/rel.
void WriteADS(const std::wstring& layerPath, const std::wstring& rel,
              const std::wstring& streamName, const std::string& content) {
    const std::wstring full = layerPath + L"\\" + rel + L":" + streamName;
    HANDLE h = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, h, L"WriteADS: open failed");
    DWORD w = 0;
    ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &w, nullptr);
    ::CloseHandle(h);
}

std::string ReadADS(const std::wstring& layerPath, const std::wstring& rel,
                    const std::wstring& streamName) {
    const std::wstring full = layerPath + L"\\" + rel + L":" + streamName;
    HANDLE h = ::CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buf[4096];
    DWORD r = 0;
    ::ReadFile(h, buf, sizeof(buf), &r, nullptr);
    ::CloseHandle(h);
    return std::string(buf, r);
}

bool ADSExists(const std::wstring& layerPath, const std::wstring& rel,
               const std::wstring& streamName) {
    const std::wstring full = layerPath + L"\\" + rel + L":" + streamName;
    HANDLE h = ::CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

} // namespace

TEST_CLASS(CopyUpPreservationTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    // ---------------------------------------------------------------
    // Sparse files
    // ---------------------------------------------------------------

    TEST_METHOD(CopyUpFile_PreservesSparseAttribute) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sparse.bin", "");  // create, zero-size
        const std::wstring srcPath = env.Lower(0) + L"\\sparse.bin";
        MakeSparse(srcPath, 1024 * 1024); // 1 MiB logical, 0 allocated

        Assert::IsTrue(HasSparseAttribute(srcPath),
            L"Precondition: lower file must be sparse");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"sparse.bin")));

        const std::wstring upPath = env.Upper() + L"\\sparse.bin";
        Assert::IsTrue(HasSparseAttribute(upPath),
            L"Copy-up should preserve FILE_ATTRIBUTE_SPARSE_FILE");
    }

    TEST_METHOD(CopyUpFile_PreservesLogicalSizeForSparseFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sparse.bin", "");
        const std::wstring srcPath = env.Lower(0) + L"\\sparse.bin";
        const LONGLONG logical = 64 * 1024;
        MakeSparse(srcPath, logical);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"sparse.bin")));

        HANDLE h = ::CreateFileW((env.Upper() + L"\\sparse.bin").c_str(),
                                  GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, h);
        LARGE_INTEGER sz{};
        ::GetFileSizeEx(h, &sz);
        ::CloseHandle(h);
        Assert::AreEqual(logical, sz.QuadPart,
            L"Logical end-of-file size should match source");
    }

    // ---------------------------------------------------------------
    // User alternate data streams
    // ---------------------------------------------------------------

    TEST_METHOD(CopyUpFile_PreservesUserAlternateDataStreams) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"doc.txt", "main-content");
        // Simulate a Mark-of-the-Web zone.identifier style ADS plus a custom one.
        WriteADS(env.Lower(0), L"doc.txt", L"zone.identifier",
                 "[ZoneTransfer]\r\nZoneId=3\r\n");
        WriteADS(env.Lower(0), L"doc.txt", L"custom.stream", "secret-metadata");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"doc.txt")));

        // Main stream copied (already covered elsewhere — quick sanity check).
        Assert::AreEqual(std::string("main-content"),
                         env.ReadFile(env.Upper(), L"doc.txt"));

        // User ADS streams must round-trip.
        Assert::IsTrue(ADSExists(env.Upper(), L"doc.txt", L"zone.identifier"),
            L"zone.identifier ADS must survive copy-up");
        Assert::AreEqual(std::string("[ZoneTransfer]\r\nZoneId=3\r\n"),
                         ReadADS(env.Upper(), L"doc.txt", L"zone.identifier"));
        Assert::IsTrue(ADSExists(env.Upper(), L"doc.txt", L"custom.stream"),
            L"Custom user ADS must survive copy-up");
    }

    TEST_METHOD(CopyUpFile_DoesNotCopyLayerMountReservedStreams) {
        // The :overlay and :overlay.opaque streams are bookkeeping — they
        // belong to whichever copy of the file lives in that layer. If copy-up
        // ever mirrored the bookkeeping ADS from lower into upper, layer-aware
        // logic would be confused.
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"book.txt", "content");
        // Plant a fake bookkeeping stream in lower.
        LayerMountMetadata fake;
        fake.originLayer = L"bogus";
        MetadataADS::WriteLayerMountMetadata(env.Lower(0) + L"\\book.txt", fake);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"book.txt")));

        // Upper's :overlay metadata should reflect copy-up truth, not the
        // fabricated value from lower.
        LayerMountMetadata md = MetadataADS::ReadLayerMountMetadata(
            env.Upper() + L"\\book.txt");
        Assert::AreNotEqual(std::wstring(L"bogus"), md.originLayer,
            L"Upper's bookkeeping ADS must be written by copy-up, not inherited");
    }

    // ---------------------------------------------------------------
    // Less common file attributes
    // ---------------------------------------------------------------

    TEST_METHOD(CopyUpFile_PreservesSystemAttribute) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"s.bin", "x");
        ::SetFileAttributesW((env.Lower(0) + L"\\s.bin").c_str(),
                              FILE_ATTRIBUTE_SYSTEM);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"s.bin")));

        DWORD attrs = ::GetFileAttributesW((env.Upper() + L"\\s.bin").c_str());
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_SYSTEM) != 0,
            L"SYSTEM attribute must survive copy-up");

        // Clean up so the temp dir can be removed.
        ::SetFileAttributesW((env.Lower(0) + L"\\s.bin").c_str(), FILE_ATTRIBUTE_NORMAL);
        ::SetFileAttributesW((env.Upper() + L"\\s.bin").c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    TEST_METHOD(CopyUpFile_PreservesTemporaryAttribute) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"t.bin", "x");
        ::SetFileAttributesW((env.Lower(0) + L"\\t.bin").c_str(),
                              FILE_ATTRIBUTE_TEMPORARY);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"t.bin")));

        DWORD attrs = ::GetFileAttributesW((env.Upper() + L"\\t.bin").c_str());
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_TEMPORARY) != 0,
            L"TEMPORARY attribute must survive copy-up");
    }

    TEST_METHOD(CopyUpFile_PreservesCombinedHiddenSystem) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"hs.bin", "x");
        ::SetFileAttributesW((env.Lower(0) + L"\\hs.bin").c_str(),
                              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"hs.bin")));

        DWORD attrs = ::GetFileAttributesW((env.Upper() + L"\\hs.bin").c_str());
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_HIDDEN) != 0);
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_SYSTEM) != 0);

        // Clean up for temp dir removal.
        ::SetFileAttributesW((env.Lower(0) + L"\\hs.bin").c_str(), FILE_ATTRIBUTE_NORMAL);
        ::SetFileAttributesW((env.Upper() + L"\\hs.bin").c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    TEST_METHOD(CopyUpDirectory_PreservesSystemAttribute) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"sysdir");
        ::SetFileAttributesW((env.Lower(0) + L"\\sysdir").c_str(),
                              FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_SYSTEM);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpDirectory(L"sysdir")));

        DWORD attrs = ::GetFileAttributesW((env.Upper() + L"\\sysdir").c_str());
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_SYSTEM) != 0,
            L"Directory SYSTEM attribute must survive copy-up");

        ::SetFileAttributesW((env.Lower(0) + L"\\sysdir").c_str(),
                              FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL);
        ::SetFileAttributesW((env.Upper() + L"\\sysdir").c_str(),
                              FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL);
    }
};

} // namespace LayerMountTests
