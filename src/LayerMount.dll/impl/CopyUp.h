#pragma once

#include "LayerMount.h"
#include "LayerPath.h"
#include "../abi/CapabilityGate.h"
#include "../abi/EventEmitter.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_set>

namespace LayerMount {

class PathResolver;
class WhiteoutManager;
class Cache;
class FileBasicInfoGuard;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = INVALID_HANDLE_VALUE) noexcept : h_(h) {}
    ~ScopedHandle() { Close(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : h_(other.h_) { other.h_ = INVALID_HANDLE_VALUE; }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) { Close(); h_ = other.h_; other.h_ = INVALID_HANDLE_VALUE; }
        return *this;
    }

    HANDLE Get() const noexcept { return h_; }
    bool IsValid() const noexcept { return h_ != INVALID_HANDLE_VALUE && h_ != nullptr; }
    HANDLE Release() noexcept { HANDLE h = h_; h_ = INVALID_HANDLE_VALUE; return h; }
    void Reset(HANDLE h = INVALID_HANDLE_VALUE) noexcept { Close(); h_ = h; }

private:
    void Close() noexcept {
        if (IsValid()) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    }
    HANDLE h_;
};

class CopyUp {
public:
    CopyUp(ConfigRef config,
           PathResolver& pathResolver,
           WhiteoutManager& whiteoutMgr,
           Cache& cache,
           LayerMountStats& stats);

    void SetCapabilityGate(::LayerMount::abi::CapabilityGate gate) noexcept { capabilities_ = gate; }
    void SetEventEmitter(::LayerMount::abi::EventEmitter* events) noexcept { events_ = events; }

    // Bump the copy-up stat counter and emit LM_EVT_COPY_UP to the host
    // callback when one is installed.
    void RecordCopyUp(const std::wstring& relativePath);

    std::wstring GenerateWorkPath();

    // Deletes every #*.tmp in the work directory. Call it only when no
    // copy-up is in flight.
    void CleanWorkDirectory();

    // Replaces finalUpperPath. Atomic only when the work directory and the
    // upper share a volume.
    NTSTATUS CommitFromWorkDir(const std::wstring& workPath,
                               const std::wstring& finalUpperPath);

    // Creates parent directories as needed. Preserves security, timestamps, data.
    NTSTATUS CopyUpFile(const std::wstring& relativePath);

    // Copy only metadata (security, timestamps) as a sparse file. Data copied on demand.
    NTSTATUS CopyUpMetadataOnly(const std::wstring& relativePath);

    // Complete a metacopy by copying actual file data from the lower layer.
    // Keeps the shell's timestamps, so a set-times on the sparse shell
    // survives the fill. Clears the metacopy ADS flag when done.
    NTSTATUS CompleteLazyCopyUp(const std::wstring& relativePath);

    // Copy a directory entry (not contents) from a lower layer to the upper layer.
    // The upper entry takes the lower entry's name, whatever the case of
    // relativePath.
    NTSTATUS CopyUpDirectory(const std::wstring& relativePath);

    // Copies the lower tree and the old upper shadow to the new upper path,
    // marks it opaque, and whites out the old path. A junction or directory
    // symlink source, when the upper supports reparse points, is copied as
    // a link, without its upper shadow and without opacity.
    // ReplaceExisting::No fails with STATUS_OBJECT_NAME_COLLISION when the
    // destination exists in the merged view. The new upper entry gets its
    // name in newCallerPath's case.
    NTSTATUS RenameLowerDirectory(const std::wstring& oldCallerPath,
                                  const std::wstring& newCallerPath,
                                  ReplaceExisting replace);

    // Moves the upper directory and carries its opaque marker. The moved
    // entry gets its name in newCallerPath's case.
    // ReplaceExisting::No fails with STATUS_OBJECT_NAME_COLLISION when the
    // destination exists in the merged view.
    NTSTATUS RenameUpperDirectory(const std::wstring& oldCallerPath,
                                  const std::wstring& newCallerPath,
                                  ReplaceExisting replace);

