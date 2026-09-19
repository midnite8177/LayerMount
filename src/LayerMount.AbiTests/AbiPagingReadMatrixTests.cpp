#include "pch.h"
#include "AbiTestFixture.h"

#include <algorithm>
#include <cwctype>
#include <map>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {
namespace {

constexpr UINT64 kPageBytes         = 4096;
constexpr UINT64 kStraddleReadBytes = 8192;

// The engine stages a metacopy shell only for a lower file larger than
// 1 MiB, so the shell origin adds 2 MiB to every listed size.
constexpr UINT64 kAboveMetacopyThresholdBytes = 2ull * 1024 * 1024;

constexpr UINT32 kCopyUpAccess = FILE_GENERIC_READ | FILE_GENERIC_WRITE;

// HRESULT_FROM_NT(STATUS_END_OF_FILE), the form the ABI returns for a read
// at or past the end of the file.
constexpr HRESULT kHrEndOfFileNt = static_cast<HRESULT>(0xD0000011);

constexpr BYTE kSentinel = 0xCD;

constexpr UINT64 kSizes[] = {0, 1, 4095, 4096, 4097, 65536, 65537, 1048576};

enum class Origin {
    UpperThroughMount,
    MetacopyShell,
    LowerCopiedUp,
    UpperBeforeMount,
    Lower,
};

const wchar_t* OriginName(Origin origin) {
    switch (origin) {
    case Origin::UpperThroughMount: return L"upper-through-mount";
    case Origin::MetacopyShell:     return L"metacopy-shell";
    case Origin::LowerCopiedUp:     return L"lower-copied-up";
    case Origin::UpperBeforeMount:  return L"upper-before-mount";
    case Origin::Lower:             return L"lower";
    }
    return L"unknown";
}

enum class ReadShape { Inside, Straddle, AtEnd };

const wchar_t* ShapeName(ReadShape shape) {
    switch (shape) {
    case ReadShape::Inside:   return L"inside";
    case ReadShape::Straddle: return L"straddle";
    case ReadShape::AtEnd:    return L"at-end";
    }
    return L"unknown";
}

struct MatrixCell {
    Origin    origin;
    UINT64    listedSize;
    UINT64    stagedSize;
    ReadShape shape;
};

struct ReadRequest {
    bool   applicable;
    UINT64 offset;
    UINT32 length;
};

UINT64 RoundUpToPage(UINT64 bytes) {
    return (bytes + kPageBytes - 1) & ~(kPageBytes - 1);
}

ReadRequest ShapeFor(ReadShape shape, UINT64 fileSize) {
    switch (shape) {
    case ReadShape::Inside:
        return {fileSize >= kPageBytes, 0, static_cast<UINT32>(kPageBytes)};
    case ReadShape::Straddle: {
        if (fileSize == 0) return {false, 0, 0};
        const UINT64 offset = ((fileSize - 1) / kPageBytes) * kPageBytes;
        return {true, offset, static_cast<UINT32>(kStraddleReadBytes)};
    }
    case ReadShape::AtEnd:
        return {true, RoundUpToPage(fileSize), static_cast<UINT32>(kPageBytes)};
    }
    return {false, 0, 0};
}

BYTE PatternByte(UINT64 index) {
    return static_cast<BYTE>((index * 7 + 13) & 0xFF);
}

std::string PatternBytes(UINT64 size) {
    std::string bytes(static_cast<size_t>(size), '\0');
    UINT64      index = 0;
    std::generate(bytes.begin(), bytes.end(),
                  [&] { return static_cast<char>(PatternByte(index++)); });
    return bytes;
}

UINT64 StagedSize(Origin origin, UINT64 listedSize) {
    return origin == Origin::MetacopyShell ? kAboveMetacopyThresholdBytes + listedSize
                                           : listedSize;
}

std::wstring FileName(Origin origin, UINT64 listedSize) {
    return std::wstring(OriginName(origin)) + L"-" + std::to_wstring(listedSize) + L".bin";
}

std::wstring OverlayPath(Origin origin, UINT64 listedSize) {
    return L"\\" + FileName(origin, listedSize);
}

bool IsEndOfFileHr(HRESULT hr) {
    return hr == kHrEndOfFileNt || hr == HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
}

class FileHandleHolder {
public:
    FileHandleHolder() = default;
    ~FileHandleHolder() { Reset(); }
    FileHandleHolder(const FileHandleHolder&)            = delete;
    FileHandleHolder& operator=(const FileHandleHolder&) = delete;

