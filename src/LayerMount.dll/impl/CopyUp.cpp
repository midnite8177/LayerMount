#include "CopyUp.h"
#include "LayerPath.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "MetadataStore.h"
#include "Cache.h"
#include "NtStatusUtil.h"
#include "ElevationUtil.h"
#include "EntryCopy.h"
#include "../abi/ErrorTls.h"

#include <winioctl.h>
#include <optional>

#pragma comment(lib, "advapi32.lib")

namespace {

NTSTATUS RecordFillFailure(const std::wstring& relativePath, const wchar_t* stage,
                           NTSTATUS status) {
    wchar_t statusText[16] = {};
    swprintf_s(statusText, L"0x%08lX", static_cast<unsigned long>(status));
    const std::wstring message = L"The metacopy fill of '" + relativePath +
                                 L"' failed to " + stage + L" (NTSTATUS " +
                                 statusText + L").";
    ::LayerMount::abi::ErrorTls::SetFillFailure(::LayerMount::HresultFromNtStatus(status), message.c_str());
    return status;
}

bool ClearSparseAndTrimAllocation(HANDLE handle) {
    FILE_SET_SPARSE_BUFFER sparseBuf{FALSE};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, &sparseBuf, sizeof(sparseBuf),
                         nullptr, 0, &bytesReturned, nullptr)) {
        return false;
    }
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(handle, &fileSize)) {
        return false;
    }
    // The clear alone keeps the clusters the volume gave the sparse file in
    // 64 KiB units, so a handle still reports that allocation. The set-info
    // frees the clusters past the file size.
    FILE_ALLOCATION_INFO allocInfo{};
    allocInfo.AllocationSize = fileSize;
    return SetFileInformationByHandle(handle, FileAllocationInfo, &allocInfo,
                                      sizeof(allocInfo)) != FALSE;
}

}

namespace LayerMount {

namespace {

DWORD WithoutReadOnly(DWORD attributes) {
    return attributes & ~FILE_ATTRIBUTE_READONLY;
}

// The source attributes that CreateFileW gives the metacopy shell. In the
// flags of CreateFileW, a bit above 0xFFFF is a FILE_FLAG_* value, and a
// source attribute such as FILE_ATTRIBUTE_PINNED uses such a bit. Thus
// only attributes that CreateFileW sets go through. Read-only does not go
// through, because NTFS refuses the stream of the copy-up record on a
// read-only file. The guard in CopyUpMetadataOnly sets read-only after the
// record. Offline does not go through, because the shell is outside the
// sync root or the storage manager of the source. Last, the guard writes
// the source times and each source attribute that a FileBasicInfo write
// can set.
DWORD MetacopyShellCreateAttributes(DWORD sourceAttributes) {
    constexpr DWORD kCreatable = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                 FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY |
                                 FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                                 FILE_ATTRIBUTE_ENCRYPTED;
    return sourceAttributes & kCreatable;
}

// attributes must be the current attributes of the file at path. Writes
// them without FILE_ATTRIBUTE_READONLY and keeps the stored times.
bool ClearReadOnly(const std::wstring& path, DWORD attributes) {
    return (attributes & FILE_ATTRIBUTE_READONLY) == 0 ||
           WriteEntryTimes(path, EntryTimes{}, WithoutReadOnly(attributes));
}

// A delete refuses a read-only file, and a staged file is read-only once
// its attributes go on.
void RemoveStagedFile(const std::wstring& workPath, const LayerConfig& config) {
    const DWORD attributes = ::GetFileAttributesW(workPath.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        ClearReadOnly(workPath, attributes);
    }
    RemoveUpperEntry(workPath, config);
}

// A rename sets FILE_ATTRIBUTE_ARCHIVE on a file. The staged file gets the
// bit before the move, so the move changes no attribute at the upper path.
std::optional<DWORD> StagedFileAttributes(DWORD sourceAttributes) {
    std::optional<DWORD> attributes = AttributesOrNone(sourceAttributes);
    if (attributes.has_value()) {
        *attributes |= FILE_ATTRIBUTE_ARCHIVE;
    }
    return attributes;
}

}

// Serializes copy-ups of one relative path. The constructor blocks until no
// other thread holds `path`. The reservation is not reentrant: a thread that
// reserves a path it already holds deadlocks. A nested reservation goes from a
// child to its parent, never from a parent to a child.
class CopyUp::PathReservation {
public:
    PathReservation(CopyUp& owner, std::wstring path)
        : owner_(owner), path_(std::move(path)) {
        std::unique_lock<std::mutex> lock(owner_.copyUpMutex_);
        owner_.copyUpCV_.wait(lock, [&]() {
            return owner_.inFlightCopyUps_.find(path_) == owner_.inFlightCopyUps_.end();
        });
        owner_.inFlightCopyUps_.insert(path_);
    }
    ~PathReservation() {
        {
            std::lock_guard<std::mutex> lock(owner_.copyUpMutex_);
            owner_.inFlightCopyUps_.erase(path_);
        }
        owner_.copyUpCV_.notify_all();
    }
    PathReservation(const PathReservation&) = delete;
    PathReservation& operator=(const PathReservation&) = delete;