    // Renames a directory whose old and new paths differ only in case.
    // Copies a lower directory up first, then renames the upper entry in
    // place to newCallerPath's case. RenameLowerDirectory and
    // RenameUpperDirectory do not fit, because old and new name the same
    // entry, so they would copy the tree onto itself, delete it and white
    // it out. The move needs no replace flag for the same reason. A lower
    // junction or directory symlink, when the upper lacks reparse-point
    // support, is copied up as a plain opaque directory holding its
    // target's tree.
    NTSTATUS RenameDirectoryCase(const std::wstring& oldCallerPath,
                                 const std::wstring& newCallerPath);

private:
    bool DestinationExistsInMerged(const std::wstring& normalizedPath) const;

    NTSTATUS OverlayUpperShadow(const std::wstring& oldUpperPath,
                                const std::wstring& newUpperPath);

    NTSTATUS EnsureParentDirectories(const std::wstring& relativePath);

    NTSTATUS CopyTreePreservingMetadata(const std::wstring& srcAbs,
                                         const std::wstring& dstAbs);

    NTSTATUS CopyDirectoryTree(const std::wstring& srcAbs,
                               const std::wstring& dstAbs);

    bool CopySecurityDescriptor(const std::wstring& srcPath, const std::wstring& dstPath);

    bool CopyTimestamps(HANDLE srcHandle, HANDLE dstHandle);

    NTSTATUS WriteCopyUpMetadataOrAbort(const std::wstring& upperPath,
                                        const LayerMountMetadata& metadata);

    NTSTATUS MarkPlaceholderSparseOrAbort(ScopedHandle& dstHandle,
                                          const std::wstring& workPath);

    NTSTATUS CopyUpReparseEntry(const std::wstring& normalized,
                                const ResolvedPath& source,
                                const std::wstring& upperPath);

    NTSTATUS StageFileInWorkDir(const std::wstring& sourcePath,
                                ScopedHandle& srcHandle,
                                DWORD srcAttrs,
                                const std::wstring& workPath);

    NTSTATUS FinishCommittedFile(const std::wstring& sourcePath,
                                 const std::wstring& upperPath,
                                 FileBasicInfoGuard& basicInfo);

    NTSTATUS StageMetacopyShellInWorkDir(const std::wstring& sourcePath,
                                         const WIN32_FILE_ATTRIBUTE_DATA& srcAttrs,
                                         const std::wstring& workPath);

    NTSTATUS FillMetacopyShell(ScopedHandle& srcHandle,
                               const std::wstring& upperPath);

    NTSTATUS FinishFilledShell(const std::wstring& upperPath,
                               LayerMountMetadata& metadata);

    NTSTATUS SecureAndTagUpperDirectory(const std::wstring& sourcePath,
                                        const std::wstring& upperPath);

    const LayerConfig& config_;
    PathResolver& pathResolver_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
    LayerMountStats& stats_;
    ::LayerMount::abi::CapabilityGate capabilities_{
        LM_CAP_ADS | LM_CAP_REPARSE_POINTS | LM_CAP_SPARSE_FILES |
        LM_CAP_MULTIPLE_STREAMS | LM_CAP_NTFS_ACLS
    };
    ::LayerMount::abi::EventEmitter* events_ = nullptr;

    std::atomic<uint64_t> workCounter_{0};

    // Serializes concurrent copy-ups to the same path. A caller that finds
    // its target path already in inFlightCopyUps_ waits on copyUpCV_ until
    // the winning thread clears the entry, then re-checks ExistsInUpper —
    // the winner's committed upper file short-circuits the waiter with
    // STATUS_SUCCESS. This prevents concurrent racers from all fighting at
    // MoveFileExW commit time (where losers see ERROR_ACCESS_DENIED or
    // sharing-violation from a just-placed target).
    std::mutex copyUpMutex_;
    std::condition_variable copyUpCV_;
    std::unordered_set<std::wstring> inFlightCopyUps_;
};

}