    LM_FILE_HANDLE  Get() const noexcept { return handle_; }
    LM_FILE_HANDLE* AddressOf() noexcept { return &handle_; }

    void Reset() noexcept {
        if (handle_ != nullptr) {
            (void)::LayerMountCloseFile(handle_);
            handle_ = nullptr;
        }
    }

private:
    LM_FILE_HANDLE handle_ = nullptr;
};

std::wstring Hex(unsigned value) {
    wchar_t buf[16] = {};
    swprintf_s(buf, L"0x%08X", value);
    return buf;
}

std::wstring CellTag(const MatrixCell& cell) {
    return std::wstring(L"origin=") + OriginName(cell.origin) +
           L" size=" + std::to_wstring(cell.listedSize) +
           L" shape=" + ShapeName(cell.shape);
}

std::wstring FailMessage(const MatrixCell& cell, const wchar_t* what) {
    return std::wstring(what) + L" [" + CellTag(cell) + L"]";
}

std::wstring ListingKey(PCWSTR name) {
    std::wstring key(name);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return key;
}

struct DirectoryEntry {
    bool         found = false;
    LM_FILE_INFO info{};
};

using DirectoryListing = std::map<std::wstring, LM_FILE_INFO>;

HRESULT LM_CALL CollectEntry(PCWSTR name, const LM_FILE_INFO* info, void* userContext) {
    auto* listing = static_cast<DirectoryListing*>(userContext);
    (*listing)[ListingKey(name)] = *info;
    return S_OK;
}

DirectoryEntry ListRootEntry(LM_HANDLE mount, const std::wstring& fileName) {
    DirectoryListing listing;
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountMergeDirectory(mount, L"\\", &CollectEntry, &listing),
        L"LayerMountMergeDirectory on the overlay root");
    DirectoryEntry entry;
    auto it = listing.find(ListingKey(fileName.c_str()));
    if (it != listing.end()) {
        entry.found = true;
        entry.info  = it->second;
    }
    return entry;
}

struct PathQuery {
    HRESULT         hr             = E_FAIL;
    LM_LAYER_SOURCE source         = LM_LAYER_NONE;
    UINT32          attributes     = INVALID_FILE_ATTRIBUTES;
    UINT64          fileSize       = 0;
    UINT64          allocationSize = 0;
};

PathQuery QueryPath(LM_HANDLE mount, const std::wstring& overlayPath) {
    std::vector<wchar_t> buf(MAX_PATH);
    LM_RESOLVED_PATH rp{};
    rp.absolutePath      = buf.data();
    rp.absolutePathChars = buf.size();
    PathQuery q;
    q.hr = ::LayerMountResolvePath(mount, overlayPath.c_str(), &rp);
    if (q.hr == S_OK) {
        q.source         = rp.source;
        q.attributes     = rp.attributes;
        q.fileSize       = rp.fileSize;
        q.allocationSize = rp.allocationSize;
    }
    return q;
}

void StageBeforeMount(const TempLayerEnv& env, Origin origin) {
    for (UINT64 listed : kSizes) {
        const std::string bytes = PatternBytes(StagedSize(origin, listed));
        switch (origin) {
        case Origin::UpperBeforeMount:
            env.WriteUpperFile(FileName(origin, listed), bytes);
            break;
        case Origin::MetacopyShell:
        case Origin::LowerCopiedUp:
        case Origin::Lower:
            env.WriteLowerFile(0, FileName(origin, listed), bytes);
            break;
        case Origin::UpperThroughMount:
            break;
        }
    }
}

