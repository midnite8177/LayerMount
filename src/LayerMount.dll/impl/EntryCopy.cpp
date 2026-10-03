#include "EntryCopy.h"
#include "LayerPath.h"
#include "MetadataStore.h"
#include "NtStatusUtil.h"

#include <winioctl.h>
#include <climits>
#include <iterator>
#include <string_view>

#pragma comment(lib, "advapi32.lib")

namespace LayerMount {

namespace {

uint64_t MakeIndexNumber(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<uint64_t>(info.nFileIndexHigh) << 32) |
           info.nFileIndexLow;
}

bool TryGetStableIndexNumberFromHandle(HANDLE h, uint64_t& outIndexNumber) {
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h, &info)) {
        return false;
    }

    outIndexNumber = MakeIndexNumber(info);
    return true;
}

bool TryGetStableIndexNumberFromPath(const std::wstring& path,
                                     uint64_t& outIndexNumber) {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    DWORD flags = 0;
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        flags |= FILE_FLAG_BACKUP_SEMANTICS;
    }
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }

    HANDLE h = ::CreateFileW(path.c_str(),
                             FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE,
                             nullptr,
                             OPEN_EXISTING,
                             flags,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    const bool ok = TryGetStableIndexNumberFromHandle(h, outIndexNumber);
    ::CloseHandle(h);
    return ok;
}

void AddSourceFileId(LayerMountMetadata& metadata,
                     const std::wstring& sourcePath) {
    uint64_t stableIndexNumber = 0;
    if (TryGetStableIndexNumberFromPath(sourcePath, stableIndexNumber)) {
        metadata.hasStableIndexNumber = true;
        metadata.stableIndexNumber = stableIndexNumber;
    }
}

LayerMountMetadata CarriedCopyUpMetadata(const std::wstring& sourcePath,
                                         const LayerConfig& config) {
    LayerMountMetadata metadata =
        MetadataStore::ReadLayerMountMetadata(sourcePath, &config);
    if (!metadata.hasStableIndexNumber) {
        AddSourceFileId(metadata, sourcePath);
    }
    return metadata;
}

// NTFS stream names come back as ":name:$DATA" or ":name:$<type>".
bool IsReservedLayerMountStream(const std::wstring& streamName) {
    static constexpr std::wstring_view kLayerMount(L":overlay");
    if (streamName.size() < kLayerMount.size()) return false;
    // Compares a lowercase copy, because NTFS stream names are
    // case-insensitive.
    std::wstring lower(streamName.begin(), streamName.begin() + kLayerMount.size());
    ::CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    return lower.compare(0, kLayerMount.size(), kLayerMount) == 0;
}

