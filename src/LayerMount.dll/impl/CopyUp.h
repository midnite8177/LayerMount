#pragma once

#include "LayerMount.h"
#include "LayerPath.h"
#include "ScopedHandle.h"
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
    // destructor moves it back and, when wasOpaque is true, marks
    // normalizedPath opaque again.
    void Hold(std::wstring normalizedPath,
              std::wstring upperPath,
              std::wstring asidePath,
              bool wasOpaque);

    void Commit();

private:
    const LayerConfig& config_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
    std::wstring normalizedPath_;
    std::wstring upperPath_;
    std::wstring asidePath_;
    bool wasOpaque_ = false;
};

class CopyUp {
public:
    CopyUp(ConfigRef config,
           PathResolver& pathResolver,
           WhiteoutManager& whiteoutMgr,
           Cache& cache,
           LayerMountStats& stats);

    void SetEventEmitter(::LayerMount::abi::EventEmitter* events) noexcept { events_ = events; }

    // Bump the copy-up stat counter and emit LM_EVT_COPY_UP to the host
    // callback when one is installed.
    void RecordCopyUp(const std::wstring& relativePath);

    std::wstring GenerateWorkPath();

    // Deletes every #*.tmp file in the work directory. Call it only when no
    // copy-up is in flight.
    void CleanWorkDirectory();

    // Moves the file at workPath to finalUpperPath with one rename. workPath
    // must be on the upper's volume. An entry at finalUpperPath fails the
    // move with STATUS_OBJECT_NAME_COLLISION and stays as it was. A failed
    // move removes the file at workPath and its sidecar record, also when
    // the file is read-only.
    NTSTATUS CommitFromWorkDir(const std::wstring& workPath,
                               const std::wstring& finalUpperPath);

    // Copies a lower parent directory up first. Builds the file in the
    // work directory: data, security, user streams, copy-up record, and
    // last the attributes and times. Then one rename moves it to the upper
    // path. A lower file whose reparse point ClonesReparsePoint clones, such
    // as a file symbolic link, copies up as a clone of that reparse point.
    // Any other lower file reparse point copies up as a plain file with its
    // data. A failure to read the reparse tag fails the copy-up with that
    // error. If an entry appears at the upper path before that move, the
    // copy-up fails with STATUS_OBJECT_NAME_COLLISION and leaves that entry
    // as it was. A failure leaves no copy in the work directory.
    NTSTATUS CopyUpFile(const std::wstring& relativePath);

    // Builds a sparse shell of the lower file's size in the work directory,
    // with security, a copy-up record with the metacopy flag, and last the
    // attributes and times. Then one rename moves it to the upper path, as
    // in CopyUpFile. CompleteLazyCopyUp copies the data later.
    NTSTATUS CopyUpMetadataOnly(const std::wstring& relativePath);

    // Complete a metacopy by copying actual file data from the lower layer.
    // Keeps the shell's timestamps, so a set-times on the sparse shell
    // survives the fill. Fills a read-only shell too, and keeps the shell's
    // attributes, except that the sparse attribute comes off unless the
    // lower file is sparse. Clears the metacopy flag of the copy-up record
    // when done.
    NTSTATUS CompleteLazyCopyUp(const std::wstring& relativePath);

    // Copy a directory entry (not contents) from a lower layer to the upper layer.
    // The upper entry takes the lower entry's name, whatever the case of
    // relativePath. Returns success when the upper already has an entry at
    // the path. Otherwise builds the directory, or the link for a lower
    // junction or directory symbolic link, in the work directory and moves
    // it to the upper path once it is complete. Any other directory
    // reparse point copies up as a plain directory. A failure to read the
    // reparse tag fails the copy-up with that error. When an entry appears
    // at the upper path before that move, fails with
    // STATUS_OBJECT_NAME_COLLISION and leaves that entry as it was. A
    // failure leaves no copy in the work directory.
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
    // with STATUS_ACCESS_DENIED, and nothing moves. aside must be empty.
    NTSTATUS SetRenameDestinationAside(const std::wstring& newNorm,
                                       EntryKind destinationKind,
                                       RenameDestinationAside* aside);

    // Renames a directory whose old and new paths differ only in case.
    // Copies a lower directory up first, then renames the upper entry in
    // place to newCallerPath's case. sourceKind is Directory or Link. A
    // lower Link source goes to the new name as a link, with a copy-up
    // record and no opaque marker. That rename does not count as a copy-up
    // and emits no LM_EVT_COPY_UP.
    NTSTATUS RenameDirectoryCase(const CallerPath& oldCallerPath,
                                 const CallerPath& newCallerPath,
                                 EntryKind sourceKind);

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

    NTSTATUS CopyUpReparseCloneAndCount(const std::wstring& normalized, const CopyUpTarget& target);

    NTSTATUS StageFileInWorkDir(const std::wstring& sourcePath,
                                ScopedHandle& srcHandle,
                                DWORD srcAttrs,
                                const std::wstring& workPath);

    // Gives the staged file at workPath the encryption, extended attributes
    // and security descriptor of the file at sourcePath, in that order. Each
    // step opens workPath by path, so no handle to it may be open. On
    // failure the caller removes the staged file.
    NTSTATUS CopyStagedFileMetadata(const std::wstring& sourcePath,
                                    DWORD srcAttrs,
                                    const std::wstring& workPath);

    NTSTATUS FinishStagedFile(const std::wstring& sourcePath,
                              const std::wstring& workPath,
                              FileBasicInfoGuard& basicInfo);

    NTSTATUS RecordStagedFile(const std::wstring& workPath,
                              const LayerMountMetadata& metadata,
                              FileBasicInfoGuard& basicInfo);

    NTSTATUS CommitStagedFile(const std::wstring& normalized,
                              const std::wstring& workPath,
                              const std::wstring& upperPath);

    NTSTATUS StageMetacopyShellInWorkDir(const std::wstring& sourcePath,
                                         const WIN32_FILE_ATTRIBUTE_DATA& srcAttrs,
                                         const std::wstring& workPath);

    NTSTATUS FillMetacopyShell(ScopedHandle& srcHandle,
                               const std::wstring& upperPath);

    NTSTATUS FinishFilledShell(const std::wstring& upperPath,
                               LayerMountMetadata& metadata);

    // Creates the directory at stagedPath with the layout, streams,
    // security and copy-up record of the directory at sourcePath. A
    // failure can leave the directory at stagedPath for the caller to
    // remove.
    NTSTATUS BuildStagedDirectory(const std::wstring& sourcePath,
                                  const std::wstring& stagedPath);

    const LayerConfig& config_;
    PathResolver& pathResolver_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
    LayerMountStats& stats_;
    ::LayerMount::abi::EventEmitter* events_ = nullptr;

    std::atomic<uint64_t> workCounter_{0};

    // Lets one copy-up of a path run at a time. A caller that finds its path
    // in inFlightCopyUps_ waits on copyUpCV_ until the holder ends, then
    // checks again whether the upper still needs the work.
    class PathReservation;
    std::mutex copyUpMutex_;
    std::condition_variable copyUpCV_;
    std::unordered_set<std::wstring> inFlightCopyUps_;
};

}