void CreateThroughMount(LM_HANDLE mount, const std::wstring& overlayPath,
                        const std::string& bytes) {
    FileHandleHolder fh;
    LM_FILE_INFO     info{};
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountCreateFile(mount, overlayPath.c_str(),
            /*createOptions*/ 0u,
            /*grantedAccess*/ GENERIC_READ | GENERIC_WRITE,
            /*fileAttributes*/ FILE_ATTRIBUTE_NORMAL,
            /*securityDescriptor*/ nullptr, 0u,
            /*allocationSize*/ 0u,
            /*originatorPid*/ 0u,
            fh.AddressOf(), &info),
        (L"LayerMountCreateFile " + overlayPath).c_str());
    if (bytes.empty()) return;
    UINT32       written = 0;
    LM_FILE_INFO postWrite{};
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountWriteFile(fh.Get(), bytes.data(), /*offset*/ 0,
            static_cast<UINT32>(bytes.size()),
            /*writeToEnd*/ FALSE, /*constrainedIo*/ FALSE,
            /*originatorPid*/ 0u, &written, &postWrite),
        (L"LayerMountWriteFile " + overlayPath).c_str());
    Assert::AreEqual<UINT32>(static_cast<UINT32>(bytes.size()), written,
        (L"write count " + overlayPath).c_str());
}

struct WritableOpenRecord {
    LM_FILE_INFO openInfo{};
    LM_FILE_INFO handleInfo{};
};

WritableOpenRecord OpenWritableAndClose(LM_HANDLE mount, const std::wstring& overlayPath) {
    FileHandleHolder   fh;
    WritableOpenRecord rec;
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountOpenFile(mount, overlayPath.c_str(),
            /*grantedAccess*/ kCopyUpAccess,
            /*createOptions*/ 0u, /*originatorPid*/ 0u,
            fh.AddressOf(), &rec.openInfo),
        (L"writable LayerMountOpenFile " + overlayPath).c_str());
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountGetFileInfo(fh.Get(), &rec.handleInfo),
        (L"LayerMountGetFileInfo after writable open " + overlayPath).c_str());
    return rec;
}

struct CellRecord {
    HRESULT        hr            = E_FAIL;
    UINT32         count         = 0;
    bool           contentMatch  = false;
    bool           tailZeroed    = false;
    bool           tailUntouched = false;
    LM_FILE_INFO   openInfo{};
    LM_FILE_INFO   handleInfo{};
    DirectoryEntry dirEntry;
    PathQuery      path;
};

bool TailIsAll(const std::vector<BYTE>& buffer, UINT32 from, BYTE value) {
    return std::all_of(buffer.begin() + from, buffer.end(),
                       [value](BYTE b) { return b == value; });
}

CellRecord ReadCell(LM_HANDLE mount, const MatrixCell& cell, const ReadRequest& request) {
    const std::wstring overlayPath = OverlayPath(cell.origin, cell.listedSize);
    const std::wstring fileName    = FileName(cell.origin, cell.listedSize);
    CellRecord rec;

    FileHandleHolder fh;
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountOpenFile(mount, overlayPath.c_str(),
            /*grantedAccess*/ GENERIC_READ,
            /*createOptions*/ 0u, /*originatorPid*/ 0u,
            fh.AddressOf(), &rec.openInfo),
        (L"read LayerMountOpenFile " + overlayPath).c_str());
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountGetFileInfo(fh.Get(), &rec.handleInfo),
        (L"LayerMountGetFileInfo before read " + overlayPath).c_str());

    std::vector<BYTE> buffer(request.length, kSentinel);
    rec.hr = ::LayerMountReadFile(fh.Get(), buffer.data(), request.offset,
                                  request.length, /*originatorPid*/ 0u, &rec.count);
    fh.Reset();

    const UINT32 got = (std::min)(rec.count, request.length);
    std::vector<BYTE> expected(got);
    UINT64 index = request.offset;
    std::generate(expected.begin(), expected.end(), [&] { return PatternByte(index++); });
    rec.contentMatch = rec.hr == S_OK &&
                       std::equal(buffer.begin(), buffer.begin() + got, expected.begin());
    rec.tailZeroed    = TailIsAll(buffer, got, 0);
    rec.tailUntouched = TailIsAll(buffer, got, kSentinel);

    rec.dirEntry = ListRootEntry(mount, fileName);
    rec.path     = QueryPath(mount, overlayPath);
    return rec;
}