// srcPath and dstPath are the full paths to the underlying files; streamName is the `:name:$<T>`
// suffix as returned by FindFirstStreamW / FILE_STREAM_INFO.StreamName.
//
// Both opens use FILE_FLAG_BACKUP_SEMANTICS. With SE_BACKUP_NAME and
// SE_RESTORE_NAME, which EnableFileSystemPrivileges enables, that bypasses
// DACL checks, so a lower file inside a directory whose DACL denies our
// process (e.g. an inherited DENY-WRITE for Everyone) can still have its ADS
// copied up. Without backup semantics the destination open would fail because the
// just-committed upper file inherits the same restrictive DACL from its
// parent shadow, and silent ADS loss would result.
bool CopyAlternateStream(const std::wstring& srcPath,
                         const std::wstring& dstPath,
                         const std::wstring& streamName) {
    const std::wstring srcFull = srcPath + streamName;
    const std::wstring dstFull = dstPath + streamName;

    HANDLE srcH = ::CreateFileW(srcFull.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN |
                                      FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if (srcH == INVALID_HANDLE_VALUE) return false;

    HANDLE dstH = ::CreateFileW(dstFull.c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL |
                                      FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if (dstH == INVALID_HANDLE_VALUE) {
        ::CloseHandle(srcH);
        return false;
    }

    BYTE buf[64 * 1024];
    bool ok = true;
    for (;;) {
        DWORD r = 0;
        if (!::ReadFile(srcH, buf, sizeof(buf), &r, nullptr)) {
            ok = false;
            break;
        }
        if (r == 0) break;
        DWORD w = 0;
        if (!::WriteFile(dstH, buf, r, &w, nullptr) || w != r) {
            ok = false;
            break;
        }
    }

    ::CloseHandle(srcH);
    ::CloseHandle(dstH);

    if (!ok) {
        ::DeleteFileW(dstFull.c_str());
    }
    return ok;
}

// A refusal is ignored: a Windows copy
// of a compressed file to a volume that cannot compress gives a dense file
// and no error. Call it before the data copy; after it, NTFS rewrites the
// file to compress it.
void SetCompressed(HANDLE handle) {
    USHORT format = COMPRESSION_FORMAT_DEFAULT;
    DWORD bytesReturned = 0;
    DeviceIoControl(handle, FSCTL_SET_COMPRESSION, &format, sizeof(format),
                    nullptr, 0, &bytesReturned, nullptr);
}

void SetCompressedDirectory(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    SetCompressed(handle);
    CloseHandle(handle);
}

constexpr DWORD kCopyBufferSize = 64 * 1024;

// Write length bytes to dstHandle. WriteFile returns success with a count
// below length on a full quota, on a network volume and on some raw
// devices; that write fails with ERROR_WRITE_FAULT. A failed call with no
// last error set maps to the same status, never to success.
NTSTATUS WriteChunk(HANDLE dstHandle, const BYTE* buffer, DWORD length) {
    DWORD bytesWritten = 0;
    if (!WriteFile(dstHandle, buffer, length, &bytesWritten, nullptr)) {
        const DWORD err = GetLastError();
        return ::LayerMount::NtStatusFromWin32(err != 0 ? err : ERROR_WRITE_FAULT);
    }
    if (bytesWritten != length) {
        return ::LayerMount::NtStatusFromWin32(ERROR_WRITE_FAULT);
    }
    return STATUS_SUCCESS;
}

// Copy up to limit bytes from the file pointer of srcHandle to the file
// pointer of dstHandle. bytesCopied is below limit when the source ended
// first. Every byte goes through the buffer, so a hole of the source
// becomes written zeros.
NTSTATUS CopyBytes(HANDLE srcHandle, HANDLE dstHandle, LONGLONG limit,
                   LONGLONG& bytesCopied) {
    BYTE buffer[kCopyBufferSize];
    bytesCopied = 0;
    while (bytesCopied < limit) {
        const LONGLONG left = limit - bytesCopied;
        const DWORD chunk = left < kCopyBufferSize ? static_cast<DWORD>(left)
                                                   : kCopyBufferSize;
        DWORD bytesRead = 0;
        if (!ReadFile(srcHandle, buffer, chunk, &bytesRead, nullptr)) {
            return ::LayerMount::NtStatusFromWin32(GetLastError());
        }
        if (bytesRead == 0) {
            break;
        }
        const NTSTATUS status = WriteChunk(dstHandle, buffer, bytesRead);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        bytesCopied += bytesRead;
    }
    return STATUS_SUCCESS;
}

NTSTATUS CopyBytesToEnd(HANDLE srcHandle, HANDLE dstHandle) {
    LONGLONG bytesCopied = 0;
    return CopyBytes(srcHandle, dstHandle, LLONG_MAX, bytesCopied);
}

// A range copied before the call is written again at the same offset.
NTSTATUS CopyBytesFromStart(HANDLE srcHandle, HANDLE dstHandle) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(srcHandle, zero, nullptr, FILE_BEGIN) ||
        !SetFilePointerEx(dstHandle, zero, nullptr, FILE_BEGIN)) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }
    return CopyBytesToEnd(srcHandle, dstHandle);
}

// Copy one allocated range of srcHandle to the same offset of dstHandle.
// bytesCopied is below the range length when the source ended inside it.
NTSTATUS CopyAllocatedRange(HANDLE srcHandle, HANDLE dstHandle,
                            const FILE_ALLOCATED_RANGE_BUFFER& range,
                            LONGLONG& bytesCopied) {
    if (!SetFilePointerEx(srcHandle, range.FileOffset, nullptr, FILE_BEGIN) ||
        !SetFilePointerEx(dstHandle, range.FileOffset, nullptr, FILE_BEGIN)) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }
    return CopyBytes(srcHandle, dstHandle, range.Length.QuadPart, bytesCopied);
}

