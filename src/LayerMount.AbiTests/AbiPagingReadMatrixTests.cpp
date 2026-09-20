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

constexpr UINT32 kCopyUpAccess = GENERIC_READ | GENERIC_WRITE;

// HRESULT_FROM_NT(STATUS_END_OF_FILE), the form the ABI returns for a read
// at or past the end of the file.
constexpr HRESULT kHrEndOfFileNt = static_cast<HRESULT>(0xD0000011);

// HRESULT_FROM_NT(STATUS_OBJECT_NAME_NOT_FOUND), the form the ABI returns
// when a fill finds no origin file.
constexpr HRESULT kHrObjectNameNotFoundNt = static_cast<HRESULT>(0xD0000034);

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

struct OriginTraits {
    bool stagesOnUpper;
    bool isShell;
};

OriginTraits TraitsOf(Origin origin) {
    switch (origin) {
    case Origin::UpperThroughMount: return {true, false};
    case Origin::MetacopyShell:     return {true, true};
    case Origin::LowerCopiedUp:     return {true, false};
    case Origin::UpperBeforeMount:  return {true, false};
    case Origin::Lower:             return {false, false};
    }
    return {false, false};
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

// True when the first `count` bytes of `buffer` are the pattern bytes
// that start at file offset `offset`.
bool MatchesPattern(const std::vector<BYTE>& buffer, UINT32 count, UINT64 offset) {
    std::vector<BYTE> expected(count);
    UINT64            index = offset;
    std::generate(expected.begin(), expected.end(), [&] { return PatternByte(index++); });
    return std::equal(buffer.begin(), buffer.begin() + count, expected.begin());
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

std::wstring UpperPathOf(const TempLayerEnv& env, Origin origin, UINT64 listedSize) {
    return env.Upper() + L"\\" + FileName(origin, listedSize);
}

std::wstring LowerPathOf(const TempLayerEnv& env, Origin origin, UINT64 listedSize) {
    return env.Lower(0) + L"\\" + FileName(origin, listedSize);
}

// The shell probes measure a sparse file, and the volume allocates a sparse
// file's clusters only when cached writes reach the disk, so the probe
// flushes the file first. For a file that is not sparse and not compressed
// the volume reports the file size, not the cluster-rounded allocation.
UINT64 FlushAndMeasureAllocation(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Assert::IsTrue(h != INVALID_HANDLE_VALUE, (L"open for flush " + path).c_str());
    Assert::IsTrue(::FlushFileBuffers(h) != FALSE, (L"FlushFileBuffers " + path).c_str());
    ::CloseHandle(h);
    DWORD high = 0;
    const DWORD low = ::GetCompressedFileSizeW(path.c_str(), &high);
    Assert::IsTrue(low != INVALID_FILE_SIZE || ::GetLastError() == NO_ERROR,
                   (L"GetCompressedFileSizeW " + path).c_str());
    return (static_cast<UINT64>(high) << 32) | low;
}

struct UpperProbe {
    UINT64 allocation = 0;
    UINT32 attributes = INVALID_FILE_ATTRIBUTES;
};

UpperProbe ProbeUpper(const std::wstring& upperPath) {
    UpperProbe probe;
    probe.allocation = FlushAndMeasureAllocation(upperPath);
    probe.attributes = ::GetFileAttributesW(upperPath.c_str());
    return probe;
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

void OpenOrFail(LM_HANDLE mount, const std::wstring& overlayPath, UINT32 grantedAccess,
                const std::wstring& label, FileHandleHolder& fh, LM_FILE_INFO* info) {
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountOpenFile(mount, overlayPath.c_str(),
            grantedAccess,
            /*createOptions*/ 0u, /*originatorPid*/ 0u,
            fh.AddressOf(), info),
        (label + L" LayerMountOpenFile " + overlayPath).c_str());
}

struct OpenRecord {
    LM_FILE_INFO openInfo{};
    LM_FILE_INFO handleInfo{};
};

OpenRecord OpenAndClose(LM_HANDLE mount, const std::wstring& overlayPath,
                        UINT32 grantedAccess) {
    FileHandleHolder fh;
    OpenRecord       rec;
    OpenOrFail(mount, overlayPath, grantedAccess, L"staging", fh, &rec.openInfo);
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountGetFileInfo(fh.Get(), &rec.handleInfo),
        (L"LayerMountGetFileInfo after open " + overlayPath).c_str());
    return rec;
}

struct MountedEnv {
    const TempLayerEnv& env;
    LM_HANDLE           mount;
};

struct CellRecord {
    HRESULT        hr            = E_FAIL;
    UINT32         count         = 0;
    bool           contentMatch  = false;
    bool           tailZeroed    = false;
    bool           tailUntouched = false;
    LM_FILE_INFO   openInfo{};
    LM_FILE_INFO   handleInfo{};
    UpperProbe     upperBeforeRead;
    DirectoryEntry dirEntry;
    PathQuery      path;
};

bool TailIsAll(const std::vector<BYTE>& buffer, UINT32 from, BYTE value) {
    return std::all_of(buffer.begin() + from, buffer.end(),
                       [value](BYTE b) { return b == value; });
}

CellRecord ReadCell(const MountedEnv& mounted, const MatrixCell& cell,
                    const ReadRequest& request) {
    const std::wstring overlayPath = OverlayPath(cell.origin, cell.listedSize);
    const std::wstring fileName    = FileName(cell.origin, cell.listedSize);
    const LM_HANDLE    mount       = mounted.mount;
    CellRecord rec;

    FileHandleHolder fh;
    OpenOrFail(mount, overlayPath, GENERIC_READ, L"read", fh, &rec.openInfo);
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountGetFileInfo(fh.Get(), &rec.handleInfo),
        (L"LayerMountGetFileInfo before read " + overlayPath).c_str());
    if (TraitsOf(cell.origin).stagesOnUpper) {
        rec.upperBeforeRead =
            ProbeUpper(UpperPathOf(mounted.env, cell.origin, cell.listedSize));
    }

    std::vector<BYTE> buffer(request.length, kSentinel);
    rec.hr = ::LayerMountReadFile(fh.Get(), buffer.data(), request.offset,
                                  request.length, /*originatorPid*/ 0u, &rec.count);
    fh.Reset();

    const UINT32 got = (std::min)(rec.count, request.length);
    rec.contentMatch = rec.hr == S_OK && MatchesPattern(buffer, got, request.offset);
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
    line += L" alloc-upper-pre-read=" + std::to_wstring(rec.upperBeforeRead.allocation);
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

struct ReportedAllocation {
    UINT64         value;
    const wchar_t* producer;
};

void AssertOneAllocation(const MatrixCell& cell, const CellRecord& rec) {
    const UINT64             expected   = RoundUpToPage(cell.stagedSize);
    const ReportedAllocation reported[] = {
        {rec.openInfo.allocationSize, L"open allocation size"},
        {rec.handleInfo.allocationSize, L"handle allocation size"},
        {rec.dirEntry.info.allocationSize, L"directory allocation size"},
        {rec.path.allocationSize, L"path query allocation size"},
    };
    for (const ReportedAllocation& r : reported) {
        Assert::AreEqual<UINT64>(expected, r.value, FailMessage(cell, r.producer).c_str());
    }
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

void AssertShellFilledAtOpen(const MatrixCell& cell, const CellRecord& rec) {
    if (!TraitsOf(cell.origin).isShell) return;
    Assert::IsTrue(rec.upperBeforeRead.allocation >= cell.stagedSize,
                   FailMessage(cell, L"upper allocation before the read").c_str());
    Assert::IsTrue((rec.openInfo.fileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) == 0,
                   FailMessage(cell, L"open for read reports no sparse attribute").c_str());
    Assert::IsTrue(rec.upperBeforeRead.attributes != INVALID_FILE_ATTRIBUTES &&
                   (rec.upperBeforeRead.attributes & FILE_ATTRIBUTE_SPARSE_FILE) == 0,
                   FailMessage(cell, L"upper path has no sparse attribute").c_str());
}

void AssertObservedBehavior(const MatrixCell& cell, const ReadRequest& request,
                            const CellRecord& rec) {
    AssertReadShape(cell, request, rec);
    AssertSizesAgree(cell, rec);
    AssertOneAllocation(cell, rec);
    AssertPathResolves(cell, rec);
    AssertShellFilledAtOpen(cell, rec);
}

void AssertShellIsSparse(const std::wstring& upperPath, UINT64 fileSize, const wchar_t* what) {
    const UINT64 allocated = FlushAndMeasureAllocation(upperPath);
    Assert::IsTrue(allocated < fileSize,
                   (std::wstring(what) + L": upper allocates " + std::to_wstring(allocated) +
                    L" of " + std::to_wstring(fileSize)).c_str());
}

// Sets the end of file on `fh` and fails the test unless the call
// succeeds and reports the new size; `what` names the handle in the
// failure message.
void SetSizeOrFail(LM_FILE_HANDLE fh, UINT64 newSize, const wchar_t* what) {
    LM_FILE_INFO postSet{};
    Assert::AreEqual<HRESULT>(S_OK, SetFileSize(fh, newSize, &postSet),
                              (L"a set-size on the " + std::wstring(what) + L" handle").c_str());
    Assert::AreEqual<UINT64>(newSize, postSet.fileSize,
                             L"the set-info result reports the new size");
}

void AssertLowerKeepsStagedSize(const TempLayerEnv& env, UINT64 listedSize) {
    Assert::AreEqual<UINT64>(StagedSize(Origin::MetacopyShell, listedSize),
        ReadAllBytes(LowerPathOf(env, Origin::MetacopyShell, listedSize)).size(),
        L"the lower file keeps its size");
}

void SetSizeThenReadEndPage(const MountedEnv& mounted, Origin origin, UINT64 listedSize,
                            UINT64 newSize) {
    const std::wstring overlayPath = OverlayPath(origin, listedSize);
    const std::wstring upperPath   = UpperPathOf(mounted.env, origin, listedSize);
    FileHandleHolder   fh;
    LM_FILE_INFO       info{};
    OpenOrFail(mounted.mount, overlayPath, GENERIC_WRITE, L"write-only", fh, &info);
    SetSizeOrFail(fh.Get(), newSize, L"write-only");

    const UINT64      pageStart = (newSize / kPageBytes) * kPageBytes;
    std::vector<BYTE> buffer(static_cast<size_t>(kPageBytes), kSentinel);
    UINT32            count = 0;
    const HRESULT     hr    = ::LayerMountReadFile(fh.Get(), buffer.data(), pageStart,
        static_cast<UINT32>(kPageBytes), /*originatorPid*/ 0u, &count);
    Assert::AreEqual<HRESULT>(S_OK, hr,
        (L"a read of the page at the new end of file on the write-only handle, got " +
         Hex(static_cast<unsigned>(hr))).c_str());
    Assert::AreEqual<UINT32>(static_cast<UINT32>(newSize - pageStart), count,
                             L"the read returns the bytes up to the new end of file");
    Assert::IsTrue(buffer[0] == PatternByte(pageStart),
                   L"the read returns the file's bytes");
    Assert::IsTrue(TailIsAll(buffer, count, 0),
                   L"the engine zero-fills the tail past the new end of file");
    fh.Reset();

    const std::string onDisk = ReadAllBytes(upperPath);
    Assert::IsTrue(onDisk == PatternBytes(newSize),
                   (L"the upper file on disk holds the first " + std::to_wstring(newSize) +
                    L" bytes, got " + std::to_wstring(onDisk.size())).c_str());
}

void StageShell(const MountedEnv& mounted, UINT64 listedSize) {
    const LM_HANDLE  mount = mounted.mount;
    const OpenRecord rec   = OpenAndClose(
        mount, OverlayPath(Origin::MetacopyShell, listedSize), kAttributeOnlyAccess);
    const DirectoryEntry dirEntry =
        ListRootEntry(mount, FileName(Origin::MetacopyShell, listedSize));
    const PathQuery path = QueryPath(mount, OverlayPath(Origin::MetacopyShell, listedSize));
    Logger::WriteMessage(
        PreReadLine(listedSize, SizeReport{rec.openInfo, rec.handleInfo, dirEntry.info, path})
            .c_str());
    Assert::IsTrue((rec.openInfo.fileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0,
                   (L"the staged file is a metacopy shell, size " +
                    std::to_wstring(listedSize)).c_str());
    AssertShellIsSparse(UpperPathOf(mounted.env, Origin::MetacopyShell, listedSize),
                        StagedSize(Origin::MetacopyShell, listedSize),
                        L"an attribute-only open keeps the shell sparse");
}

void StageThroughMount(const MountedEnv& mounted, Origin origin, UINT64 listedSize) {
    switch (origin) {
    case Origin::UpperThroughMount:
        CreateThroughMount(mounted.mount, OverlayPath(origin, listedSize),
                           PatternBytes(listedSize));
        break;
    case Origin::LowerCopiedUp:
        (void)OpenAndClose(mounted.mount, OverlayPath(origin, listedSize), kCopyUpAccess);
        break;
    case Origin::MetacopyShell:
        StageShell(mounted, listedSize);
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
    const MountedEnv mounted{env, mount.Get()};

    for (UINT64 listed : kSizes) {
        StageThroughMount(mounted, origin, listed);
    }

    for (UINT64 listed : kSizes) {
        for (ReadShape shape : {ReadShape::Inside, ReadShape::Straddle, ReadShape::AtEnd}) {
            const MatrixCell  cell{origin, listed, StagedSize(origin, listed), shape};
            const ReadRequest request = ShapeFor(shape, cell.stagedSize);
            if (!request.applicable) {
                Logger::WriteMessage(SkipLine(cell).c_str());
                continue;
            }
            const CellRecord rec = ReadCell(mounted, cell, request);
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

    TEST_METHOD(MetacopyShell_ReadOnAttributeOnlyHandle_FailsAndKeepsShellSparse) {
        constexpr UINT64 listed = 0;
        TempLayerEnv     env(1);
        StageBeforeMount(env, Origin::MetacopyShell);
        LayerMountHolder mount = CreateLayerMount(env);

        FileHandleHolder fh;
        LM_FILE_INFO     info{};
        OpenOrFail(mount.Get(), OverlayPath(Origin::MetacopyShell, listed),
                   kAttributeOnlyAccess, L"attribute-only", fh, &info);
        Assert::IsTrue((info.fileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0,
                       L"the attribute-only open staged a metacopy shell");

        std::vector<BYTE> buffer(static_cast<size_t>(kPageBytes), kSentinel);
        UINT32            count = 0;
        const HRESULT     hr    = ::LayerMountReadFile(fh.Get(), buffer.data(), /*offset*/ 0,
            static_cast<UINT32>(kPageBytes), /*originatorPid*/ 0u, &count);
        Assert::IsTrue(FAILED(hr),
                       (L"a read on a handle with no read-data right fails, got " +
                        Hex(static_cast<unsigned>(hr))).c_str());
        AssertShellIsSparse(UpperPathOf(env, Origin::MetacopyShell, listed),
                            StagedSize(Origin::MetacopyShell, listed),
                            L"the read path does not fill the shell");
    }

    TEST_METHOD(UpperThroughMount_SetSizeOnWriteOnlyHandle_ReadSucceedsAndUpperShrinks) {
        constexpr UINT64 listed  = 65536;
        constexpr UINT64 newSize = kPageBytes + 1;
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreateThroughMount(mount.Get(), OverlayPath(Origin::UpperThroughMount, listed),
                           PatternBytes(listed));

        SetSizeThenReadEndPage(MountedEnv{env, mount.Get()}, Origin::UpperThroughMount,
                               listed, newSize);
    }

    TEST_METHOD(MetacopyShell_SetSizeOnWriteOnlyHandle_ReadSucceedsAndUpperShrinks) {
        constexpr UINT64 listed  = kPageBytes + 1;
        constexpr UINT64 newSize = kPageBytes + 1;
        TempLayerEnv     env(1);
        StageBeforeMount(env, Origin::MetacopyShell);
        LayerMountHolder mount = CreateLayerMount(env);

        SetSizeThenReadEndPage(MountedEnv{env, mount.Get()}, Origin::MetacopyShell,
                               listed, newSize);
        AssertLowerKeepsStagedSize(env, listed);
    }

    TEST_METHOD(MetacopyShell_SetSizeOnAttributeOnlyHandle_FillsAndReadOpenSeesNewSize) {
        constexpr UINT64 listed  = kPageBytes + 1;
        constexpr UINT64 newSize = kPageBytes + 1;
        TempLayerEnv     env(1);
        StageBeforeMount(env, Origin::MetacopyShell);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring overlayPath = OverlayPath(Origin::MetacopyShell, listed);

        FileHandleHolder fh;
        LM_FILE_INFO     info{};
        OpenOrFail(mount.Get(), overlayPath, kAttributeOnlyAccess, L"attribute-only", fh, &info);
        Assert::IsTrue((info.fileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0,
                       L"the attribute-only open staged a metacopy shell");

        SetSizeOrFail(fh.Get(), newSize, L"attribute-only");
        fh.Reset();

        FileHandleHolder reader;
        LM_FILE_INFO     readInfo{};
        OpenOrFail(mount.Get(), overlayPath, GENERIC_READ, L"read", reader, &readInfo);
        Assert::AreEqual<UINT64>(newSize, readInfo.fileSize,
                                 L"the open for read reports the new size");
        std::vector<BYTE> buffer(static_cast<size_t>(2 * kPageBytes), kSentinel);
        UINT32            count = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountReadFile(reader.Get(), buffer.data(), /*offset*/ 0,
                static_cast<UINT32>(buffer.size()), /*originatorPid*/ 0u, &count),
            L"a read on the reopened handle");
        Assert::AreEqual<UINT32>(static_cast<UINT32>(newSize), count,
                                 L"the read returns the bytes up to the new end of file");
        Assert::IsTrue(MatchesPattern(buffer, count, /*offset*/ 0),
                       L"the read returns the lower's bytes up to the new end of file");
        reader.Reset();

        Assert::AreEqual<UINT64>(newSize,
            ReadAllBytes(UpperPathOf(env, Origin::MetacopyShell, listed)).size(),
            L"the upper file on disk holds the new size");
        AssertLowerKeepsStagedSize(env, listed);
    }

    TEST_METHOD(MetacopyShell_SetTimesOnAttributeOnlyHandle_KeepsShellSparse) {
        constexpr UINT64 listed = kPageBytes + 1;
        TempLayerEnv     env(1);
        StageBeforeMount(env, Origin::MetacopyShell);
        LayerMountHolder mount = CreateLayerMount(env);

        FileHandleHolder fh;
        LM_FILE_INFO     info{};
        OpenOrFail(mount.Get(), OverlayPath(Origin::MetacopyShell, listed),
                   kAttributeOnlyAccess, L"attribute-only", fh, &info);

        constexpr UINT64 lastWriteTime = 132000000000000000ull;
        LM_FILE_INFO     postSet{};
        Assert::AreEqual<HRESULT>(S_OK, SetLastWriteTime(fh.Get(), lastWriteTime, &postSet),
                                  L"a set-times on the attribute-only handle");
        Assert::AreEqual<UINT64>(lastWriteTime, postSet.lastWriteTime,
                                 L"the set-info result reports the new time");
        fh.Reset();

        AssertShellIsSparse(UpperPathOf(env, Origin::MetacopyShell, listed),
                            StagedSize(Origin::MetacopyShell, listed),
                            L"a set-times keeps the shell sparse");
    }

    TEST_METHOD(MetacopyShell_SetTimesOnAttributeOnlyHandle_ReadOpenFillsAndKeepsSetTime) {
        constexpr UINT64 listed = kPageBytes + 1;
        TempLayerEnv     env(1);
        StageBeforeMount(env, Origin::MetacopyShell);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring overlayPath = OverlayPath(Origin::MetacopyShell, listed);

        FileHandleHolder fh;
        LM_FILE_INFO     info{};
        OpenOrFail(mount.Get(), overlayPath, kAttributeOnlyAccess, L"attribute-only", fh, &info);

        constexpr UINT64 lastWriteTime = 132000000000000000ull;
        LM_FILE_INFO     postSet{};
        Assert::AreEqual<HRESULT>(S_OK, SetLastWriteTime(fh.Get(), lastWriteTime, &postSet),
                                  L"a set-times on the attribute-only handle");
        Assert::AreEqual<UINT64>(lastWriteTime, postSet.lastWriteTime,
                                 L"the set-info result reports the new time");
        fh.Reset();

        FileHandleHolder reader;
        LM_FILE_INFO     readInfo{};
        OpenOrFail(mount.Get(), overlayPath, GENERIC_READ, L"read", reader, &readInfo);
        Assert::AreEqual<UINT64>(lastWriteTime, readInfo.lastWriteTime,
                                 L"the open for read reports the set write time, not the lower's");
        reader.Reset();

        const UINT64      stagedSize = StagedSize(Origin::MetacopyShell, listed);
        const std::string onDisk = ReadAllBytes(UpperPathOf(env, Origin::MetacopyShell, listed));
        Assert::AreEqual<UINT64>(stagedSize, onDisk.size(),
                                 L"the upper file on disk holds the staged size");
        Assert::IsTrue(onDisk == PatternBytes(stagedSize),
                       L"the open for read filled the shell with the lower's bytes");
        AssertLowerKeepsStagedSize(env, listed);
    }

    TEST_METHOD(MetacopyShell_OpenForReadWithOriginMissing_FailsAndReturnsNoHandle) {
        constexpr UINT64 listed = 0;
        TempLayerEnv     env(1);
        StageBeforeMount(env, Origin::MetacopyShell);
        LayerMountHolder mount = CreateLayerMount(env);
        StageShell(MountedEnv{env, mount.Get()}, listed);

        Assert::IsTrue(
            ::DeleteFileW(LowerPathOf(env, Origin::MetacopyShell, listed).c_str()) != FALSE,
            L"the lower origin is removable");

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        const HRESULT  hr = ::LayerMountOpenFile(mount.Get(),
            OverlayPath(Origin::MetacopyShell, listed).c_str(),
            /*grantedAccess*/ GENERIC_READ,
            /*createOptions*/ 0u, /*originatorPid*/ 0u, &fh, &info);
        Assert::AreEqual<HRESULT>(kHrObjectNameNotFoundNt, hr,
                                  L"the open for read data fails with the fill's status");
        Assert::IsNull(fh, L"a failed open returns no handle");
        AssertShellIsSparse(UpperPathOf(env, Origin::MetacopyShell, listed),
                            StagedSize(Origin::MetacopyShell, listed),
                            L"a failed fill leaves the shell sparse");
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