struct SizeReport {
    LM_FILE_INFO openInfo{};
    LM_FILE_INFO handleInfo{};
    LM_FILE_INFO dirInfo{};
    PathQuery    path;
};

SizeReport SizesOf(const CellRecord& rec) {
    return SizeReport{rec.openInfo, rec.handleInfo, rec.dirEntry.info, rec.path};
}

void AppendSizeFields(std::wstring& line, const SizeReport& sizes) {
    line += L" alloc-open=" + std::to_wstring(sizes.openInfo.allocationSize);
    line += L" alloc-handle=" + std::to_wstring(sizes.handleInfo.allocationSize);
    line += L" alloc-dir=" + std::to_wstring(sizes.dirInfo.allocationSize);
    line += L" alloc-path=" + std::to_wstring(sizes.path.allocationSize);
    line += L" size-open=" + std::to_wstring(sizes.openInfo.fileSize);
    line += L" size-handle=" + std::to_wstring(sizes.handleInfo.fileSize);
    line += L" size-dir=" + std::to_wstring(sizes.dirInfo.fileSize);
    line += L" size-path=" + std::to_wstring(sizes.path.fileSize);
}

std::wstring CellLine(const MatrixCell& cell, const ReadRequest& request,
                      const CellRecord& rec) {
    std::wstring line = CellTag(cell);
    line += L" offset=" + std::to_wstring(request.offset);
    line += L" length=" + std::to_wstring(request.length);
    line += L" hr=" + Hex(static_cast<unsigned>(rec.hr));
    line += L" count=" + std::to_wstring(rec.count);
    line += L" content=";
    line += rec.hr == S_OK ? (rec.contentMatch ? L"1" : L"0") : L"-";
    line += L" tail-zeroed=" + std::to_wstring(rec.tailZeroed ? 1 : 0);
    line += L" tail-untouched=" + std::to_wstring(rec.tailUntouched ? 1 : 0);
    AppendSizeFields(line, SizesOf(rec));
    line += L" source=" + std::to_wstring(static_cast<int>(rec.path.source));
    line += L" attrs=" + Hex(rec.path.attributes);
    return line;
}

std::wstring PreReadLine(UINT64 listedSize, const SizeReport& sizes) {
    std::wstring line = L"origin=";
    line += OriginName(Origin::MetacopyShell);
    line += L" size=" + std::to_wstring(listedSize);
    line += L" shape=pre-read";
    AppendSizeFields(line, sizes);
    return line;
}

std::wstring SkipLine(const MatrixCell& cell) {
    return CellTag(cell) + L" not-applicable";
}

void AssertReadShape(const MatrixCell& cell, const ReadRequest& request,
                     const CellRecord& rec) {
    switch (cell.shape) {
    case ReadShape::Inside:
        Assert::AreEqual<HRESULT>(S_OK, rec.hr, FailMessage(cell, L"inside read status").c_str());
        Assert::AreEqual<UINT32>(request.length, rec.count,
                                 FailMessage(cell, L"inside read count").c_str());
        Assert::IsTrue(rec.contentMatch, FailMessage(cell, L"inside read content").c_str());
        break;
    case ReadShape::Straddle:
        Assert::AreEqual<HRESULT>(S_OK, rec.hr, FailMessage(cell, L"straddle read status").c_str());
        Assert::AreEqual<UINT32>(static_cast<UINT32>(cell.stagedSize - request.offset), rec.count,
                                 FailMessage(cell, L"straddle read count").c_str());
        Assert::IsTrue(rec.contentMatch, FailMessage(cell, L"straddle read content").c_str());
        Assert::IsTrue(rec.tailZeroed, FailMessage(cell, L"straddle read tail zero-filled").c_str());
        break;
    case ReadShape::AtEnd:
        Assert::IsTrue(IsEndOfFileHr(rec.hr), FailMessage(cell, L"at-end read status").c_str());
        Assert::AreEqual<UINT32>(0u, rec.count, FailMessage(cell, L"at-end read count").c_str());
        Assert::IsTrue(rec.tailUntouched, FailMessage(cell, L"at-end read tail").c_str());
        break;
    }
}