enum class RangeQuery { Complete, More, Refused };

// Ask srcHandle for the allocated ranges inside query. count receives the
// number of ranges written to ranges. More means the source has ranges
// past the last one returned.
RangeQuery QueryAllocatedRanges(HANDLE srcHandle,
                                FILE_ALLOCATED_RANGE_BUFFER& query,
                                FILE_ALLOCATED_RANGE_BUFFER* ranges,
                                DWORD capacity, DWORD& count) {
    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(srcHandle, FSCTL_QUERY_ALLOCATED_RANGES,
                                    &query, sizeof(query),
                                    ranges, capacity * sizeof(ranges[0]),
                                    &bytesReturned, nullptr);
    count = bytesReturned / sizeof(ranges[0]);
    if (ok) {
        return RangeQuery::Complete;
    }
    return ::GetLastError() == ERROR_MORE_DATA ? RangeQuery::More
                                               : RangeQuery::Refused;
}

// Copy the allocated ranges of srcHandle to the same offsets of dstHandle,
// in order, until the ranges end or the source ends. When the volume
// refuses the range query, rangesRefused is set and the copy stops.
NTSTATUS CopyAllocatedRanges(HANDLE srcHandle, HANDLE dstHandle,
                             LONGLONG srcSize, bool& rangesRefused) {
    rangesRefused = false;
    FILE_ALLOCATED_RANGE_BUFFER query{};
    query.FileOffset.QuadPart = 0;
    query.Length.QuadPart = srcSize;
    while (query.FileOffset.QuadPart < srcSize) {
        FILE_ALLOCATED_RANGE_BUFFER ranges[64];
        DWORD count = 0;
        const RangeQuery result = QueryAllocatedRanges(
            srcHandle, query, ranges, static_cast<DWORD>(std::size(ranges)), count);
        if (result == RangeQuery::Refused) {
            rangesRefused = true;
            return STATUS_SUCCESS;
        }
        for (DWORD i = 0; i < count; ++i) {
            LONGLONG bytesCopied = 0;
            const NTSTATUS status =
                CopyAllocatedRange(srcHandle, dstHandle, ranges[i], bytesCopied);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            if (bytesCopied < ranges[i].Length.QuadPart) {
                return STATUS_SUCCESS;
            }
        }
        if (result == RangeQuery::Complete || count == 0) {
            return STATUS_SUCCESS;
        }
        query.FileOffset.QuadPart = ranges[count - 1].FileOffset.QuadPart +
                                    ranges[count - 1].Length.QuadPart;
        query.Length.QuadPart = srcSize - query.FileOffset.QuadPart;
    }
    return STATUS_SUCCESS;
}

// Set the end of file of dstHandle to size when it differs.
NTSTATUS ExtendToSize(HANDLE dstHandle, LARGE_INTEGER size) {
    LARGE_INTEGER dstSize{};
    if (!GetFileSizeEx(dstHandle, &dstSize)) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }
    if (dstSize.QuadPart == size.QuadPart) {
        return STATUS_SUCCESS;
    }
    if (!SetFilePointerEx(dstHandle, size, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(dstHandle)) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }
    return STATUS_SUCCESS;
}

// A refused attribute query counts as dense, so the copy takes the byte
// path and the saving is lost, never the data.
bool IsSparseHandle(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(handle, &info) &&
           HasFileAttribute(info.dwFileAttributes, FILE_ATTRIBUTE_SPARSE_FILE);
}

