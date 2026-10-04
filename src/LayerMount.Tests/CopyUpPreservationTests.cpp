// Unit tests for metadata preservation during copy-up.

#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataStore.h"
#include "EntryCopy.h"
#include "ExtendedAttributeTestHelpers.h"

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

// Each entry starts at a ULONG boundary, as NtQueryEaFile returns it.
std::vector<BYTE> FullEaList(const std::vector<NamedExtendedAttribute>& attributes) {
    std::vector<BYTE> list;
    size_t previous = 0;
    for (size_t i = 0; i < attributes.size(); ++i) {
        if (i != 0) {
            list.resize((list.size() + 3) & ~size_t{3});
            reinterpret_cast<FullEaHeader*>(list.data() + previous)->nextEntryOffset =
                static_cast<ULONG>(list.size() - previous);
        }
        previous = list.size();
        const std::vector<BYTE> entry = FullEaEntry(attributes[i].name, attributes[i].value);
        list.insert(list.end(), entry.begin(), entry.end());
    }
    return list;
}

// Fails the test when an entry does not start at a ULONG boundary.
std::vector<NamedExtendedAttribute> AttributesInFullEaList(const std::vector<BYTE>& list) {
    std::vector<NamedExtendedAttribute> attributes;
    size_t offset = 0;
    while (offset < list.size()) {
        Assert::AreEqual(size_t{0}, offset % sizeof(ULONG),
            L"Each entry of the list must start at a ULONG boundary");
        const auto* header = reinterpret_cast<const FullEaHeader*>(list.data() + offset);
        const auto* name = reinterpret_cast<const char*>(header + 1);
        attributes.push_back({std::string(name, header->nameLength),
                              std::string(name + header->nameLength + 1, header->valueLength)});
        if (header->nextEntryOffset == 0) {
            break;
        }
        offset += header->nextEntryOffset;
    }
    return attributes;
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

    TEST_METHOD(DirectoryRename_CompressedDirectory_KeepsEachChildFilesCompression) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"tree");
        const std::string payload(64 * 1024, 'p');
        env.WriteFile(env.Lower(0), L"tree\\plain.bin", payload);
        env.WriteFile(env.Lower(0), L"tree\\cmp.bin", payload);
        const std::wstring lowerTree = env.Lower(0) + L"\\tree";
        if (!EnableCompression(lowerTree + L"\\cmp.bin") || !EnableCompression(lowerTree)) {
            Logger::WriteMessage(L"[SKIP] The volume refused FSCTL_SET_COMPRESSION on the lower tree");
            return;
        }
        Assert::IsFalse(HasAttribute(lowerTree + L"\\plain.bin", FILE_ATTRIBUTE_COMPRESSED),
            L"Precondition: lower plain.bin must be uncompressed");

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            RenameEntryKind::Directory, ReplaceExisting::No)));

        const std::wstring movedTree = env.Upper() + L"\\moved";
        Assert::IsFalse(HasAttribute(movedTree + L"\\plain.bin", FILE_ATTRIBUTE_COMPRESSED),
            L"The tree copy keeps an uncompressed child uncompressed");
        Assert::IsTrue(HasAttribute(movedTree + L"\\cmp.bin", FILE_ATTRIBUTE_COMPRESSED),
            L"The tree copy keeps a compressed child compressed");
        Assert::IsTrue(HasAttribute(movedTree, FILE_ATTRIBUTE_COMPRESSED),
            L"The tree copy keeps the directory compressed");
        Assert::AreEqual(payload, env.ReadFile(env.Upper(), L"moved\\plain.bin"));
    }

    TEST_METHOD(DirectoryRename_CompressedDirectory_KeepsUncompressedSubdirectoryUncompressed) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"tree\\sub");
        env.WriteFile(env.Lower(0), L"tree\\sub\\f.txt", "lower");
        const std::wstring lowerTree = env.Lower(0) + L"\\tree";
        if (!EnableCompression(lowerTree)) {
            Logger::WriteMessage(L"[SKIP] The volume refused FSCTL_SET_COMPRESSION on the lower tree");
            return;
        }
        Assert::IsFalse(HasAttribute(lowerTree + L"\\sub", FILE_ATTRIBUTE_COMPRESSED),
            L"Precondition: lower tree\\sub must be uncompressed");

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            RenameEntryKind::Directory, ReplaceExisting::No)));

        Assert::IsFalse(HasAttribute(env.Upper() + L"\\moved\\sub", FILE_ATTRIBUTE_COMPRESSED),
            L"The tree copy keeps an uncompressed subdirectory uncompressed");
        Assert::IsFalse(HasAttribute(env.Upper() + L"\\moved\\sub\\f.txt", FILE_ATTRIBUTE_COMPRESSED),
            L"The tree copy keeps a file of the uncompressed subdirectory uncompressed");
    }

    TEST_METHOD(DirectoryRename_EncryptedDirectory_KeepsPlaintextChildUnencrypted) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"tree");
        env.WriteFile(env.Lower(0), L"tree\\plain.txt", "plaintext");
        const std::wstring lowerTree = env.Lower(0) + L"\\tree";
        if (!EncryptedOrSkipped(lowerTree)) {
            return;
        }
        Assert::IsFalse(HasAttribute(lowerTree + L"\\plain.txt", FILE_ATTRIBUTE_ENCRYPTED),
            L"Precondition: lower plain.txt must be unencrypted");

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            RenameEntryKind::Directory, ReplaceExisting::No)));

        Assert::IsFalse(HasAttribute(env.Upper() + L"\\moved\\plain.txt", FILE_ATTRIBUTE_ENCRYPTED),
            L"The tree copy keeps an unencrypted child unencrypted");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\moved", FILE_ATTRIBUTE_ENCRYPTED),
            L"The tree copy keeps the directory encrypted");
        Assert::AreEqual(std::string("plaintext"), env.ReadFile(env.Upper(), L"moved\\plain.txt"));
    }


    TEST_METHOD(CopyUpFile_CopiesExtendedAttributesOfLowerFile) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"wsl.txt", "content");
        const auto attributes = WslAndUserExtendedAttributes(kWslFileMode);
        SetExtendedAttributes(env.Lower(0) + L"\\wsl.txt", attributes);
        const FILETIME stamped = LayerMountTestShared::MakeFileTime(2001, 1, 1);
        LayerMountTestShared::StampTimes(env.Lower(0) + L"\\wsl.txt", stamped, stamped, stamped);

        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpFile(L"wsl.txt"),
            L"The copy-up of a file with extended attributes must succeed");

        AssertHasExtendedAttributes(env.Upper() + L"\\wsl.txt", attributes);
        FILETIME upperWrite{};
        LayerMountTestShared::GetTimes(env.Upper() + L"\\wsl.txt", nullptr, nullptr, &upperWrite);
        Assert::IsTrue(::CompareFileTime(&stamped, &upperWrite) == 0,
            L"The upper file must have the last-write time of the lower file");
    }

    TEST_METHOD(ExtendedAttributesUserModeCanSet_LeavesOutKernelAttributesInAnyCase) {
        const std::string id1000 = LittleEndianUlong(1000);
        const std::vector<BYTE> list = FullEaList({{"$KERNEL.PURGE.ESBCACHE", "kernel"},
                                                   {"$LXUID", id1000},
                                                   {"$kernel.lower", "kernel"},
                                                   {"USER.NOTE", "lower note"},
                                                   {"$KERNEL.LAST", "kernel"}});

        const std::vector<BYTE> settable =
            ExtendedAttributesUserModeCanSet(list.data(), static_cast<ULONG>(list.size()));

        const std::vector<NamedExtendedAttribute> kept = AttributesInFullEaList(settable);
        Assert::AreEqual(size_t{2}, kept.size(),
            L"Only the two attributes outside $KERNEL. must be left");
        Assert::AreEqual(std::string("$LXUID"), kept[0].name);
        Assert::IsTrue(id1000 == kept[0].value, L"$LXUID must keep its value");
        Assert::AreEqual(std::string("USER.NOTE"), kept[1].name);
        Assert::AreEqual(std::string("lower note"), kept[1].value);
        const ULONG secondOffset =
            reinterpret_cast<const FullEaHeader*>(settable.data())->nextEntryOffset;
        const auto* last = reinterpret_cast<const FullEaHeader*>(settable.data() + secondOffset);
        Assert::AreEqual(ULONG{0}, last->nextEntryOffset,
            L"The last entry of the list must have a NextEntryOffset of 0");
    }

    TEST_METHOD(ExtendedAttributesUserModeCanSet_ListOfOnlyKernelAttributes_IsEmpty) {
        const std::vector<BYTE> list = FullEaList({{"$KERNEL.ONE", "kernel"},
                                                   {"$KERNEL.TWO", "kernel"}});

        const std::vector<BYTE> settable =
            ExtendedAttributesUserModeCanSet(list.data(), static_cast<ULONG>(list.size()));

        Assert::IsTrue(settable.empty(), L"No attribute of the list may be left");
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

    TEST_METHOD(CopyUpDirectory_CopiesExtendedAttributesOfLowerDirectory) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"dir");
        const auto attributes = WslAndUserExtendedAttributes(kWslDirectoryMode);
        SetExtendedAttributes(env.Lower(0) + L"\\dir", attributes);

        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpDirectory(L"dir"),
            L"The copy-up of a directory with extended attributes must succeed");

        AssertHasExtendedAttributes(env.Upper() + L"\\dir", attributes);
    }

    TEST_METHOD(CopyUpDirectory_PreservesUserStreamOfDirectory) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"dir");
        WriteADS(env.Lower(0), L"dir", L"notes", "directory notes");
        WriteADS(env.Lower(0), L"dir", L"overlay.extra", "reserved");
        const FILETIME stamped = LayerMountTestShared::MakeFileTime(2001, 1, 1);
        LayerMountTestShared::StampTimes(env.Lower(0) + L"\\dir", stamped, stamped, stamped);

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpDirectory(L"dir")));

        Assert::AreEqual(std::string("directory notes"),
                         ReadADS(env.Upper(), L"dir", L"notes"),
            L"The upper directory has the user stream of the lower directory");
        Assert::IsFalse(ADSExists(env.Upper(), L"dir", L"overlay.extra"),
            L"A reserved overlay.* stream does not copy up");
        FILETIME upperWrite{};
        LayerMountTestShared::GetTimes(env.Upper() + L"\\dir", nullptr, nullptr, &upperWrite);
        Assert::IsTrue(::CompareFileTime(&stamped, &upperWrite) == 0,
            L"The upper directory has the last-write time of the lower directory");
    }

    TEST_METHOD(DirectoryRename_CopiesUserStreamsOfRenamedDirectoryAndChildDirectory) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"tree");
        env.CreateDir(env.Lower(0), L"tree\\sub");
        WriteADS(env.Lower(0), L"tree", L"notes", "tree notes");
        WriteADS(env.Lower(0), L"tree\\sub", L"notes", "sub notes");

        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            RenameEntryKind::Directory, ReplaceExisting::No)));

        Assert::AreEqual(std::string("tree notes"),
                         ReadADS(env.Upper(), L"moved", L"notes"),
            L"The renamed directory keeps its user stream");
        Assert::AreEqual(std::string("sub notes"),
                         ReadADS(env.Upper(), L"moved\\sub", L"notes"),
            L"A child directory of the renamed directory keeps its user stream");
    }

    TEST_METHOD(DirectoryRename_CopiesExtendedAttributesOfTheTree) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"tree");
        env.CreateDir(env.Lower(0), L"tree\\sub");
        env.WriteFile(env.Lower(0), L"tree\\sub\\file.txt", "content");
        const auto directoryAttributes = WslAndUserExtendedAttributes(kWslDirectoryMode);
        const auto fileAttributes = WslAndUserExtendedAttributes(kWslFileMode);
        SetExtendedAttributes(env.Lower(0) + L"\\tree", directoryAttributes);
        SetExtendedAttributes(env.Lower(0) + L"\\tree\\sub", directoryAttributes);
        SetExtendedAttributes(env.Lower(0) + L"\\tree\\sub\\file.txt", fileAttributes);

        CopyUpAndRenameRig rig(env.MakeConfig());

        AssertStatus(STATUS_SUCCESS, rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            RenameEntryKind::Directory, ReplaceExisting::No),
            L"The rename of a tree with extended attributes must succeed");

        AssertHasExtendedAttributes(env.Upper() + L"\\moved", directoryAttributes);
        AssertHasExtendedAttributes(env.Upper() + L"\\moved\\sub", directoryAttributes);
        AssertHasExtendedAttributes(env.Upper() + L"\\moved\\sub\\file.txt", fileAttributes);
    }

    TEST_METHOD(DirectoryRename_CopiesExtendedAttributesOfReadOnlyDirectoryAndReadOnlyFile) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"ro");
        env.WriteFile(env.Lower(0), L"ro\\file.txt", "content");
        const auto directoryAttributes = WslAndUserExtendedAttributes(kWslDirectoryMode);
        const auto fileAttributes = WslAndUserExtendedAttributes(kWslFileMode);
        const std::wstring lowerDir = env.Lower(0) + L"\\ro";
        const std::wstring lowerFile = lowerDir + L"\\file.txt";
        SetExtendedAttributes(lowerDir, directoryAttributes);
        SetExtendedAttributes(lowerFile, fileAttributes);
        ::SetFileAttributesW(lowerFile.c_str(), FILE_ATTRIBUTE_READONLY);
        ::SetFileAttributesW(lowerDir.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY);

        CopyUpAndRenameRig rig(env.MakeConfig());

        const NTSTATUS status = rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"ro"), CallerPath(L"moved"),
            RenameEntryKind::Directory, ReplaceExisting::No);
        const std::wstring upperDir = env.Upper() + L"\\moved";
        const std::wstring upperFile = upperDir + L"\\file.txt";
        const DWORD upperDirAttrs = ::GetFileAttributesW(upperDir.c_str());
        const DWORD upperFileAttrs = ::GetFileAttributesW(upperFile.c_str());
        ::SetFileAttributesW(lowerDir.c_str(), FILE_ATTRIBUTE_DIRECTORY);
        ::SetFileAttributesW(lowerFile.c_str(), FILE_ATTRIBUTE_NORMAL);
        ::SetFileAttributesW(upperDir.c_str(), FILE_ATTRIBUTE_DIRECTORY);
        ::SetFileAttributesW(upperFile.c_str(), FILE_ATTRIBUTE_NORMAL);

        AssertStatus(STATUS_SUCCESS, status,
            L"The rename of a read-only directory with extended attributes must succeed");
        AssertHasExtendedAttributes(upperDir, directoryAttributes);
        AssertHasExtendedAttributes(upperFile, fileAttributes);
        Assert::IsTrue(upperDirAttrs != INVALID_FILE_ATTRIBUTES &&
                           (upperDirAttrs & FILE_ATTRIBUTE_READONLY) != 0,
            L"The renamed directory keeps FILE_ATTRIBUTE_READONLY");
        Assert::IsTrue(upperFileAttrs != INVALID_FILE_ATTRIBUTES &&
                           (upperFileAttrs & FILE_ATTRIBUTE_READONLY) != 0,
            L"The copied file keeps FILE_ATTRIBUTE_READONLY");
    }

    TEST_METHOD(DirectoryRename_CopiesUserStreamOfReadOnlyDirectory) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"ro");
        WriteADS(env.Lower(0), L"ro", L"notes", "read-only notes");
        const std::wstring lowerDir = env.Lower(0) + L"\\ro";
        ::SetFileAttributesW(lowerDir.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY);

        CopyUpAndRenameRig rig(env.MakeConfig());

        const NTSTATUS status = rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"ro"), CallerPath(L"moved"),
            RenameEntryKind::Directory, ReplaceExisting::No);
        const std::wstring upperDir = env.Upper() + L"\\moved";
        const DWORD upperAttrs = ::GetFileAttributesW(upperDir.c_str());
        const std::string notes = ReadADS(env.Upper(), L"moved", L"notes");
        ::SetFileAttributesW(lowerDir.c_str(), FILE_ATTRIBUTE_DIRECTORY);
        ::SetFileAttributesW(upperDir.c_str(), FILE_ATTRIBUTE_DIRECTORY);

        AssertStatus(STATUS_SUCCESS, status, L"The rename of a read-only directory succeeds");
        Assert::AreEqual(std::string("read-only notes"), notes,
            L"The renamed read-only directory keeps its user stream");
        Assert::IsTrue(upperAttrs != INVALID_FILE_ATTRIBUTES &&
                           (upperAttrs & FILE_ATTRIBUTE_READONLY) != 0,
            L"The renamed directory keeps FILE_ATTRIBUTE_READONLY");
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