void AssertSizesAgree(const MatrixCell& cell, const CellRecord& rec) {
    const UINT64 fileSize = cell.stagedSize;
    Assert::AreEqual<UINT64>(fileSize, rec.openInfo.fileSize,
                             FailMessage(cell, L"open file size").c_str());
    Assert::AreEqual<UINT64>(fileSize, rec.handleInfo.fileSize,
                             FailMessage(cell, L"handle file size").c_str());
    Assert::IsTrue(rec.dirEntry.found, FailMessage(cell, L"directory listing entry").c_str());
    Assert::AreEqual<UINT64>(fileSize, rec.dirEntry.info.fileSize,
                             FailMessage(cell, L"directory file size").c_str());
    Assert::AreEqual<UINT64>(fileSize, rec.path.fileSize,
                             FailMessage(cell, L"path query file size").c_str());
}

// The matrix stages no preallocation, so the rounded file size is the exact
// value every producer must report.
void AssertOneAllocation(const MatrixCell& cell, const CellRecord& rec) {
    const UINT64 expected = RoundUpToPage(cell.stagedSize);
    Assert::AreEqual<UINT64>(expected, rec.openInfo.allocationSize,
                             FailMessage(cell, L"open allocation size").c_str());
    Assert::AreEqual<UINT64>(expected, rec.handleInfo.allocationSize,
                             FailMessage(cell, L"handle allocation size").c_str());
    Assert::AreEqual<UINT64>(expected, rec.dirEntry.info.allocationSize,
                             FailMessage(cell, L"directory allocation size").c_str());
    Assert::AreEqual<UINT64>(expected, rec.path.allocationSize,
                             FailMessage(cell, L"path query allocation size").c_str());
}

void AssertPathResolves(const MatrixCell& cell, const CellRecord& rec) {
    Assert::AreEqual<HRESULT>(S_OK, rec.path.hr, FailMessage(cell, L"resolve path status").c_str());
    const LM_LAYER_SOURCE expectedSource =
        cell.origin == Origin::Lower ? LM_LAYER_LOWER : LM_LAYER_UPPER;
    Assert::AreEqual<int>(static_cast<int>(expectedSource), static_cast<int>(rec.path.source),
                          FailMessage(cell, L"resolve path source").c_str());
    Assert::IsTrue(rec.path.attributes != INVALID_FILE_ATTRIBUTES,
                   FailMessage(cell, L"resolve path attributes").c_str());
}

void AssertObservedBehavior(const MatrixCell& cell, const ReadRequest& request,
                            const CellRecord& rec) {
    AssertReadShape(cell, request, rec);
    AssertSizesAgree(cell, rec);
    AssertOneAllocation(cell, rec);
    AssertPathResolves(cell, rec);
}