NTSTATUS CopyUpReparsePointEntry(const std::wstring& srcAbsolute,
                                 const std::wstring& dstAbsolute,
                                 DWORD srcAttrs) {
    HANDLE srcH = CreateFileW(
        srcAbsolute.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (srcH == INVALID_HANDLE_VALUE) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    BYTE reparseBuf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE]{};
    DWORD returned = 0;
    if (!DeviceIoControl(srcH, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                          reparseBuf, sizeof(reparseBuf), &returned, nullptr)) {
        DWORD err = GetLastError();
        CloseHandle(srcH);
        return ::LayerMount::NtStatusFromWin32(err);
    }
    CloseHandle(srcH);

    const bool isDir = (srcAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (isDir) {
        const NTSTATUS dirStatus = CreateDirectoryOrUseExisting(dstAbsolute);
        if (!NT_SUCCESS(dirStatus)) {
            return dirStatus;
        }
    } else {
        HANDLE dstCreate = CreateFileW(dstAbsolute.c_str(), GENERIC_WRITE, 0,
                                         nullptr, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dstCreate == INVALID_HANDLE_VALUE) {
            return ::LayerMount::NtStatusFromWin32(GetLastError());
        }
        CloseHandle(dstCreate);
    }

    HANDLE dstH = CreateFileW(
        dstAbsolute.c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (dstH == INVALID_HANDLE_VALUE) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    DWORD setReturned = 0;
    if (!DeviceIoControl(dstH, FSCTL_SET_REPARSE_POINT, reparseBuf, returned,
                          nullptr, 0, &setReturned, nullptr)) {
        DWORD err = GetLastError();
        CloseHandle(dstH);
        return ::LayerMount::NtStatusFromWin32(err);
    }
    CloseHandle(dstH);
    return STATUS_SUCCESS;
}

NewUpperEntryKind NewUpperEntryKindOf(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? NewUpperEntryKind::Directory
                                                        : NewUpperEntryKind::File;
}

void RemoveNewUpperEntry(const std::wstring& upperPath, NewUpperEntryKind kind) {
    if (kind == NewUpperEntryKind::Directory) {
        ::RemoveDirectoryW(upperPath.c_str());
    } else {
        ::DeleteFileW(upperPath.c_str());
    }
}

}

LayerMountMetadata MakeCopyUpMetadata(const std::wstring& sourcePath) {
    LayerMountMetadata metadata = {};
    ::GetSystemTimeAsFileTime(&metadata.copyUpTimestamp);
    metadata.originLayer = sourcePath;
    AddSourceFileId(metadata, sourcePath);
    return metadata;
}

LayerMountMetadata CopiedEntryMetadata(const std::wstring& sourcePath,
                                       CopiedEntryRecord record,
                                       const LayerConfig& config) {
    if (record == CopiedEntryRecord::CarriedFromSource) {
        return CarriedCopyUpMetadata(sourcePath, config);
    }
    return MakeCopyUpMetadata(sourcePath);
}

// Uses the handle-based GetFileInformationByHandleEx with FileStreamInfo
// rather than the path-based FindFirstStreamW. Handle-based
// enumeration honors FILE_FLAG_BACKUP_SEMANTICS on the source open, which
// (with SE_BACKUP_NAME enabled in EnableFileSystemPrivileges) bypasses DACL
// checks so a lower file inside a directory whose ACL denies our process
// can still have its ADS enumerated. The path-based FindFirstStreamW does
// not carry backup semantics and fails with ERROR_ACCESS_DENIED on such
// files, so the copy-up would lose all of their ADS.
bool CopyUserAlternateDataStreams(const std::wstring& srcPath,
                                  const std::wstring& dstPath) {
    HANDLE srcH = ::CreateFileW(srcPath.c_str(),
                                  FILE_GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (srcH == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        return err == ERROR_FILE_NOT_FOUND;
    }

    std::vector<BYTE> buf(64 * 1024);
    if (!::GetFileInformationByHandleEx(srcH, FileStreamInfo, buf.data(),
                                          static_cast<DWORD>(buf.size()))) {
        DWORD err = ::GetLastError();
        ::CloseHandle(srcH);
        // ERROR_HANDLE_EOF means the file has no streams.
        return err == ERROR_HANDLE_EOF;
    }
    ::CloseHandle(srcH);

    bool ok = true;
    auto* p = reinterpret_cast<FILE_STREAM_INFO*>(buf.data());
    while (true) {
        // StreamName is NOT null-terminated; length is in bytes.
        const std::wstring name(p->StreamName,
                                p->StreamNameLength / sizeof(wchar_t));
        if (name != L"::$DATA" && !IsReservedLayerMountStream(name)) {
            if (!CopyAlternateStream(srcPath, dstPath, name)) {
                ok = false;
            }
        }
        if (p->NextEntryOffset == 0) break;
        p = reinterpret_cast<FILE_STREAM_INFO*>(
            reinterpret_cast<BYTE*>(p) + p->NextEntryOffset);
    }
    return ok;
}

bool ApplyEncryptedStateIfNeeded(const std::wstring& path, DWORD attrs) {
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_ENCRYPTED) == 0) {
        return true;
    }
    if (::EncryptFileW(path.c_str())) {
        return true;
    }
    const DWORD encryptErr = ::GetLastError();
    const DWORD encryptedAttrs = ::GetFileAttributesW(path.c_str());
    if (encryptedAttrs != INVALID_FILE_ATTRIBUTES &&
        (encryptedAttrs & FILE_ATTRIBUTE_ENCRYPTED) != 0) {
        return true;
    }
    ::SetLastError(encryptErr ? encryptErr : ERROR_ACCESS_DENIED);
    return false;
}

