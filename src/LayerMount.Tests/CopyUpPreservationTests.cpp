// Unit tests for metadata preservation during copy-up.

#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataStore.h"
#include "EntryCopy.h"

#include <winioctl.h>
#include <cstddef>
#include <string_view>

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

void MakeSparseWithDataRange(const std::wstring& path, LONGLONG logicalBytes,
                             LONGLONG dataOffset, const std::string& data) {
    MakeSparse(path, logicalBytes);
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, h,
        L"MakeSparseWithDataRange: open failed");
    LARGE_INTEGER pos{};
    pos.QuadPart = dataOffset;
    Assert::IsTrue(::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != FALSE);
    DWORD w = 0;
    Assert::IsTrue(::WriteFile(h, data.data(), static_cast<DWORD>(data.size()),
                               &w, nullptr) != FALSE);
    Assert::AreEqual<DWORD>(static_cast<DWORD>(data.size()), w);
    ::CloseHandle(h);
}

::LayerMount::abi::CapabilityGate GateWithoutSparseFiles() {
    return ::LayerMount::abi::CapabilityGate(
        LM_CAP_ADS | LM_CAP_REPARSE_POINTS | LM_CAP_MULTIPLE_STREAMS |
        LM_CAP_NTFS_ACLS);
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

constexpr size_t kLongStreamNameLength = 120;
constexpr size_t kListedStreamNameChars =
    std::wstring_view(L":").size() + kLongStreamNameLength +
    std::wstring_view(L":$DATA").size();
constexpr size_t kStreamInfoEntryBytes =
    offsetof(FILE_STREAM_INFO, StreamName) + kListedStreamNameChars * sizeof(wchar_t);
constexpr int kLongStreamCount =
    static_cast<int>(kInitialStreamListSize * 3 / 2 / kStreamInfoEntryBytes);

std::wstring LongStreamName(int index) {
    std::wstring name = L"s" + std::to_wstring(index) + L"-";
    name.resize(kLongStreamNameLength, L'x');
    return name;
}

}