void StageShell(LM_HANDLE mount, UINT64 listedSize) {
    const WritableOpenRecord rec =
        OpenWritableAndClose(mount, OverlayPath(Origin::MetacopyShell, listedSize));
    const DirectoryEntry dirEntry =
        ListRootEntry(mount, FileName(Origin::MetacopyShell, listedSize));
    const PathQuery path = QueryPath(mount, OverlayPath(Origin::MetacopyShell, listedSize));
    Logger::WriteMessage(
        PreReadLine(listedSize, SizeReport{rec.openInfo, rec.handleInfo, dirEntry.info, path})
            .c_str());
    Assert::IsTrue((rec.openInfo.fileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0,
                   (L"the staged file is a metacopy shell, size " +
                    std::to_wstring(listedSize)).c_str());
}

void StageThroughMount(LM_HANDLE mount, Origin origin, UINT64 listedSize) {
    switch (origin) {
    case Origin::UpperThroughMount:
        CreateThroughMount(mount, OverlayPath(origin, listedSize), PatternBytes(listedSize));
        break;
    case Origin::LowerCopiedUp:
        (void)OpenWritableAndClose(mount, OverlayPath(origin, listedSize));
        break;
    case Origin::MetacopyShell:
        StageShell(mount, listedSize);
        break;
    case Origin::UpperBeforeMount:
    case Origin::Lower:
        break;
    }
}

void RunMatrixFor(Origin origin) {
    TempLayerEnv env(1);
    StageBeforeMount(env, origin);
    LayerMountHolder mount = CreateLayerMount(env);

    for (UINT64 listed : kSizes) {
        StageThroughMount(mount.Get(), origin, listed);
    }

    for (UINT64 listed : kSizes) {
        for (ReadShape shape : {ReadShape::Inside, ReadShape::Straddle, ReadShape::AtEnd}) {
            const MatrixCell  cell{origin, listed, StagedSize(origin, listed), shape};
            const ReadRequest request = ShapeFor(shape, cell.stagedSize);
            if (!request.applicable) {
                Logger::WriteMessage(SkipLine(cell).c_str());
                continue;
            }
            const CellRecord rec = ReadCell(mount.Get(), cell, request);
            Logger::WriteMessage(CellLine(cell, request, rec).c_str());
            AssertObservedBehavior(cell, request, rec);
        }
    }
}

}

TEST_CLASS(AbiPagingReadMatrixTests) {
public:
    TEST_METHOD(UpperThroughMount_ShortReadZeroFillsTailAndSizesAgree) {
        RunMatrixFor(Origin::UpperThroughMount);
    }

    TEST_METHOD(MetacopyShell_ShortReadZeroFillsTailAndSizesAgree) {
        RunMatrixFor(Origin::MetacopyShell);
    }

    TEST_METHOD(LowerCopiedUp_ShortReadZeroFillsTailAndSizesAgree) {
        RunMatrixFor(Origin::LowerCopiedUp);
    }

    TEST_METHOD(UpperBeforeMount_ShortReadZeroFillsTailAndSizesAgree) {
        RunMatrixFor(Origin::UpperBeforeMount);
    }

    TEST_METHOD(Lower_ShortReadZeroFillsTailAndSizesAgree) {
        RunMatrixFor(Origin::Lower);
    }

    TEST_METHOD(Preallocation_AboveRounding_StaysVisibleOnHandle) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), L"\\prealloc.bin", 0u,
                GENERIC_READ | GENERIC_WRITE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fh, &info));

        const char   payload[]  = "short";
        const UINT32 payloadLen = static_cast<UINT32>(sizeof(payload) - 1);
        UINT32       written    = 0;
        LM_FILE_INFO postWrite{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountWriteFile(fh, payload, 0, payloadLen, FALSE, FALSE, 0u,
                                  &written, &postWrite));

        const UINT64 requested = 64u * 1024u;
        LM_FILE_INFO postSet{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountSetFileInfo(
                fh,
                INVALID_FILE_ATTRIBUTES,
                /*creationTime*/   0,
                /*lastAccessTime*/ 0,
                /*lastWriteTime*/  0,
                /*changeTime*/     0,
                /*allocationSize*/ requested,
                /*fileSize*/       UINT64_MAX,
                &postSet));

        LM_FILE_INFO afterSet{};
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountGetFileInfo(fh, &afterSet));
        Assert::AreEqual<UINT64>(payloadLen, afterSet.fileSize,
            L"the file size stays at the bytes written");
        Assert::IsTrue(afterSet.allocationSize >= requested,
            (L"the handle reports the requested allocation or more, got " +
             std::to_wstring(afterSet.allocationSize)).c_str());
        Assert::IsTrue(postSet.allocationSize >= requested,
            (L"the set-info result reports the requested allocation or more, got " +
             std::to_wstring(postSet.allocationSize)).c_str());

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }
};

}