    bool ExistsInUpperWhileHeld() const {
        return owner_.pathResolver_.ExistsInUpper(path_);
    }
private:
    CopyUp& owner_;
    std::wstring path_;
};

// Captures the timestamps of a source, and attribute bits when the
// caller gives them. Restore() writes them to the copy-up target. A data
// write and a metadata stream write each change the target's
// LastWriteTime, so Restore() runs after the last write to the target.
// A zero time keeps the stored value, so a capture that failed and left
// a time at zero restores nothing for that field.
//
// Restore() returns false with the Win32 error in GetLastError.
class FileBasicInfoGuard {
public:
    FileBasicInfoGuard(HANDLE source, std::optional<DWORD> attributes,
                       std::wstring targetPath)
        : attributes_(attributes), targetPath_(std::move(targetPath)) {
        GetFileTime(source, &times_.creation, &times_.access, &times_.write);
    }
    FileBasicInfoGuard(const WIN32_FILE_ATTRIBUTE_DATA& source,
                       std::optional<DWORD> attributes, std::wstring targetPath)
        : times_(EntryTimesOf(source)),
          attributes_(attributes),
          targetPath_(std::move(targetPath)) {}
    // The result is discarded, because on a failure path the caller can
    // have deleted the target already.
    ~FileBasicInfoGuard() {
        if (!restored_) {
            Restore();
        }
    }
    FileBasicInfoGuard(const FileBasicInfoGuard&) = delete;
    FileBasicInfoGuard& operator=(const FileBasicInfoGuard&) = delete;

    bool Restore() {
        restored_ = true;
        return WriteEntryTimes(targetPath_, times_, attributes_);
    }

