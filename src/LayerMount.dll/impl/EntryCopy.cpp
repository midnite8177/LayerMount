#include "EntryCopy.h"
#include "LayerPath.h"
#include "MetadataStore.h"
#include "NtStatusUtil.h"
#include "NtdllExport.h"
#include "ScopedHandle.h"
#include "ElevationUtil.h"

#include <aclapi.h>
#include <winioctl.h>
#include <climits>
#include <cstring>
#include <iterator>
#include <memory>
#include <string_view>
#include <vector>

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
    const std::wstring extendedPath = WithExtendedPrefix(path);
    const DWORD attrs = ::GetFileAttributesW(extendedPath.c_str());
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

    HANDLE h = ::CreateFileW(extendedPath.c_str(),
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
    HANDLE handle = CreateFileW(WithExtendedPrefix(path).c_str(),
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

// WriteFile returns success with a count below length on a full quota, on a
// network volume and on some raw devices; that write fails with
// ERROR_WRITE_FAULT.
NTSTATUS WriteChunk(HANDLE dstHandle, const BYTE* buffer, DWORD length) {
    DWORD bytesWritten = 0;
    if (!WriteFile(dstHandle, buffer, length, &bytesWritten, nullptr)) {
        return StatusOfFailedCall(ERROR_WRITE_FAULT);
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
            return StatusOfFailedCall(ERROR_READ_FAULT);
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

// streamName is the `:name:$TYPE` form of FILE_STREAM_INFO.StreamName. A
// failed copy removes the destination stream and returns the status of the
// call that failed.
//
// Both opens use FILE_FLAG_BACKUP_SEMANTICS. With SE_BACKUP_NAME and
// SE_RESTORE_NAME, which EnableFileSystemPrivileges enables, that bypasses
// DACL checks. A lower file inside a directory whose DACL denies this
// process, for example with an inherited DENY-WRITE for Everyone, still has
// its streams copied up. Without backup semantics the destination open fails,
// because the destination inherits the same DACL from its parent.
NTSTATUS CopyAlternateStream(const HostPath& srcPath,
                             const HostPath& dstPath,
                             std::wstring_view streamName) {
    const std::wstring srcFull = srcPath.ForWin32() + std::wstring(streamName);
    const std::wstring dstFull = dstPath.ForWin32() + std::wstring(streamName);

    ScopedHandle srcHandle(::CreateFileW(
        srcFull.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!srcHandle.IsValid()) {
        return StatusOfFailedCall(ERROR_READ_FAULT);
    }

    ScopedHandle dstHandle(::CreateFileW(
        dstFull.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!dstHandle.IsValid()) {
        return StatusOfFailedCall(ERROR_WRITE_FAULT);
    }

    const NTSTATUS status = CopyBytesToEnd(srcHandle.Get(), dstHandle.Get());
    if (!NT_SUCCESS(status)) {
        dstHandle.Reset();
        ::DeleteFileW(dstFull.c_str());
    }
    return status;
}

// Reads the FILE_STREAM_INFO list of handle into list. FileStreamInfo gives
// no size hint, so the buffer doubles on each ERROR_MORE_DATA until the list
// fits or the size passes what a DWORD length can hold. ERROR_HANDLE_EOF
// means the file has no streams and leaves list empty.
//
// The list comes from GetFileInformationByHandleEx and not from the
// path-based FindFirstStreamW. A handle opened with
// FILE_FLAG_BACKUP_SEMANTICS lists the streams of a file whose DACL denies
// this process; FindFirstStreamW fails there with ERROR_ACCESS_DENIED.
NTSTATUS ReadStreamList(HANDLE handle, std::vector<BYTE>& list) {
    list.resize(kInitialStreamListSize);
    for (;;) {
        if (::GetFileInformationByHandleEx(handle, FileStreamInfo, list.data(),
                                           static_cast<DWORD>(list.size()))) {
            return STATUS_SUCCESS;
        }
        const DWORD err = ::GetLastError();
        if (err == ERROR_HANDLE_EOF) {
            list.clear();
            return STATUS_SUCCESS;
        }
        if (err != ERROR_MORE_DATA) {
            return StatusFromWin32Error(err, ERROR_READ_FAULT);
        }
        if (list.size() > MAXDWORD / 2) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        list.resize(list.size() * 2);
    }
}

// Reads the FILE_STREAM_INFO list of the file or directory at path. The
// open uses FILE_FLAG_BACKUP_SEMANTICS, so it reads the stream list of a
// file whose DACL denies this process. The open asks for
// FILE_READ_ATTRIBUTES only, because an open with data access of a cloud
// placeholder file with a dehydrated range fails when no sync provider is
// connected: with STATUS_CLOUD_FILE_ACCESS_DENIED on Windows 11 and with
// STATUS_CLOUD_FILE_PROVIDER_NOT_RUNNING on Windows Server 2022.
NTSTATUS ReadStreamListOfPath(const std::wstring& path, std::vector<BYTE>& list) {
    ScopedHandle handle(::CreateFileW(
        WithExtendedPrefix(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle.IsValid()) {
        return StatusOfFailedCall(ERROR_READ_FAULT);
    }
    return ReadStreamList(handle.Get(), list);
}

// Calls visit with the name of each stream in list that
// IsUserAlternateStream accepts. Stops at the first visit that fails and
// returns its status.
template <typename Visit>
NTSTATUS ForEachUserStream(const std::vector<BYTE>& list, Visit visit) {
    if (list.empty()) {
        return STATUS_SUCCESS;
    }
    const BYTE* entry = list.data();
    for (;;) {
        const auto* info = reinterpret_cast<const FILE_STREAM_INFO*>(entry);
        // StreamName is not null-terminated, and its length is in bytes.
        const std::wstring_view name(info->StreamName,
                                     info->StreamNameLength / sizeof(wchar_t));
        if (IsUserAlternateStream(name)) {
            const NTSTATUS status = visit(name);
            if (!NT_SUCCESS(status)) {
                return status;
            }
        }
        if (info->NextEntryOffset == 0) {
            return STATUS_SUCCESS;
        }
        entry += info->NextEntryOffset;
    }
}

// A range copied before the call is written again at the same offset.
NTSTATUS CopyBytesFromStart(HANDLE srcHandle, HANDLE dstHandle) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(srcHandle, zero, nullptr, FILE_BEGIN) ||
        !SetFilePointerEx(dstHandle, zero, nullptr, FILE_BEGIN)) {
        return StatusOfFailedCall(ERROR_SEEK);
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
        return StatusOfFailedCall(ERROR_SEEK);
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

// A metacopy shell already has the source size and can be open elsewhere for
// write, so an equal size stays as it is.
NTSTATUS SetEndOfFileIfSizeDiffers(HANDLE dstHandle, LARGE_INTEGER size) {
    LARGE_INTEGER dstSize{};
    if (!GetFileSizeEx(dstHandle, &dstSize)) {
        return StatusOfFailedCall(ERROR_READ_FAULT);
    }
    if (dstSize.QuadPart == size.QuadPart) {
        return STATUS_SUCCESS;
    }
    if (!SetFilePointerEx(dstHandle, size, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(dstHandle)) {
        return StatusOfFailedCall(ERROR_WRITE_FAULT);
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

// A reparse point such as a cloud placeholder, or a file a storage manager
// moved offline, can list only its local data as allocated, so its ranges
// show a hole where the provider keeps data. A read fetches that data or
// fails, so the copy of such a file reads every byte.
bool MayHoldRemoteData(DWORD attributes) {
    constexpr DWORD remote = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
                             FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                             FILE_ATTRIBUTE_RECALL_ON_OPEN;
    return (attributes & remote) != 0;
}

// The SMB client fails a query for extended attributes with
// STATUS_INVALID_PARAMETER when its buffer is larger than 64 KB. A list
// longer than the buffer comes back over several queries.
constexpr size_t kExtendedAttributeBufferSize = 64 * 1024;

// A volume without extended attribute support answers with one of these,
// as a Linux file system without xattrs answers EOPNOTSUPP.
bool RefusesExtendedAttributes(NTSTATUS status) {
    return status == STATUS_EAS_NOT_SUPPORTED || status == STATUS_NOT_SUPPORTED ||
           status == STATUS_INVALID_DEVICE_REQUEST;
}

bool IsKernelExtendedAttributeName(std::string_view name) {
    constexpr std::string_view prefix = "$KERNEL.";
    return name.size() >= prefix.size() &&
           ::_strnicmp(name.data(), prefix.data(), prefix.size()) == 0;
}

size_t AlignedToUlong(size_t offset) {
    return (offset + sizeof(ULONG) - 1) & ~(sizeof(ULONG) - 1);
}

// The number of entries in list, a FILE_FULL_EA_INFORMATION list of length
// bytes.
ULONG FullEaEntryCount(const BYTE* list, ULONG length) {
    ULONG count = 0;
    size_t offset = 0;
    while (offset + sizeof(FullEaHeader) <= length) {
        ++count;
        const auto* header = reinterpret_cast<const FullEaHeader*>(list + offset);
        if (header->nextEntryOffset == 0) {
            break;
        }
        offset += header->nextEntryOffset;
    }
    return count;
}

NTSTATUS CopyUpReparsePointEntry(const std::wstring& srcAbsolute,
                                 const std::wstring& dstAbsolute,
                                 DWORD srcAttrs) {
    HANDLE srcH = CreateFileW(
        WithExtendedPrefix(srcAbsolute).c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (srcH == INVALID_HANDLE_VALUE) {
        return StatusOfFailedCall(ERROR_READ_FAULT);
    }

    BYTE reparseBuf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE]{};
    DWORD returned = 0;
    if (!DeviceIoControl(srcH, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                          reparseBuf, sizeof(reparseBuf), &returned, nullptr)) {
        DWORD err = GetLastError();
        CloseHandle(srcH);
        return StatusFromWin32Error(err, ERROR_READ_FAULT);
    }
    CloseHandle(srcH);

    const std::wstring extendedDst = WithExtendedPrefix(dstAbsolute);
    const bool isDir = (srcAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (isDir) {
        const NTSTATUS dirStatus = CreateDirectoryOrUseExisting(dstAbsolute);
        if (!NT_SUCCESS(dirStatus)) {
            return dirStatus;
        }
    } else {
        HANDLE dstCreate = CreateFileW(extendedDst.c_str(), GENERIC_WRITE, 0,
                                         nullptr, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dstCreate == INVALID_HANDLE_VALUE) {
            return StatusOfFailedCall(ERROR_WRITE_FAULT);
        }
        CloseHandle(dstCreate);
    }

    HANDLE dstH = CreateFileW(
        extendedDst.c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (dstH == INVALID_HANDLE_VALUE) {
        return StatusOfFailedCall(ERROR_WRITE_FAULT);
    }

    DWORD setReturned = 0;
    if (!DeviceIoControl(dstH, FSCTL_SET_REPARSE_POINT, reparseBuf, returned,
                          nullptr, 0, &setReturned, nullptr)) {
        DWORD err = GetLastError();
        CloseHandle(dstH);
        return StatusFromWin32Error(err, ERROR_WRITE_FAULT);
    }
    CloseHandle(dstH);
    return STATUS_SUCCESS;
}

NewUpperEntryKind NewUpperEntryKindOf(DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? NewUpperEntryKind::Directory
                                                        : NewUpperEntryKind::File;
}

void RemoveNewUpperEntry(const std::wstring& upperPath, NewUpperEntryKind kind) {
    const std::wstring extendedPath = WithExtendedPrefix(upperPath);
    if (kind == NewUpperEntryKind::Directory) {
        ::RemoveDirectoryW(extendedPath.c_str());
    } else {
        ::DeleteFileW(extendedPath.c_str());
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

NTSTATUS CopyUserAlternateDataStreams(const std::wstring& srcPath,
                                      const std::wstring& dstPath) {
    const HostPath src(srcPath);
    const HostPath dst(dstPath);
    std::vector<BYTE> list;
    const NTSTATUS listStatus = ReadStreamListOfPath(srcPath, list);
    if (!NT_SUCCESS(listStatus)) {
        return listStatus;
    }
    return ForEachUserStream(list, [&](std::wstring_view name) {
        return CopyAlternateStream(src, dst, name);
    });
}

NTSTATUS HasUserAlternateDataStream(const std::wstring& path, bool* has) {
    std::vector<BYTE> list;
    const NTSTATUS listStatus = ReadStreamListOfPath(path, list);
    if (!NT_SUCCESS(listStatus)) {
        return listStatus;
    }
    bool found = false;
    const NTSTATUS status = ForEachUserStream(list, [&](std::wstring_view) {
        found = true;
        return STATUS_SUCCESS;
    });
    if (NT_SUCCESS(status)) {
        *has = found;
    }
    return status;
}

bool ApplyEncryptedStateIfNeeded(const HostPath& path, DWORD attrs) {
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_ENCRYPTED) == 0) {
        return true;
    }
    const std::wstring extendedPath = path.ForWin32();
    if (::EncryptFileW(extendedPath.c_str())) {
        return true;
    }
    const DWORD encryptErr = ::GetLastError();
    const DWORD encryptedAttrs = ::GetFileAttributesW(extendedPath.c_str());
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

std::optional<DWORD> AttributesOrNone(DWORD attributes) {
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return std::nullopt;
    }
    return attributes;
}

bool SetSparse(HANDLE handle) {
    FILE_SET_SPARSE_BUFFER sparseBuf{TRUE};
    DWORD bytesReturned = 0;
    return DeviceIoControl(handle, FSCTL_SET_SPARSE, &sparseBuf, sizeof(sparseBuf),
                           nullptr, 0, &bytesReturned, nullptr) != FALSE;
}

NTSTATUS SparseRefusalStatus() {
    return StatusOfFailedCall(ERROR_ACCESS_DENIED);
}

void SetCompressedIfSource(HANDLE handle, DWORD srcAttrs) {
    if (HasFileAttribute(srcAttrs, FILE_ATTRIBUTE_COMPRESSED)) {
        SetCompressed(handle);
    }
}

NTSTATUS CopyDirectoryOwnMetadata(const std::wstring& srcAbs, const std::wstring& dstAbs) {
    const DWORD srcAttrs = ::GetFileAttributesW(WithExtendedPrefix(srcAbs).c_str());
    if (srcAttrs != INVALID_FILE_ATTRIBUTES) {
        if (HasFileAttribute(srcAttrs, FILE_ATTRIBUTE_COMPRESSED)) {
            SetCompressedDirectory(dstAbs);
        }
        if (!ApplyEncryptedStateIfNeeded(HostPath(dstAbs), srcAttrs)) {
            return StatusOfFailedCall(ERROR_ACCESS_DENIED);
        }
    }
    const NTSTATUS streamStatus = CopyUserAlternateDataStreams(srcAbs, dstAbs);
    if (!NT_SUCCESS(streamStatus)) {
        return streamStatus;
    }
    return CopyExtendedAttributes(srcAbs, dstAbs);
}

HANDLE OpenSourceFileForCopy(const std::wstring& path) {
    return ::CreateFileW(WithExtendedPrefix(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_BACKUP_SEMANTICS,
                         nullptr);
}

NTSTATUS CopyFileDataKeepingHoles(HANDLE srcHandle, HANDLE dstHandle) {
    BY_HANDLE_FILE_INFORMATION srcInfo{};
    if (!GetFileInformationByHandle(srcHandle, &srcInfo) ||
        !HasFileAttribute(srcInfo.dwFileAttributes, FILE_ATTRIBUTE_SPARSE_FILE) ||
        MayHoldRemoteData(srcInfo.dwFileAttributes) || !IsSparseHandle(dstHandle)) {
        return CopyBytesToEnd(srcHandle, dstHandle);
    }

    LARGE_INTEGER srcSize{};
    if (!GetFileSizeEx(srcHandle, &srcSize)) {
        return StatusOfFailedCall(ERROR_READ_FAULT);
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
    return SetEndOfFileIfSizeDiffers(dstHandle, srcSize);
}

namespace {

// The pin and offline bits tell how a sync provider or a storage manager
// keeps the source. The copy is a local entry outside that provider, so it
// does not take them.
DWORD AttributesFileBasicInfoCanSet(DWORD attributes) {
    constexpr DWORD settable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                               FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
                               FILE_ATTRIBUTE_TEMPORARY |
                               FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                               FILE_ATTRIBUTE_NO_SCRUB_DATA;
    const DWORD kept = attributes & settable;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return kept | FILE_ATTRIBUTE_DIRECTORY;
    }
    // A FileAttributes of 0 keeps the stored bits, and NORMAL clears them.
    return kept != 0 ? kept : FILE_ATTRIBUTE_NORMAL;
}

}

bool ClearReadOnly(HANDLE entry, DWORD attributes) {
    if ((attributes & FILE_ATTRIBUTE_READONLY) == 0) {
        return true;
    }
    // Zero times keep the stored times, and a ChangeTime of -1 keeps the
    // stored value. A FileAttributes of zero would keep the stored
    // attributes, the read-only bit with them, so a set left empty goes
    // out as FILE_ATTRIBUTE_NORMAL.
    FILE_BASIC_INFO basic{};
    basic.ChangeTime.QuadPart = -1;
    basic.FileAttributes = AttributesFileBasicInfoCanSet(attributes & ~FILE_ATTRIBUTE_READONLY);
    return ::SetFileInformationByHandle(entry, FileBasicInfo, &basic, sizeof(basic)) != FALSE;
}

EntryTimes EntryTimesOf(const WIN32_FILE_ATTRIBUTE_DATA& data) {
    return {data.ftCreationTime, data.ftLastAccessTime, data.ftLastWriteTime};
}

std::optional<EntryTimes> ReadEntryTimes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(WithExtendedPrefix(path).c_str(), GetFileExInfoStandard, &data)) {
        return std::nullopt;
    }
    return EntryTimesOf(data);
}

bool WriteEntryTimes(const std::wstring& path,
                     const EntryTimes& times,
                     std::optional<DWORD> attributes) {
    ScopedHandle handle(::CreateFileW(
        WithExtendedPrefix(path).c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle.IsValid()) {
        return false;
    }
    BOOL ok = FALSE;
    if (attributes.has_value()) {
        FILE_BASIC_INFO basic{};
        basic.CreationTime.LowPart    = times.creation.dwLowDateTime;
        basic.CreationTime.HighPart   = static_cast<LONG>(times.creation.dwHighDateTime);
        basic.LastAccessTime.LowPart  = times.access.dwLowDateTime;
        basic.LastAccessTime.HighPart = static_cast<LONG>(times.access.dwHighDateTime);
        basic.LastWriteTime.LowPart   = times.write.dwLowDateTime;
        basic.LastWriteTime.HighPart  = static_cast<LONG>(times.write.dwHighDateTime);
        // A ChangeTime of -1 keeps the stored value.
        basic.ChangeTime.QuadPart     = -1;
        basic.FileAttributes          = AttributesFileBasicInfoCanSet(*attributes);
        ok = ::SetFileInformationByHandle(handle.Get(), FileBasicInfo, &basic, sizeof(basic));
    } else {
        ok = ::SetFileTime(handle.Get(), &times.creation, &times.access, &times.write);
    }
    if (!ok) {
        const DWORD err = ::GetLastError();
        handle.Reset();
        ::SetLastError(err);
        return false;
    }
    return true;
}

bool WriteOwnAttributes(const std::wstring& path, DWORD attributes) {
    ScopedHandle handle(::CreateFileW(
        WithExtendedPrefix(path).c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.IsValid()) {
        return false;
    }
    constexpr DWORD settable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                               FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
                               FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE |
                               FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                               FILE_ATTRIBUTE_NO_SCRUB_DATA | FILE_ATTRIBUTE_PINNED |
                               FILE_ATTRIBUTE_UNPINNED | FILE_ATTRIBUTE_DIRECTORY;
    const DWORD kept = attributes & settable;
    FILE_BASIC_INFO basic{};
    // Zero times keep the stored values, and NORMAL clears the bits.
    basic.FileAttributes = kept != 0 ? kept : FILE_ATTRIBUTE_NORMAL;
    if (!::SetFileInformationByHandle(handle.Get(), FileBasicInfo, &basic, sizeof(basic))) {
        const DWORD err = ::GetLastError();
        handle.Reset();
        ::SetLastError(err);
        return false;
    }
    return true;
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
    return StatusFromWin32Error(err, ERROR_WRITE_FAULT);
}

std::vector<BYTE> ExtendedAttributesUserModeCanSet(const BYTE* list, ULONG length) {
    std::vector<BYTE> settable;
    size_t lastKept = 0;
    size_t offset = 0;
    while (offset + sizeof(FullEaHeader) <= length) {
        const auto* header = reinterpret_cast<const FullEaHeader*>(list + offset);
        const size_t entrySize =
            sizeof(FullEaHeader) + header->nameLength + 1 + header->valueLength;
        if (offset + entrySize > length) {
            break;
        }
        const std::string_view name(reinterpret_cast<const char*>(header + 1),
                                    header->nameLength);
        if (!IsKernelExtendedAttributeName(name)) {
            if (!settable.empty()) {
                settable.resize(AlignedToUlong(settable.size()));
                reinterpret_cast<FullEaHeader*>(settable.data() + lastKept)->nextEntryOffset =
                    static_cast<ULONG>(settable.size() - lastKept);
            }
            lastKept = settable.size();
            settable.insert(settable.end(), list + offset, list + offset + entrySize);
            reinterpret_cast<FullEaHeader*>(settable.data() + lastKept)->nextEntryOffset = 0;
        }
        if (header->nextEntryOffset == 0) {
            break;
        }
        offset += header->nextEntryOffset;
    }
    return settable;
}

NTSTATUS CopyExtendedAttributes(const std::wstring& srcPath, const std::wstring& dstPath) {
    const auto queryEa = LoadNtdllExport<NtQueryEaFileFn>("NtQueryEaFile");
    const auto setEa = LoadNtdllExport<NtSetEaFileFn>("NtSetEaFile");
    if (queryEa == nullptr || setEa == nullptr) {
        return STATUS_PROCEDURE_NOT_FOUND;
    }
    ScopedHandle srcHandle(::CreateFileW(
        WithExtendedPrefix(srcPath).c_str(), FILE_READ_EA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    if (!srcHandle.IsValid()) {
        return StatusOfFailedCall(ERROR_READ_FAULT);
    }
    std::vector<BYTE> buffer(kExtendedAttributeBufferSize);
    ScopedHandle dstHandle;
    // The first query restarts the scan. Each later query names the 1-based
    // index of the entry it starts at, because the SMB client starts a query
    // that names no index at the first entry again.
    ULONG nextIndex = 1;
    for (;;) {
        IO_STATUS_BLOCK readIo{};
        ULONG index = nextIndex;
        const bool firstQuery = nextIndex == 1;
        const NTSTATUS readStatus =
            queryEa(srcHandle.Get(), &readIo, buffer.data(), static_cast<ULONG>(buffer.size()),
                    FALSE, nullptr, 0, firstQuery ? nullptr : &index, firstQuery);
        if (readStatus == STATUS_NO_EAS_ON_FILE || readStatus == STATUS_NO_MORE_EAS ||
            RefusesExtendedAttributes(readStatus)) {
            return STATUS_SUCCESS;
        }
        // STATUS_BUFFER_OVERFLOW returns the entries that fit.
        if (!NT_SUCCESS(readStatus) && readStatus != STATUS_BUFFER_OVERFLOW) {
            return readStatus;
        }
        if (readIo.Information == 0) {
            return STATUS_SUCCESS;
        }
        nextIndex += FullEaEntryCount(buffer.data(), static_cast<ULONG>(readIo.Information));
        std::vector<BYTE> settable = ExtendedAttributesUserModeCanSet(
            buffer.data(), static_cast<ULONG>(readIo.Information));
        if (settable.empty()) {
            continue;
        }
        if (!dstHandle.IsValid()) {
            dstHandle.Reset(::CreateFileW(
                WithExtendedPrefix(dstPath).c_str(), FILE_WRITE_EA,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
            if (!dstHandle.IsValid()) {
                return StatusOfFailedCall(ERROR_WRITE_FAULT);
            }
        }
        IO_STATUS_BLOCK writeIo{};
        const NTSTATUS writeStatus = setEa(dstHandle.Get(), &writeIo, settable.data(),
                                           static_cast<ULONG>(settable.size()));
        // Overlayfs skips the xattr copy when the upper has no xattrs.
        if (RefusesExtendedAttributes(writeStatus)) {
            return STATUS_SUCCESS;
        }
        if (!NT_SUCCESS(writeStatus)) {
            return writeStatus;
        }
    }
}

NTSTATUS CloneReparsePointWithCopyUpRecord(const SourceEntry& source,
                                           const std::wstring& dstAbsolute,
                                           const EntryCopyPolicy& policy) {
    const NewUpperEntryKind kind = NewUpperEntryKindOf(source.attributes);
    NTSTATUS status = CopyUpReparsePointEntry(source.path, dstAbsolute, source.attributes);
    if (NT_SUCCESS(status)) {
        status = CopyExtendedAttributes(source.path, dstAbsolute);
    }
    if (!NT_SUCCESS(status)) {
        RemoveNewUpperEntry(dstAbsolute, kind);
        return status;
    }
    return WriteCopyUpRecordOrRemoveEntry(
        dstAbsolute, CopiedEntryMetadata(source.path, policy.record, policy.config), kind,
        policy.config);
}

NTSTATUS WriteSecurityToInheritAs(const std::wstring& dirAbs, const std::wstring& parentAbs) {
    const SECURITY_INFORMATION lists =
        DropSaclWithoutPrivilege(DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION);
    PACL parentDacl = nullptr;
    PACL parentSacl = nullptr;
    PSECURITY_DESCRIPTOR rawParentSd = nullptr;
    const DWORD readError = ::GetNamedSecurityInfoW(WithExtendedPrefix(parentAbs).c_str(),
                                                    SE_FILE_OBJECT, lists, nullptr, nullptr,
                                                    &parentDacl, &parentSacl, &rawParentSd);
    if (readError != ERROR_SUCCESS) {
        return NtStatusFromWin32(readError);
    }
    const std::unique_ptr<void, decltype(&::LocalFree)> parentSd(rawParentSd, &::LocalFree);

    // A protected write keeps the ACEs that parentAbs inherits in the list.
    // So a child of dirAbs also inherits the inheritable ACEs among them.
    const SECURITY_INFORMATION protection = (lists & SACL_SECURITY_INFORMATION) != 0
        ? PROTECTED_DACL_SECURITY_INFORMATION | PROTECTED_SACL_SECURITY_INFORMATION
        : PROTECTED_DACL_SECURITY_INFORMATION;
    std::wstring extendedDir = WithExtendedPrefix(dirAbs);
    const DWORD writeError = ::SetNamedSecurityInfoW(
        extendedDir.data(), SE_FILE_OBJECT, lists | protection,
        nullptr, nullptr, parentDacl, parentSacl);
    if (writeError != ERROR_SUCCESS) {
        return NtStatusFromWin32(writeError);
    }
    return STATUS_SUCCESS;
}

NTSTATUS CloneReparsePointThroughWorkDir(const SourceEntry& source,
                                         const HostPath& containerPath,
                                         const std::wstring& upperPath,
                                         const EntryCopyPolicy& policy) {
    return BuildInContainerAndMove(
        containerPath, upperPath, policy.config, [&](const HostPath& stagedPath) {
            return CloneReparsePointWithCopyUpRecord(source, stagedPath.Text(), policy);
        });
}

}