TEST_CLASS(CopyUpPreservationTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }


    TEST_METHOD(CopyUpFile_PreservesSparseAttribute) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sparse.bin", "");  // create, zero-size
        const std::wstring srcPath = env.Lower(0) + L"\\sparse.bin";
        MakeSparse(srcPath, 1024 * 1024); // 1 MiB logical, 0 allocated

        Assert::IsTrue(HasSparseAttribute(srcPath),
            L"Precondition: lower file must be sparse");

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"sparse.bin")));

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

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"sparse.bin")));

        Assert::AreEqual(logical,
                         LayerMountTests::LogicalBytes(env.Upper() + L"\\sparse.bin"),
            L"Logical end-of-file size should match source");
    }

    TEST_METHOD(CopyUpFile_KeepsHolesOfSparseSource) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sparse.bin", "");
        const std::wstring srcPath = env.Lower(0) + L"\\sparse.bin";
        const LONGLONG logical = 4LL * 1024 * 1024;
        const LONGLONG dataOffset = 1LL * 1024 * 1024;
        const std::string data(64 * 1024, 'd');
        MakeSparseWithDataRange(srcPath, logical, dataOffset, data);
        Assert::IsTrue(LayerMountTests::AllocatedBytes(srcPath) < dataOffset,
            L"Precondition: the lower file allocates only its data range");

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"sparse.bin")));

        const std::wstring upPath = env.Upper() + L"\\sparse.bin";
        Assert::IsTrue(HasSparseAttribute(upPath),
            L"The upper copy keeps FILE_ATTRIBUTE_SPARSE_FILE");
        Assert::AreEqual(logical, LayerMountTests::LogicalBytes(upPath),
            L"The upper copy has the logical size of the source");
        Assert::IsTrue(LayerMountTests::AllocatedBytes(upPath) < dataOffset,
            L"The upper copy allocates its data range only, not its holes");
        Assert::AreEqual(data,
                         LayerMountTests::ReadRange(upPath, dataOffset,
                                                    static_cast<DWORD>(data.size())),
            L"The data range of the upper copy matches the source");
        Assert::AreEqual(std::string(64 * 1024, '\0'),
                         LayerMountTests::ReadRange(upPath, 0, 64 * 1024),
            L"A hole of the upper copy reads as zeros");
    }

    TEST_METHOD(CopyUpFile_GivesDenseCopyOfSparseSourceWithoutSparseCapability) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sparse.bin", "");
        const std::wstring srcPath = env.Lower(0) + L"\\sparse.bin";
        MakeSparse(srcPath, 64 * 1024);
        Assert::IsTrue(HasSparseAttribute(srcPath),
            L"Precondition: lower file must be sparse");

        CopyUpAndRenameRig rig(env.MakeConfig());
        rig.copyUp.SetCapabilityGate(GateWithoutSparseFiles());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"sparse.bin")));

        Assert::IsFalse(HasSparseAttribute(env.Upper() + L"\\sparse.bin"),
            L"Without the sparse capability the upper copy is dense");
    }

    TEST_METHOD(DirectoryRename_GivesDenseCopyOfSparseChildWithoutSparseCapability) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"tree");
        env.WriteFile(env.Lower(0), L"tree\\sparse.bin", "");
        const std::wstring srcPath = env.Lower(0) + L"\\tree\\sparse.bin";
        MakeSparse(srcPath, 64 * 1024);
        Assert::IsTrue(HasSparseAttribute(srcPath),
            L"Precondition: lower file must be sparse");

        CopyUpAndRenameRig rig(env.MakeConfig());
        rig.copyUp.SetCapabilityGate(GateWithoutSparseFiles());
        DirectoryRename dirRename(rig.config, rig.resolver, rig.whiteouts, rig.cache,
                                  rig.copyUp, GateWithoutSparseFiles());

        Assert::IsTrue(NT_SUCCESS(dirRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            RenameEntryKind::Directory, ReplaceExisting::No)));

        Assert::IsTrue(env.FileExists(env.Upper(), L"moved\\sparse.bin"));
        Assert::IsFalse(HasSparseAttribute(env.Upper() + L"\\moved\\sparse.bin"),
            L"Without the sparse capability the tree copy is dense");
    }

    TEST_METHOD(DirectoryRename_PreservesSparseAttributeOfChild) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"tree");
        env.WriteFile(env.Lower(0), L"tree\\sparse.bin", "");
        const std::wstring srcPath = env.Lower(0) + L"\\tree\\sparse.bin";
        MakeSparse(srcPath, 64 * 1024);
        Assert::IsTrue(HasSparseAttribute(srcPath),
            L"Precondition: lower file must be sparse");

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            RenameEntryKind::Directory, ReplaceExisting::No)));

        Assert::IsTrue(HasSparseAttribute(env.Upper() + L"\\moved\\sparse.bin"),
            L"With the sparse capability the tree copy keeps FILE_ATTRIBUTE_SPARSE_FILE");
    }

    TEST_METHOD(DirectoryRename_PreservesCompressionOfChild) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"tree");
        const std::string payload(64 * 1024, 'c');
        env.WriteFile(env.Lower(0), L"tree\\cmp.bin", payload);
        const std::wstring srcPath = env.Lower(0) + L"\\tree\\cmp.bin";
        if (!EnableCompression(srcPath)) {
            Logger::WriteMessage(L"[SKIP] The volume refused FSCTL_SET_COMPRESSION on the lower file");
            return;
        }
        Assert::IsTrue(HasAttribute(srcPath, FILE_ATTRIBUTE_COMPRESSED),
            L"Precondition: lower file must be compressed");

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            RenameEntryKind::Directory, ReplaceExisting::No)));

        Assert::IsTrue(HasAttribute(env.Upper() + L"\\moved\\cmp.bin", FILE_ATTRIBUTE_COMPRESSED),
            L"The tree copy keeps FILE_ATTRIBUTE_COMPRESSED on a compressed child");
        Assert::AreEqual(payload, env.ReadFile(env.Upper(), L"moved\\cmp.bin"));
    }


    TEST_METHOD(CopyUpFile_PreservesUserAlternateDataStreams) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"doc.txt", "main-content");
        // Simulate a Mark-of-the-Web zone.identifier style ADS plus a custom one.
        WriteADS(env.Lower(0), L"doc.txt", L"zone.identifier",
                 "[ZoneTransfer]\r\nZoneId=3\r\n");
        WriteADS(env.Lower(0), L"doc.txt", L"custom.stream", "secret-metadata");

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"doc.txt")));

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
        MetadataStore::WriteLayerMountMetadata(env.Lower(0) + L"\\book.txt", fake, nullptr);

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"book.txt")));

        // Upper's :overlay metadata should reflect copy-up truth, not the
        // fabricated value from lower.
        LayerMountMetadata md = MetadataStore::ReadLayerMountMetadata(
            env.Upper() + L"\\book.txt", nullptr);
        Assert::AreNotEqual(std::wstring(L"bogus"), md.originLayer,
            L"Upper's bookkeeping ADS must be written by copy-up, not inherited");
    }

    TEST_METHOD(CopyUpFile_CopiesUserStreamNamedOverlayNotes) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"notes.txt", "content");
        WriteADS(env.Lower(0), L"notes.txt", L"overlayNotes", "user notes");
        WriteADS(env.Lower(0), L"notes.txt", L"overlay.extra", "reserved");

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"notes.txt")));

        Assert::AreEqual(std::string("user notes"),
                         ReadADS(env.Upper(), L"notes.txt", L"overlayNotes"),
            L"A stream whose name only starts with overlay is user data");
        Assert::IsFalse(ADSExists(env.Upper(), L"notes.txt", L"overlay.extra"),
            L"A stream under overlay. is reserved and stays in its layer");
    }

    TEST_METHOD(CopyUpFile_CopiesEveryStreamOfFileWithManyLongNamedStreams) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"many.txt", "content");
        for (int i = 0; i < kLongStreamCount; ++i) {
            WriteADS(env.Lower(0), L"many.txt", LongStreamName(i), std::to_string(i));
        }

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"many.txt")));

        for (int i = 0; i < kLongStreamCount; ++i) {
            Assert::AreEqual(std::to_string(i),
                             ReadADS(env.Upper(), L"many.txt", LongStreamName(i)),
                L"Every stream of the lower file must reach the upper copy");
        }
    }

    TEST_METHOD(DirectoryRename_CopiesUserStreamNamedOverlayNotesOfChild) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"tree");
        env.WriteFile(env.Lower(0), L"tree\\notes.txt", "content");
        WriteADS(env.Lower(0), L"tree\\notes.txt", L"overlayNotes", "user notes");

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            RenameEntryKind::Directory, ReplaceExisting::No)));

        Assert::AreEqual(std::string("user notes"),
                         ReadADS(env.Upper(), L"moved\\notes.txt", L"overlayNotes"),
            L"The tree copy keeps a user stream whose name starts with overlay");
    }


    TEST_METHOD(CopyUpFile_PreservesSystemAttribute) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"s.bin", "x");
        ::SetFileAttributesW((env.Lower(0) + L"\\s.bin").c_str(),
                              FILE_ATTRIBUTE_SYSTEM);

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"s.bin")));

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

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"t.bin")));

        DWORD attrs = ::GetFileAttributesW((env.Upper() + L"\\t.bin").c_str());
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_TEMPORARY) != 0,
            L"TEMPORARY attribute must survive copy-up");
    }

    TEST_METHOD(CopyUpFile_PreservesCombinedHiddenSystem) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"hs.bin", "x");
        ::SetFileAttributesW((env.Lower(0) + L"\\hs.bin").c_str(),
                              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"hs.bin")));

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

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpDirectory(L"sysdir")));

        DWORD attrs = ::GetFileAttributesW((env.Upper() + L"\\sysdir").c_str());
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_SYSTEM) != 0,
            L"Directory SYSTEM attribute must survive copy-up");

        ::SetFileAttributesW((env.Lower(0) + L"\\sysdir").c_str(),
                              FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL);
        ::SetFileAttributesW((env.Upper() + L"\\sysdir").c_str(),
                              FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL);
    }
};

}
