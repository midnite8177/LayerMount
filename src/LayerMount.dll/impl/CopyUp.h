#pragma once

#include "LayerMount.h"
#include "LayerPath.h"
#include "ScopedHandle.h"
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

// An upper destination entry that CopyUp::SetRenameDestinationAside moved
// into the work directory. Until Commit runs, the destructor acts on a
// failed rename. When the destination path is free, it moves the entry
// back and restores a directory's opaque marker. When the rename placed an
// entry at the path, it removes the copy in the work directory. Commit
// removes that copy. An object that holds no entry does nothing.
class RenameDestinationAside {
public:
    RenameDestinationAside(ConfigRef config, WhiteoutManager& whiteoutMgr, Cache& cache);
    ~RenameDestinationAside();

    RenameDestinationAside(const RenameDestinationAside&) = delete;
    RenameDestinationAside& operator=(const RenameDestinationAside&) = delete;

    // Takes the entry that moved from upperPath to asidePath. The
    // destructor moves it back with restoreCopy and, when wasOpaque is
    // true, marks normalizedPath opaque again.
    void Hold(std::wstring normalizedPath,
              std::wstring upperPath,
              std::wstring asidePath,
              bool wasOpaque,
              CopyAcrossVolumes restoreCopy);

    void Commit();

private:
    const LayerConfig& config_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
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

    // Copies a lower parent directory up first. Preserves security,
    // timestamps, data.
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

    // Returns success when the parent of normalizedPath is in the upper.
    // Copies the parent up when the overlay shows it as a lower directory,
    // so the upper entry takes the lower's name. Returns
    // STATUS_OBJECT_PATH_NOT_FOUND and writes nothing for every other
    // parent. Overlayfs fails with ENOTDIR when the parent or an ancestor
    // is a file, and with ENOENT when the overlay shows no parent.
    NTSTATUS EnsureUpperParent(const std::wstring& normalizedPath);

    // Moves the upper entry at newNorm into the work directory, so a
    // replace-rename can put its source at the path. destinationKind is
    // the kind of the merged view's entry at newNorm. A Directory loses
    // its opaque marker first. A Link moves as a link, and its target
    // keeps its markers. Nothing moves and aside stays empty when the
    // upper has no entry at newNorm. A read-only File destination fails
    // with STATUS_ACCESS_DENIED, and nothing moves. When the work
    // directory is on another volume, a File destination moves there as a
    // copy. A Directory or a Link destination cannot, so the method
    // removes it at once and aside stays empty, and a failed rename cannot
    // restore it. aside must be empty.
    NTSTATUS SetRenameDestinationAside(const std::wstring& newNorm,
                                       RenameEntryKind destinationKind,
                                       RenameDestinationAside* aside);

    // Renames a directory whose old and new paths differ only in case.
    // Copies a lower directory up first, then renames the upper entry in
    // place to newCallerPath's case. sourceKind is Directory or Link. A
    // lower Link source goes to the new name as a link, with a copy-up
    // record and no opaque marker.
    NTSTATUS RenameDirectoryCase(const CallerPath& oldCallerPath,
                                 const CallerPath& newCallerPath,
                                 RenameEntryKind sourceKind);

private:
    bool CopySecurityDescriptor(const std::wstring& srcPath, const std::wstring& dstPath);

    bool CopyTimestamps(HANDLE srcHandle, HANDLE dstHandle);

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

    NTSTATUS CopyUpLinkAndCount(const std::wstring& normalized, const CopyUpTarget& target);

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
