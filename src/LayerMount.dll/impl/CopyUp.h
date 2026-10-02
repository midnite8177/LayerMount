#pragma once

#include "LayerMount.h"
#include "LayerPath.h"
#include "UpperParent.h"
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

// An upper destination entry that CopyUp::SetRenameDestinationAside moved
// into the work directory. Until Commit runs, the destructor acts on a
// failed rename. When the destination path is free, it moves the entry
// back and restores a directory's opaque marker. When the rename placed an
// entry at the path, it removes the copy in the work directory. Commit
// removes that copy. An object that holds no entry does nothing.
class RenameDestinationAside {
public:
    RenameDestinationAside() = default;
    ~RenameDestinationAside();

    RenameDestinationAside(const RenameDestinationAside&) = delete;
    RenameDestinationAside& operator=(const RenameDestinationAside&) = delete;

    // Takes the entry that moved from upperPath to asidePath. The
    // destructor moves it back with restoreCopy and, when wasOpaque is
    // true, marks normalizedPath opaque again.
    void Hold(WhiteoutManager* whiteoutMgr,
              Cache* cache,
              std::wstring normalizedPath,
              std::wstring upperPath,
              std::wstring asidePath,
              bool wasOpaque,
              CopyAcrossVolumes restoreCopy);

    void Commit();

private:
    WhiteoutManager* whiteoutMgr_ = nullptr;
    Cache* cache_ = nullptr;
    std::wstring normalizedPath_;
    std::wstring upperPath_;
    std::wstring asidePath_;
    bool wasOpaque_ = false;
    CopyAcrossVolumes restoreCopy_ = CopyAcrossVolumes::No;
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

    // Makes the parent exist in the upper first, as EnsureUpperParent does.
    // Preserves security, timestamps, data.
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

    // Makes the parent of callerPath exist in the upper, as
    // UpperParent::Ensure does.
    NTSTATUS EnsureUpperParent(const CallerPath& callerPath);

    // Copies the lower tree and the old upper shadow to the new upper path,
    // marks it opaque, and removes the old upper entry. The caller writes the
    // whiteout at the old path. sourceKind is Directory or Link. A Link
    // source, when the upper supports reparse points, is copied as a link,
    // without its upper shadow and without opacity.
    // ReplaceExisting::No fails with STATUS_OBJECT_NAME_COLLISION when the
    // destination exists in the merged view. The new upper entry gets its
    // name in newCallerPath's case.
    NTSTATUS RenameLowerDirectory(const CallerPath& oldCallerPath,
                                  const CallerPath& newCallerPath,
                                  RenameEntryKind sourceKind,
                                  ReplaceExisting replace);

    // Before the move, makes the new parent exist in the upper, as
    // UpperParent::Ensure does. Moves the upper directory and carries its
    // opaque marker. Marks the moved entry opaque when a lower layer has
    // its new path, so lower children of a replaced destination stay
    // hidden. sourceKind is Directory or Link. A moved Link never becomes
    // opaque, because the marker would go into its target. The moved entry
    // gets its name in newCallerPath's case. When a lower layer has the old
    // path, the caller writes the whiteout there.
    // ReplaceExisting::No fails with STATUS_OBJECT_NAME_COLLISION when the
    // destination exists in the merged view.
    NTSTATUS RenameUpperDirectory(const CallerPath& oldCallerPath,
                                  const CallerPath& newCallerPath,
                                  RenameEntryKind sourceKind,
                                  ReplaceExisting replace);

    // Moves the upper entry at newNorm into the work directory, so a
    // replace-rename can put its source at the path. destinationKind is the
    // kind of the merged view's entry at newNorm. A Directory loses its
    // opaque marker first. A Link moves as a link, and its target keeps its
    // markers. Nothing moves and aside stays empty when the upper has no
    // entry at newNorm, or when both kinds are File, because the move of a
    // file replaces a file. When the work directory is on another volume,
    // a File destination moves there as a copy. A Directory or a Link
    // destination cannot, so the method removes it at once and aside stays
    // empty, and a failed rename cannot restore it. aside must be empty.
    NTSTATUS SetRenameDestinationAside(const std::wstring& newNorm,
                                       RenameEntryKind sourceKind,
                                       RenameEntryKind destinationKind,
                                       RenameDestinationAside* aside);

    // Renames a directory whose old and new paths differ only in case.
    // Copies a lower directory up first, then renames the upper entry in
    // place to newCallerPath's case. RenameLowerDirectory and
    // RenameUpperDirectory do not fit, because old and new name the same
    // entry, so they would copy the tree onto itself, delete it and white
    // it out. The move needs no replace flag for the same reason. A lower
    // junction or directory symlink, when the upper lacks reparse-point
    // support, is copied up as a plain opaque directory holding its
    // target's tree.
    NTSTATUS RenameDirectoryCase(const CallerPath& oldCallerPath,
                                 const CallerPath& newCallerPath);

private:
    bool DestinationExistsInMerged(const std::wstring& normalizedPath) const;

    // Fails with STATUS_OBJECT_NAME_COLLISION when replace is
    // ReplaceExisting::No and the destination exists in the merged view.
    // Then makes the parent of newCallerPath exist in the upper.
    NTSTATUS PrepareRenameDestination(const CallerPath& newCallerPath,
                                      ReplaceExisting replace);

    NTSTATUS OverlayUpperShadow(const std::wstring& oldUpperPath,
                                const std::wstring& newUpperPath);

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

    // The lower entry a copy-up reads and the upper path it writes.
    struct CopyUpTarget {
        ResolvedPath source;
        std::wstring upperPath;
    };

    // Finds the first visible lower that holds normalized, makes the upper
    // parent exist, and names the upper path after the lower entry. Returns
    // STATUS_OBJECT_NAME_NOT_FOUND when no visible lower holds normalized.
    NTSTATUS PrepareCopyUpTarget(const std::wstring& normalized, CopyUpTarget* target);

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
    UpperParent upperParent_;

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
