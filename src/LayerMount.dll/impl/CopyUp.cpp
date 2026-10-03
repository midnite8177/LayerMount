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
        std::wstring filePath = JoinDirPath(config_.workDirPath, findData.cFileName);
        DeleteFileW(filePath.c_str());
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
}

NTSTATUS CopyUp::CommitFromWorkDir(const std::wstring& workPath,
                                    const std::wstring& finalUpperPath) {
    std::filesystem::path parentDir = std::filesystem::path(finalUpperPath).parent_path();
    if (!parentDir.empty()) {
        EnsureDirectoryExists(parentDir.wstring());
    }

    // Without MOVEFILE_COPY_ALLOWED, MoveFileExW fails with
    // ERROR_NOT_SAME_DEVICE when the work directory and the upper are on
    // different volumes. With it, the move falls back to a copy and a delete,
    // which is not crash-safe. On one volume the move stays an atomic rename.
    DWORD flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH |
                  MOVEFILE_COPY_ALLOWED;
    if (MoveFileExW(workPath.c_str(), finalUpperPath.c_str(), flags)) {
        return STATUS_SUCCESS;
    }

    const DWORD err = GetLastError();
    if (err != ERROR_ACCESS_DENIED) {
        DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err);
    }

    // Fallback path for restrictive parent ACLs. When CopyUpDirectory copied
    // the lower parent's DACL up (e.g. an inherited DENY-WRITE for Everyone),
    // upper\<parent> ends up denying WRITE to our own process — even though
    // we created and own that directory. MoveFileExW then fails at the
    // destination's ACL check with ERROR_ACCESS_DENIED.
    //
    // SE_RESTORE_NAME (enabled in EnableFileSystemPrivileges) lets a backup-
    // semantics-opened source handle perform a rename via FileRenameInfo
    // that bypasses the destination directory's DACL. This is the same
    // mechanism backup/restore tools use to write into protected paths.
    ScopedHandle src(CreateFileW(workPath.c_str(),
                                  GENERIC_READ | DELETE | SYNCHRONIZE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!src.IsValid()) {
        const DWORD openErr = GetLastError();
        DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(openErr);
    }

    // FILE_RENAME_INFO is a variable-length struct: a fixed header plus the
    // destination path as UTF-16 (byte length in FileNameLength, NOT
    // null-terminated). Allocate one contiguous buffer so the kernel can
    // walk it without a separate allocation.
    const size_t pathBytes = finalUpperPath.size() * sizeof(wchar_t);
    std::vector<BYTE> buf(sizeof(FILE_RENAME_INFO) + pathBytes);
    auto* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
    ri->ReplaceIfExists = TRUE;
    ri->RootDirectory = nullptr;
    ri->FileNameLength = static_cast<DWORD>(pathBytes);
    memcpy(ri->FileName, finalUpperPath.data(), pathBytes);

    if (!SetFileInformationByHandle(src.Get(), FileRenameInfo, ri,
                                      static_cast<DWORD>(buf.size()))) {
        const DWORD renameErr = GetLastError();
        src.Reset();
        DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(renameErr);
    }

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::CopyUpLinkAndCount(const std::wstring& normalized,
                                    const CopyUpTarget& target) {
    const NTSTATUS status = CopyLinkWithCopyUpRecord(
        target.source.absolutePath, target.source.attributes, target.upperPath,
        {CopiedEntryRecord::NewFromSource, config_, capabilities_});
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
        capabilities_.HasSparseFiles() &&
        !SetSparse(dstHandle.Get())) {
        const NTSTATUS status = SparseRefusalStatus();
        dstHandle.Reset();
        ::DeleteFileW(workPath.c_str());
        return status;
    }

    SetCompressedIfSource(dstHandle.Get(), srcAttrs);

    NTSTATUS status = CopyFileDataKeepingHoles(srcHandle.Get(), dstHandle.Get());
    if (!NT_SUCCESS(status)) {
        dstHandle.Reset();
        DeleteFileW(workPath.c_str());
        return status;
    }

    // Close handles before commit (MoveFileEx needs exclusive access)
    srcHandle.Reset();
    dstHandle.Reset();

    if (!ApplyEncryptedStateIfNeeded(workPath, srcAttrs)) {
        DWORD err = ::GetLastError();
        ::DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    // Copy security descriptor (path-based, handles already closed).
    // Security-descriptor failures are fatal: committing with inherited or
    // default DACL can broaden access relative to the source, silently
    // changing access-control semantics after the first write. Tear down
    // the staged work-dir copy so the caller retries from a clean state.
    if (!CopySecurityDescriptor(sourcePath, workPath)) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::FinishCommittedFile(const std::wstring& sourcePath,
                                     const std::wstring& upperPath,
                                     FileBasicInfoGuard& basicInfo) {
    const NTSTATUS streamStatus = CopyUserAlternateDataStreams(sourcePath, upperPath);
    if (!NT_SUCCESS(streamStatus)) {
        ::DeleteFileW(upperPath.c_str());
        return streamStatus;
    }

    LayerMountMetadata metadata = MakeCopyUpMetadata(sourcePath);
    NTSTATUS metadataStatus = WriteCopyUpRecordOrRemoveEntry(
        upperPath, metadata, NewUpperEntryKind::File, config_);
    if (!NT_SUCCESS(metadataStatus)) {
        return metadataStatus;
    }

    if (!basicInfo.Restore()) {
        const DWORD err = ::GetLastError();
        RemoveUpperEntry(upperPath, config_);
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
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

    // Serialize concurrent copy-ups to the same path. The reservation blocks
    // any second thread until ours completes; on wake-up the second thread
    // re-checks ExistsInUpper below and short-circuits to STATUS_SUCCESS
    // because our commit has already landed. Without serialization, racers
    // would all fight at MoveFileExW commit time (losers see ACCESS_DENIED
    // or sharing violations from the just-placed target).
    PathReservation reservation(*this, normalized);

    // Check if already in upper layer (could have been copied by concurrent thread)
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

    // A data copy would follow the link and put the target's data in a plain
    // upper file.
    if ((source.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return CopyUpLinkAndCount(normalized, target);
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
    FileBasicInfoGuard basicInfo(srcHandle.Get(), AttributesOrNone(srcAttrs), upperPath);

    std::wstring workPath = GenerateWorkPath();
    status = StageFileInWorkDir(source.absolutePath, srcHandle, srcAttrs, workPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Atomic commit from work dir to upper layer
    status = CommitFromWorkDir(workPath, upperPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = FinishCommittedFile(source.absolutePath, upperPath, basicInfo);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    cache_.InvalidateWithAncestors(normalized);
    RecordCopyUp(normalized);

    // Reservation released by RAII at scope exit; waiters then re-check
    // ExistsInUpper and short-circuit with SUCCESS — our commit is done.
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
        srcAttrs.dwFileAttributes,
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

    if (!ApplyEncryptedStateIfNeeded(workPath, srcAttrs.dwFileAttributes)) {
        DWORD err = ::GetLastError();
        ::DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    // Copy security descriptor. Fatal on failure -- see StageFileInWorkDir
    // for the reasoning. Tear down the staged work-dir copy on failure.
    if (!CopySecurityDescriptor(sourcePath, workPath)) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::CopyUpMetadataOnly(const std::wstring& relativePath) {
    std::wstring normalized = NormalizePath(relativePath);

    // Fast-path: already committed before we even check the reservation.
    if (pathResolver_.ExistsInUpper(normalized)) {
        return STATUS_SUCCESS;
    }

    // Serialize concurrent metacopy stages on the same path. Without this,
    // two threads can both stage a sparse shell into the work dir and race
    // at MoveFileExW commit. The loser sees ACCESS_DENIED and leaves an
    // orphaned work file behind, while the winner's metacopy can later be
    // overwritten by an interleaved second commit. Same invariant CopyUpFile
    // enforces.
    PathReservation reservation(*this, normalized);

    // Re-check after the reservation is held. A winner can have committed
    // while we waited, in which case there's nothing to do.
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

    // Get source file info for size and timestamps
    WIN32_FILE_ATTRIBUTE_DATA srcAttrs;
    if (!GetFileAttributesExW(source.absolutePath.c_str(), GetFileExInfoStandard, &srcAttrs)) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    // CreateFileW below sets the attribute bits on the shell, so the guard
    // carries the times only.
    FileBasicInfoGuard basicInfo(srcAttrs, std::nullopt, upperPath);

    std::wstring workPath = GenerateWorkPath();
    status = StageMetacopyShellInWorkDir(source.absolutePath, srcAttrs, workPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Atomic commit
    status = CommitFromWorkDir(workPath, upperPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    LayerMountMetadata metacopyMetadata = MakeCopyUpMetadata(source.absolutePath);
    metacopyMetadata.metacopy = true;
    NTSTATUS metacopyMetadataStatus = WriteCopyUpRecordOrRemoveEntry(
        upperPath, metacopyMetadata, NewUpperEntryKind::File, config_);
    if (!NT_SUCCESS(metacopyMetadataStatus)) {
        return metacopyMetadataStatus;
    }

    basicInfo.Restore();

    cache_.InvalidateWithAncestors(normalized);
    RecordCopyUp(normalized);

    return STATUS_SUCCESS;
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
    GetFileAttributesExW(upperPath.c_str(), GetFileExInfoStandard, &shellInfo);
    // The write handle on the shell lives inside FillMetacopyShell, which
    // returns before this guard is destroyed, so the handle closes first.
    // The close of a written handle is the last write of LastWriteTime.
    FileBasicInfoGuard basicInfo(shellInfo, std::nullopt, upperPath);

    NTSTATUS status = FillMetacopyShell(srcHandle, upperPath);
    if (!NT_SUCCESS(status)) {
        return RecordFillFailure(normalized, L"copy the origin's data into the shell",
                                 status);
    }

    status = FinishFilledShell(upperPath, metadata);
    if (!NT_SUCCESS(status)) {
        return RecordFillFailure(normalized, L"finish the filled shell", status);
    }

    basicInfo.Restore();

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
    // A stream failure returns before the code clears the metacopy flag, so
    // the next open tries the fill again.
    const NTSTATUS streamStatus =
        CopyUserAlternateDataStreams(metadata.originLayer, upperPath);
    if (!NT_SUCCESS(streamStatus)) {
        return streamStatus;
    }

    // Clear metacopy flag. Metadata persistence is the atomic commit point
    // for the completion: if it fails, the upper file's data is in place
    // but the metacopy flag is still set (we haven't re-entered ADS yet),
    // so subsequent resolutions will attempt the completion again. That
    // is the correct retry shape. Do NOT delete upperPath here -- the
    // data has been copied, and another handle may already be holding it
    // open; tearing it down would clobber user writes that may have
    // landed between the data copy and here. Surface the failure so the
    // caller sees the completion did not commit.
    metadata.metacopy = false;
    if (!MetadataStore::WriteLayerMountMetadata(upperPath, metadata, &config_)) {
        const DWORD err = ::GetLastError();
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
    }

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::CopyUpDirectory(const std::wstring& relativePath) {
    std::wstring normalized = NormalizePath(relativePath);

    // Taken before the upper check and the link branch. Otherwise a racing
    // copy-up adopts the directory that this call creates and removes it when
    // the racer fails.
    PathReservation reservation(*this, normalized);

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

    // Without this branch a lower junction or directory symlink copies up as
    // a plain empty directory and loses its reparse tag.
    if ((source.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return CopyUpLinkAndCount(normalized, target);
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
    FileBasicInfoGuard basicInfo(srcHandle.Get(), AttributesOrNone(srcAttrs), upperPath);
    srcHandle.Reset();

    status = BuildUpperDirectory(source.absolutePath, upperPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // The attributes go on after the streams, because NTFS refuses a new
    // stream on a read-only directory.
    basicInfo.Restore();

    cache_.InvalidateWithAncestors(normalized);
    RecordCopyUp(normalized);

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::BuildUpperDirectory(const std::wstring& sourcePath,
                                     const std::wstring& upperPath) {
    // A failed security copy is fatal: the directory's DACL is also the
    // template for auto-inheritance onto children created inside it, so
    // dropping the source's DACL would broaden or narrow access on every
    // child created later.
    NTSTATUS status = CopyDirectoryShell(sourcePath, upperPath);
    if (NT_SUCCESS(status) && !CopySecurityDescriptor(sourcePath, upperPath)) {
        const DWORD err = ::GetLastError();
        status = ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }
    if (!NT_SUCCESS(status)) {
        ::RemoveDirectoryW(upperPath.c_str());
        return status;
    }

    // Write the copy-up record. Fatal on failure: without origin/stable-id
    // the upper directory looks like a foreign creation and later
    // resolution can misbehave. Tear down the staged upper directory.
    return WriteCopyUpRecordOrRemoveEntry(upperPath, MakeCopyUpMetadata(sourcePath),
                                          NewUpperEntryKind::Directory, config_);
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
                                         restoreCopy_, config_)) &&
               wasOpaque_) {
        whiteoutMgr_.SetOpaque(normalizedPath_);
    }
    cache_.InvalidateWithAncestors(normalizedPath_);
}

void RenameDestinationAside::Hold(std::wstring normalizedPath,
                                  std::wstring upperPath,
                                  std::wstring asidePath,
                                  bool wasOpaque,
                                  CopyAcrossVolumes restoreCopy) {
    normalizedPath_ = std::move(normalizedPath);
    upperPath_ = std::move(upperPath);
    asidePath_ = std::move(asidePath);
    wasOpaque_ = wasOpaque;
    restoreCopy_ = restoreCopy;
}

void RenameDestinationAside::Commit() {
    if (asidePath_.empty()) {
        return;
    }
    RemoveUpperEntry(asidePath_, config_);
    asidePath_.clear();
}

NTSTATUS CopyUp::SetRenameDestinationAside(const std::wstring& newNorm,
                                           RenameEntryKind destinationKind,
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
    if (destinationKind == RenameEntryKind::File &&
        (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        return STATUS_ACCESS_DENIED;
    }

    // A link gets no opaque marker, as overlayfs gives a symlink no opaque
    // xattr. The marker-file check would also go through the link into its
    // target.
    const bool wasOpaque = destinationKind == RenameEntryKind::Directory &&
                           whiteoutMgr_.IsOpaque(newNorm);
    if (wasOpaque) {
        whiteoutMgr_.RemoveOpaque(newNorm);
    }
    const CopyAcrossVolumes copy = destinationKind == RenameEntryKind::File
        ? CopyAcrossVolumes::Yes
        : CopyAcrossVolumes::No;
    const std::wstring asidePath = GenerateWorkPath();
    NTSTATUS moveStatus = MoveUpperEntry(upperPath, asidePath, ReplaceExisting::No, copy, config_);
    if (moveStatus == ::LayerMount::NtStatusFromWin32(ERROR_NOT_SAME_DEVICE)) {
        // MoveFileExW cannot move a directory or a link to another volume.
        // The entry goes at once, so a failed rename cannot restore it.
        moveStatus = RemoveUpperEntry(upperPath, config_);
        cache_.InvalidateWithAncestors(newNorm);
        return moveStatus;
    }
    if (!NT_SUCCESS(moveStatus) && wasOpaque) {
        whiteoutMgr_.SetOpaque(newNorm);
    }
    // Runs on a failed move too, because the marker calls changed the upper.
    cache_.InvalidateWithAncestors(newNorm);
    if (!NT_SUCCESS(moveStatus)) {
        return moveStatus;
    }

    aside->Hold(newNorm, upperPath, asidePath, wasOpaque, copy);
    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::RenameDirectoryCase(const CallerPath& oldCallerPath,
                                     const CallerPath& newCallerPath,
                                     RenameEntryKind sourceKind) {
    const std::wstring normalized = NormalizePath(oldCallerPath.Text());
    const std::wstring newUpperPath = pathResolver_.GetUpperPathForNewEntry(newCallerPath);

    if (sourceKind == RenameEntryKind::Link && !pathResolver_.ExistsInUpper(normalized)) {
        const ResolvedPath source = pathResolver_.ResolveLowerPath(normalized);
        if (source.Found()) {
            NTSTATUS status = EnsureUpperParent(NormalizePath(newCallerPath.Text()));
            if (!NT_SUCCESS(status)) {
                return status;
            }
            status = CopyLinkWithCopyUpRecord(
                source.absolutePath, source.attributes, newUpperPath,
                {CopiedEntryRecord::NewFromSource, config_, capabilities_});
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
                            newUpperPath, ReplaceExisting::No, CopyAcrossVolumes::No, config_);
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