bool HasFileAttribute(DWORD attrs, DWORD flag) {
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & flag) != 0;
}

bool SetSparse(HANDLE handle) {
    FILE_SET_SPARSE_BUFFER sparseBuf{TRUE};
    DWORD bytesReturned = 0;
    return DeviceIoControl(handle, FSCTL_SET_SPARSE, &sparseBuf, sizeof(sparseBuf),
                           nullptr, 0, &bytesReturned, nullptr) != FALSE;
}

NTSTATUS SparseRefusalStatus() {
    const DWORD err = ::GetLastError();
    return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
}

void SetCompressedIfSource(HANDLE handle, DWORD srcAttrs) {
    if (HasFileAttribute(srcAttrs, FILE_ATTRIBUTE_COMPRESSED)) {
        SetCompressed(handle);
    }
}

NTSTATUS ApplyDirectoryLayout(const std::wstring& upperPath, DWORD srcAttrs) {
    if (HasFileAttribute(srcAttrs, FILE_ATTRIBUTE_COMPRESSED)) {
        SetCompressedDirectory(upperPath);
    }
    if (!ApplyEncryptedStateIfNeeded(upperPath, srcAttrs)) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }
    return STATUS_SUCCESS;
}

NTSTATUS CopyFileDataKeepingHoles(HANDLE srcHandle, HANDLE dstHandle) {
    if (!IsSparseHandle(srcHandle) || !IsSparseHandle(dstHandle)) {
        return CopyBytesToEnd(srcHandle, dstHandle);
    }

    LARGE_INTEGER srcSize{};
    if (!GetFileSizeEx(srcHandle, &srcSize)) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    bool rangesRefused = false;
    const NTSTATUS status =
        CopyAllocatedRanges(srcHandle, dstHandle, srcSize.QuadPart, rangesRefused);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (rangesRefused) {
        return CopyBytesFromStart(srcHandle, dstHandle);
    }
    return ExtendToSize(dstHandle, srcSize);
}

NTSTATUS WriteCopyUpRecordOrRemoveEntry(const std::wstring& upperPath,
                                        const LayerMountMetadata& metadata,
                                        NewUpperEntryKind kind,
                                        const LayerConfig& config) {
    if (MetadataStore::WriteLayerMountMetadata(upperPath, metadata, &config)) {
        return STATUS_SUCCESS;
    }
    const DWORD err = ::GetLastError();
    RemoveNewUpperEntry(upperPath, kind);
    return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
}

NTSTATUS CopyLinkWithCopyUpRecord(const std::wstring& srcAbsolute,
                                  DWORD srcAttrs,
                                  const std::wstring& dstAbsolute,
                                  const EntryCopyPolicy& policy) {
    const NewUpperEntryKind kind = NewUpperEntryKindOf(srcAttrs);
    const NTSTATUS status = CopyUpReparsePointEntry(srcAbsolute, dstAbsolute, srcAttrs);
    if (!NT_SUCCESS(status)) {
        RemoveNewUpperEntry(dstAbsolute, kind);
        return status;
    }
    return WriteCopyUpRecordOrRemoveEntry(
        dstAbsolute, CopiedEntryMetadata(srcAbsolute, policy.record, policy.config), kind,
        policy.config);
}

}