    // The destructor then writes nothing. A failed staged copy calls this
    // before its removal, so a copy that the removal leaves stays writable.
    void Cancel() { restored_ = true; }

private:
    EntryTimes times_{};
    std::optional<DWORD> attributes_;
    std::wstring targetPath_;
    bool restored_ = false;
};

CopyUp::CopyUp(ConfigRef config,
               PathResolver& pathResolver,
               WhiteoutManager& whiteoutMgr,
               Cache& cache,
               LayerMountStats& stats)
    : config_(config.Get())
    , pathResolver_(pathResolver)
    , whiteoutMgr_(whiteoutMgr)
    , cache_(cache)
    , stats_(stats) {
    EnableFileSystemPrivileges();
}

void CopyUp::RecordCopyUp(const std::wstring& relativePath) {
    stats_.copyUpCount.fetch_add(1, std::memory_order_relaxed);
    if (events_ != nullptr) {
        events_->Emit(LM_EVT_COPY_UP, S_OK, relativePath.c_str(), nullptr);
    }
}

std::wstring CopyUp::GenerateWorkPath() {
    uint64_t counter = workCounter_.fetch_add(1, std::memory_order_relaxed);
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t timestamp = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;

    // pid, tid and counter make the name unique. Two calls can get the
    // same timestamp.
    return JoinDirPath(config_.workDirPath,
                       L"#" + std::to_wstring(pid) + L"." + std::to_wstring(tid) + L"." +
                       std::to_wstring(counter) + L"." + std::to_wstring(timestamp) + L".tmp");
}

void CopyUp::CleanWorkDirectory() {
    std::wstring searchPath = JoinDirPath(config_.workDirPath, L"#*.tmp");
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        RemoveStagedFile(JoinDirPath(config_.workDirPath, findData.cFileName), config_);
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
}

NTSTATUS CopyUp::CommitFromWorkDir(const std::wstring& workPath,
                                    const std::wstring& finalUpperPath) {
    std::filesystem::path parentDir = std::filesystem::path(finalUpperPath).parent_path();
    if (!parentDir.empty()) {
        EnsureDirectoryExists(parentDir.wstring());
    }

    const NTSTATUS status =
        MoveUpperEntry(workPath, finalUpperPath, ReplaceExisting::No, config_);
    if (!NT_SUCCESS(status)) {
        RemoveStagedFile(workPath, config_);
    }
    return status;
}

NTSTATUS CopyUp::CopyUpReparseCloneAndCount(const std::wstring& normalized,
                                            const CopyUpTarget& target) {
    const NTSTATUS status = CloneReparsePointThroughWorkDir(
        {target.source.absolutePath, target.source.attributes}, GenerateWorkPath(),
        target.upperPath, {CopiedEntryRecord::NewFromSource, config_});
    if (!NT_SUCCESS(status)) {
        return status;
    }

    cache_.InvalidateWithAncestors(normalized);
    RecordCopyUp(normalized);

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::StageFileInWorkDir(const std::wstring& sourcePath,
                                    ScopedHandle& srcHandle,
                                    DWORD srcAttrs,
                                    const std::wstring& workPath) {
    ScopedHandle dstHandle(CreateFileW(
        workPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));

    if (!dstHandle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    // SetFileAttributes cannot set FILE_ATTRIBUTE_SPARSE_FILE. Only FSCTL_SET_SPARSE can.
    if (HasFileAttribute(srcAttrs, FILE_ATTRIBUTE_SPARSE_FILE) &&
        config_.Capabilities().HasSparseFiles() &&
        !SetSparse(dstHandle.Get())) {
        return SparseRefusalStatus();
    }

    SetCompressedIfSource(dstHandle.Get(), srcAttrs);

    const NTSTATUS status = CopyFileDataKeepingHoles(srcHandle.Get(), dstHandle.Get());
    if (!NT_SUCCESS(status)) {
        return status;
    }

    srcHandle.Reset();
    dstHandle.Reset();

    return CopyStagedFileMetadata(sourcePath, srcAttrs, workPath);
}

NTSTATUS CopyUp::CopyStagedFileMetadata(const std::wstring& sourcePath,
                                        DWORD srcAttrs,
                                        const std::wstring& workPath) {
    if (!ApplyEncryptedStateIfNeeded(workPath, srcAttrs)) {
        const DWORD err = ::GetLastError();
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    const NTSTATUS eaStatus = CopyExtendedAttributes(sourcePath, workPath);
    if (!NT_SUCCESS(eaStatus)) {
        return eaStatus;
    }

    // A failed security copy fails the copy-up, because a commit with an
    // inherited or default DACL can give more access than the source does.
    if (!CopySecurityDescriptor(sourcePath, workPath)) {
        const DWORD err = ::GetLastError();
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::FinishStagedFile(const std::wstring& sourcePath,
                                  const std::wstring& workPath,
                                  FileBasicInfoGuard& basicInfo) {
    const NTSTATUS streamStatus = CopyUserAlternateDataStreams(sourcePath, workPath);
    if (!NT_SUCCESS(streamStatus)) {
        return streamStatus;
    }
    return RecordStagedFile(workPath, MakeCopyUpMetadata(sourcePath), basicInfo);
}

NTSTATUS CopyUp::RecordStagedFile(const std::wstring& workPath,
                                  const LayerMountMetadata& metadata,
                                  FileBasicInfoGuard& basicInfo) {
    if (!MetadataStore::WriteLayerMountMetadata(workPath, metadata, &config_)) {
        return StatusOfFailedCall(ERROR_WRITE_FAULT);
    }

    // The attributes go on after the streams and the record, because NTFS
    // refuses a new stream on a read-only file, and the stream and extended
    // attribute writes move the times.
    if (!basicInfo.Restore()) {
        return StatusOfFailedCall(ERROR_ACCESS_DENIED);
    }

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::PrepareCopyUpTarget(const std::wstring& normalized, CopyUpTarget* target) {
    target->source = pathResolver_.ResolveLowerPath(normalized);
    if (!target->source.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    const NTSTATUS status = EnsureUpperParent(normalized);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    target->upperPath = pathResolver_.GetUpperPathForCopyUp(normalized, target->source);
    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::CopyUpFile(const std::wstring& relativePath) {
    std::wstring normalized = NormalizePath(relativePath);

    PathReservation reservation(*this, normalized);

    // A copy-up that waited on the reservation finds the copy of the one
    // before it here. Without this check, its move fails with
    // STATUS_OBJECT_NAME_COLLISION.
    if (pathResolver_.ExistsInUpper(normalized)) {
        return STATUS_SUCCESS;
    }

    CopyUpTarget target;
    NTSTATUS status = PrepareCopyUpTarget(normalized, &target);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    const ResolvedPath& source = target.source;
    const std::wstring& upperPath = target.upperPath;

    bool clonesReparsePoint = false;
    status = ClonesReparsePoint(source.absolutePath, source.attributes, &clonesReparsePoint);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (clonesReparsePoint) {
        return CopyUpReparseCloneAndCount(normalized, target);
    }

    ScopedHandle srcHandle(CreateFileW(
        source.absolutePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));

    if (!srcHandle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    DWORD srcAttrs = GetFileAttributesW(source.absolutePath.c_str());
    const std::wstring workPath = GenerateWorkPath();
    FileBasicInfoGuard basicInfo(srcHandle.Get(), StagedFileAttributes(srcAttrs), workPath);

    status = StageFileInWorkDir(source.absolutePath, srcHandle, srcAttrs, workPath);
    if (NT_SUCCESS(status)) {
        status = FinishStagedFile(source.absolutePath, workPath, basicInfo);
    }
    if (!NT_SUCCESS(status)) {
        basicInfo.Cancel();
        RemoveStagedFile(workPath, config_);
        return status;
    }

    return CommitStagedFile(normalized, workPath, upperPath);
}

NTSTATUS CopyUp::CommitStagedFile(const std::wstring& normalized,
                                  const std::wstring& workPath,
                                  const std::wstring& upperPath) {
    const NTSTATUS status = CommitFromWorkDir(workPath, upperPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    cache_.InvalidateWithAncestors(normalized);
    RecordCopyUp(normalized);

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::MarkPlaceholderSparseOrAbort(ScopedHandle& dstHandle,
                                              const std::wstring& workPath) {
    if (!SetSparse(dstHandle.Get())) {
        const NTSTATUS status = SparseRefusalStatus();
        dstHandle.Reset();
        ::DeleteFileW(workPath.c_str());
        return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::StageMetacopyShellInWorkDir(const std::wstring& sourcePath,
                                             const WIN32_FILE_ATTRIBUTE_DATA& srcAttrs,
                                             const std::wstring& workPath) {
    ScopedHandle dstHandle(CreateFileW(
        workPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        MetacopyShellCreateAttributes(srcAttrs.dwFileAttributes),
        nullptr));

    if (!dstHandle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    NTSTATUS sparseStatus = MarkPlaceholderSparseOrAbort(dstHandle, workPath);
    if (!NT_SUCCESS(sparseStatus)) {
        return sparseStatus;
    }

    // CreateFileW ignores FILE_ATTRIBUTE_COMPRESSED in its attributes, so
    // the shell sets compression here.
    SetCompressedIfSource(dstHandle.Get(), srcAttrs.dwFileAttributes);

    // Set file size without allocating disk space
    LARGE_INTEGER fileSize;
    fileSize.LowPart = srcAttrs.nFileSizeLow;
    fileSize.HighPart = static_cast<LONG>(srcAttrs.nFileSizeHigh);
    SetFilePointerEx(dstHandle.Get(), fileSize, nullptr, FILE_BEGIN);
    SetEndOfFile(dstHandle.Get());

    dstHandle.Reset();

    return CopyStagedFileMetadata(sourcePath, srcAttrs.dwFileAttributes, workPath);
}

NTSTATUS CopyUp::CopyUpMetadataOnly(const std::wstring& relativePath) {
    std::wstring normalized = NormalizePath(relativePath);

    // Fast-path: already committed before we even check the reservation.
    if (pathResolver_.ExistsInUpper(normalized)) {
        return STATUS_SUCCESS;
    }

    PathReservation reservation(*this, normalized);

    // A metacopy that waited on the reservation finds the copy of the one
    // before it here. Without this check, its move fails with
    // STATUS_OBJECT_NAME_COLLISION.
    if (pathResolver_.ExistsInUpper(normalized)) {
        return STATUS_SUCCESS;
    }

    const ResolvedPath lowerSource = pathResolver_.ResolveLowerPath(normalized);
    if (lowerSource.Found()) {
        bool hasUserStream = false;
        const NTSTATUS streamStatus =
            HasUserAlternateDataStream(lowerSource.absolutePath, &hasUserStream);
        if (!NT_SUCCESS(streamStatus)) {
            return streamStatus;
        }
        if (hasUserStream) {
            return STATUS_INVALID_PARAMETER;
        }
    }

    CopyUpTarget target;
    NTSTATUS status = PrepareCopyUpTarget(normalized, &target);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    const ResolvedPath& source = target.source;
    const std::wstring& upperPath = target.upperPath;

    // Get source file info for size and timestamps
    WIN32_FILE_ATTRIBUTE_DATA srcAttrs;
    if (!GetFileAttributesExW(source.absolutePath.c_str(), GetFileExInfoStandard, &srcAttrs)) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    const std::wstring workPath = GenerateWorkPath();
    FileBasicInfoGuard basicInfo(srcAttrs, StagedFileAttributes(srcAttrs.dwFileAttributes),
                                 workPath);

    status = StageMetacopyShellInWorkDir(source.absolutePath, srcAttrs, workPath);
    if (NT_SUCCESS(status)) {
        LayerMountMetadata metacopyMetadata = MakeCopyUpMetadata(source.absolutePath);
        metacopyMetadata.metacopy = true;
        status = RecordStagedFile(workPath, metacopyMetadata, basicInfo);
    }
    if (!NT_SUCCESS(status)) {
        basicInfo.Cancel();
        RemoveStagedFile(workPath, config_);
        return status;
    }

    return CommitStagedFile(normalized, workPath, upperPath);
}

FileCopyUpResult CopyUp::CopyUpFileOrShell(const std::wstring& relativePath,
                                           std::optional<LONGLONG> shellOnlyAboveBytes) {
    const auto copyInFull = [&] { return FileCopyUpResult{CopyUpFile(relativePath), false}; };

    // Without sparse files on the upper, a shell is a dense file of zeros.
    if (!config_.Capabilities().HasSparseFiles()) {
        return copyInFull();
    }
    const ResolvedPath source = pathResolver_.ResolveLowerPath(NormalizePath(relativePath));
    if (!source.Found()) {
        return copyInFull();
    }
    if (shellOnlyAboveBytes.has_value()) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!::GetFileAttributesExW(source.absolutePath.c_str(), GetFileExInfoStandard, &data)) {
            return copyInFull();
        }
        LARGE_INTEGER size{};
        size.LowPart = data.nFileSizeLow;
        size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
        if (size.QuadPart <= *shellOnlyAboveBytes) {
            return copyInFull();
        }
    }

    bool clonesReparsePoint = false;
    NTSTATUS status =
        ClonesReparsePoint(source.absolutePath, source.attributes, &clonesReparsePoint);
    if (!NT_SUCCESS(status)) {
        return {status, false};
    }
    if (clonesReparsePoint) {
        return copyInFull();
    }
    bool hasUserStream = false;
    status = HasUserAlternateDataStream(source.absolutePath, &hasUserStream);
    if (!NT_SUCCESS(status)) {
        return {status, false};
    }
    if (hasUserStream) {
        return copyInFull();
    }

    status = CopyUpMetadataOnly(relativePath);
    return {status, NT_SUCCESS(status)};
}

NTSTATUS CopyUp::CompleteLazyCopyUp(const std::wstring& relativePath) {
    std::wstring normalized = NormalizePath(relativePath);
    std::wstring upperPath = pathResolver_.GetUpperPath(normalized);

    // Serialize concurrent completions on the same path. Without this, two
    // callers on the same path can both enter here and both copy lower
    // bytes into the same upper file. The loser's still-running data copy
    // can clobber a user-write that landed between the two completions, or
    // simply duplicate writes onto the same handle range. Reservation makes
    // the second caller wait, then re-check `metacopy` and short-circuit.
    PathReservation reservation(*this, normalized);

    // Read metadata AFTER acquiring the reservation. A winner that ran
    // before us has already cleared the metacopy flag and may have applied
    // a user-write into upper; re-copying lower bytes here would clobber
    // that write. The post-lock read guarantees we see the winner's commit.
    LayerMountMetadata metadata = MetadataStore::ReadLayerMountMetadata(upperPath, &config_);
    if (!metadata.metacopy) {
        return STATUS_SUCCESS;
    }

    ScopedHandle srcHandle(CreateFileW(
        metadata.originLayer.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));

    if (!srcHandle.IsValid()) {
        return RecordFillFailure(normalized, L"open the origin file",
                                 ::LayerMount::NtStatusFromWin32(GetLastError()));
    }

    WIN32_FILE_ATTRIBUTE_DATA shellInfo{};
    const std::optional<DWORD> shellAttributes =
        GetFileAttributesExW(upperPath.c_str(), GetFileExInfoStandard, &shellInfo)
            ? std::optional<DWORD>(shellInfo.dwFileAttributes)
            : std::nullopt;
    // The write handle on the shell lives inside FillMetacopyShell, which
    // returns before this guard is destroyed, so the handle closes first.
    // The close of a written handle is the last write of LastWriteTime.
    FileBasicInfoGuard basicInfo(shellInfo, shellAttributes, upperPath);

    // A read-only shell refuses the data write.
    if (shellAttributes.has_value() && !ClearReadOnly(upperPath, *shellAttributes)) {
        return RecordFillFailure(normalized, L"clear the read-only attribute of the shell",
                                 StatusOfFailedCall(ERROR_ACCESS_DENIED));
    }

    NTSTATUS status = FillMetacopyShell(srcHandle, upperPath);
    if (!NT_SUCCESS(status)) {
        return RecordFillFailure(normalized, L"copy the origin's data into the shell",
                                 status);
    }

    status = FinishFilledShell(upperPath, metadata);
    if (!NT_SUCCESS(status)) {
        return RecordFillFailure(normalized, L"finish the filled shell", status);
    }

    if (!basicInfo.Restore()) {
        return RecordFillFailure(normalized,
                                 L"restore the attributes and times of the filled file",
                                 StatusOfFailedCall(ERROR_ACCESS_DENIED));
    }

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::FillMetacopyShell(ScopedHandle& srcHandle,
                                   const std::wstring& upperPath) {
    // Open destination (upper layer file) for writing. Share modes must
    // include SHARE_READ | SHARE_WRITE | SHARE_DELETE so a caller that
    // already holds a writable handle on the file does not fail this open
    // with a sharing violation.
    ScopedHandle dstHandle(CreateFileW(
        upperPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr));

    if (!dstHandle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    // Seek to beginning of both files
    LARGE_INTEGER zero = {};
    SetFilePointerEx(srcHandle.Get(), zero, nullptr, FILE_BEGIN);
    SetFilePointerEx(dstHandle.Get(), zero, nullptr, FILE_BEGIN);

    // Copy all data
    NTSTATUS status = CopyFileDataKeepingHoles(srcHandle.Get(), dstHandle.Get());
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // The shell must stay sparse while the data goes in, so the sparse
    // attribute comes off after the copy, not before it as in CopyUpFile.
    BY_HANDLE_FILE_INFORMATION originInfo{};
    if (!GetFileInformationByHandle(srcHandle.Get(), &originInfo)) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }
    if (!HasFileAttribute(originInfo.dwFileAttributes, FILE_ATTRIBUTE_SPARSE_FILE) &&
        !ClearSparseAndTrimAllocation(dstHandle.Get())) {
        const DWORD err = ::GetLastError();
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    srcHandle.Reset();
    dstHandle.Reset();

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::FinishFilledShell(const std::wstring& upperPath,
                                   LayerMountMetadata& metadata) {
    // The record write without the metacopy flag commits the fill. When it
    // fails, the data is in place but the flag stays set, so the next open
    // fills again. The file stays: another handle may hold it open, and a
    // user write may have landed after the data copy.
    metadata.metacopy = false;
    if (!MetadataStore::WriteLayerMountMetadata(upperPath, metadata, &config_)) {
        const DWORD err = ::GetLastError();
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
    }

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::CopyUpDirectory(const std::wstring& relativePath) {
    std::wstring normalized = NormalizePath(relativePath);

    PathReservation reservation(*this, normalized);

    if (reservation.ExistsInUpperWhileHeld()) {
        return STATUS_SUCCESS;
    }

    CopyUpTarget target;
    NTSTATUS status = PrepareCopyUpTarget(normalized, &target);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    const ResolvedPath& source = target.source;
    const std::wstring& upperPath = target.upperPath;

    bool clonesReparsePoint = false;
    status = ClonesReparsePoint(source.absolutePath, source.attributes, &clonesReparsePoint);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (clonesReparsePoint) {
        return CopyUpReparseCloneAndCount(normalized, target);
    }

    DWORD srcAttrs = GetFileAttributesW(source.absolutePath.c_str());
    ScopedHandle srcHandle(CreateFileW(
        source.absolutePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));
    const std::wstring stagedPath = GenerateWorkPath();
    FileBasicInfoGuard basicInfo(srcHandle.Get(), AttributesOrNone(srcAttrs), stagedPath);
    srcHandle.Reset();

    status = BuildStagedDirectory(source.absolutePath, stagedPath);
    if (NT_SUCCESS(status)) {
        // The attributes go on after the streams, because NTFS refuses a
        // new stream on a read-only directory.
        basicInfo.Restore();
        status = MoveUpperEntry(stagedPath, upperPath, ReplaceExisting::No, config_);
    }
    if (!NT_SUCCESS(status)) {
        RemoveUpperEntry(stagedPath, config_);
        return status;
    }

    cache_.InvalidateWithAncestors(normalized);
    RecordCopyUp(normalized);

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::BuildStagedDirectory(const std::wstring& sourcePath,
                                      const std::wstring& stagedPath) {
    if (!::CreateDirectoryW(stagedPath.c_str(), nullptr)) {
        return StatusOfFailedCall(ERROR_WRITE_FAULT);
    }
    // The staged directory has no children yet, so its layout goes on now
    // and passes to the entries created in it later.
    const NTSTATUS status = CopyDirectoryOwnMetadata(sourcePath, stagedPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    // A failed security copy is fatal: the directory's DACL is also the
    // template for auto-inheritance onto children created inside it, so
    // dropping the source's DACL would broaden or narrow access on every
    // child created later.
    if (!CopySecurityDescriptor(sourcePath, stagedPath)) {
        return StatusOfFailedCall(ERROR_ACCESS_DENIED);
    }

    // Without the record the directory has no origin and no stable file ID.
    if (!MetadataStore::WriteLayerMountMetadata(stagedPath, MakeCopyUpMetadata(sourcePath),
                                                &config_)) {
        return StatusOfFailedCall(ERROR_WRITE_FAULT);
    }
    return STATUS_SUCCESS;
}

RenameDestinationAside::RenameDestinationAside(ConfigRef config,
                                               WhiteoutManager& whiteoutMgr,
                                               Cache& cache)
    : config_(config.Get()), whiteoutMgr_(whiteoutMgr), cache_(cache) {}

RenameDestinationAside::~RenameDestinationAside() {
    if (asidePath_.empty()) {
        return;
    }
    if (::GetFileAttributesW(upperPath_.c_str()) != INVALID_FILE_ATTRIBUTES) {
        RemoveUpperEntry(asidePath_, config_);
    } else if (NT_SUCCESS(MoveUpperEntry(asidePath_, upperPath_, ReplaceExisting::No,
                                         config_)) &&
               wasOpaque_) {
        whiteoutMgr_.SetOpaque(normalizedPath_);
    }
    cache_.InvalidateWithAncestors(normalizedPath_);
}

void RenameDestinationAside::Hold(std::wstring normalizedPath,
                                  std::wstring upperPath,
                                  std::wstring asidePath,
                                  bool wasOpaque) {
    normalizedPath_ = std::move(normalizedPath);
    upperPath_ = std::move(upperPath);
    asidePath_ = std::move(asidePath);
    wasOpaque_ = wasOpaque;
}

void RenameDestinationAside::Commit() {
    if (asidePath_.empty()) {
        return;
    }
    RemoveUpperEntry(asidePath_, config_);
    asidePath_.clear();
}

NTSTATUS CopyUp::SetRenameDestinationAside(const std::wstring& newNorm,
                                           EntryKind destinationKind,
                                           RenameDestinationAside* aside) {
    const std::wstring upperPath = pathResolver_.GetStoredUpperPath(newNorm);
    const DWORD attributes = ::GetFileAttributesW(upperPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD probeErr = ::GetLastError();
        if (probeErr == ERROR_FILE_NOT_FOUND || probeErr == ERROR_PATH_NOT_FOUND) {
            return STATUS_SUCCESS;
        }
        return ::LayerMount::NtStatusFromWin32(probeErr);
    }
    // NTFS refuses a replace of a read-only file, but not a move of one into
    // the work directory.
    if (destinationKind == EntryKind::File &&
        (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        return STATUS_ACCESS_DENIED;
    }

    // A link gets no opaque marker, as overlayfs gives a symlink no opaque
    // xattr. The marker-file check would also go through the link into its
    // target.
    const bool wasOpaque = destinationKind == EntryKind::Directory &&
                           whiteoutMgr_.IsOpaque(newNorm);
    if (wasOpaque) {
        whiteoutMgr_.RemoveOpaque(newNorm);
    }
    const std::wstring asidePath = GenerateWorkPath();
    const NTSTATUS moveStatus = MoveUpperEntry(upperPath, asidePath, ReplaceExisting::No,
                                               config_);
    if (!NT_SUCCESS(moveStatus) && wasOpaque) {
        whiteoutMgr_.SetOpaque(newNorm);
    }
    // Runs on a failed move too, because the marker calls changed the upper.
    cache_.InvalidateWithAncestors(newNorm);
    if (!NT_SUCCESS(moveStatus)) {
        return moveStatus;
    }

    aside->Hold(newNorm, upperPath, asidePath, wasOpaque);
    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::RenameDirectoryCase(const CallerPath& oldCallerPath,
                                     const CallerPath& newCallerPath,
                                     EntryKind sourceKind) {
    const std::wstring normalized = NormalizePath(oldCallerPath.Text());
    const std::wstring newUpperPath = pathResolver_.GetUpperPathForNewEntry(newCallerPath);

    if (sourceKind == EntryKind::Link && !pathResolver_.ExistsInUpper(normalized)) {
        const ResolvedPath source = pathResolver_.ResolveLowerPath(normalized);
        if (source.Found()) {
            NTSTATUS status = EnsureUpperParent(NormalizePath(newCallerPath.Text()));
            if (!NT_SUCCESS(status)) {
                return status;
            }
            status = CloneReparsePointThroughWorkDir(
                {source.absolutePath, source.attributes}, GenerateWorkPath(), newUpperPath,
                {CopiedEntryRecord::NewFromSource, config_});
            if (!NT_SUCCESS(status)) {
                return status;
            }
            cache_.InvalidateWithAncestors(normalized);
            return STATUS_SUCCESS;
        }
    }

    NTSTATUS status = CopyUpDirectory(oldCallerPath.Text());
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = MoveUpperEntry(pathResolver_.GetStoredUpperPath(oldCallerPath.Text()),
                            newUpperPath, ReplaceExisting::No, config_);
    cache_.InvalidateWithAncestors(normalized);
    return status;
}

NTSTATUS CopyUp::EnsureUpperParent(const std::wstring& normalizedPath) {
    const size_t separator = normalizedPath.find_last_of(L'\\');
    if (separator == std::wstring::npos) {
        return STATUS_SUCCESS;
    }
    const std::wstring parent = normalizedPath.substr(0, separator);
    if (pathResolver_.ExistsInUpper(parent)) {
        return STATUS_SUCCESS;
    }

    const ResolvedPath shown = pathResolver_.ResolvePath(parent);
    if (!shown.Found() || shown.source != LayerSource::Lower ||
        (shown.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    return CopyUpDirectory(parent);
}

bool CopyUp::CopySecurityDescriptor(const std::wstring& srcPath,
                                     const std::wstring& dstPath) {
    // Audit ACEs survive copy-up only when the process holds SE_SECURITY_NAME.
    const SECURITY_INFORMATION secInfo = DropSaclWithoutPrivilege(
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION);

    DWORD sdSize = 0;
    GetFileSecurityW(srcPath.c_str(), secInfo, nullptr, 0, &sdSize);
    if (sdSize == 0) {
        return false;
    }

    std::vector<BYTE> sdBuffer(sdSize);
    auto* sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(sdBuffer.data());

    if (!GetFileSecurityW(srcPath.c_str(), secInfo, sd, sdSize, &sdSize)) {
        return false;
    }

    return SetFileSecurityW(dstPath.c_str(), secInfo, sd) != FALSE;
}

bool CopyUp::CopyTimestamps(HANDLE srcHandle, HANDLE dstHandle) {
    FILETIME creation, access, write;
    if (!GetFileTime(srcHandle, &creation, &access, &write)) {
        return false;
    }
    return SetFileTime(dstHandle, &creation, &access, &write) != FALSE;
}

}
