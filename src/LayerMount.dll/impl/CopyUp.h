#pragma once

#include "LayerMount.h"
#include "LayerPath.h"
#include "ScopedHandle.h"
#include "../abi/EventEmitter.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
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

// The result of CopyUp::CopyUpMetadataOnly and CopyUp::CopyUpFileOrShell.
struct FileCopyUpResult {
    NTSTATUS status;
    // True when CopyUpMetadataOnly left a metacopy shell in the upper, and
    // when it found an entry already at the path in the upper, whatever
    // that entry holds. False after a copy of the data, and when the upper
    // reaches the entry through a link.
    bool stagedShell;
};

// The shellOnlyAboveBytes of CopyUp::CopyUpFileOrShell that lets a file of
// any size get a shell.
inline constexpr std::optional<LONGLONG> kShellAtAnySize = std::nullopt;

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
    // as it was. A failure leaves no copy in the work directory. When the
    // upper reaches the entry through a link, such as a lower link above it
    // that EnsureUpperParent copies up, copies nothing and returns
    // STATUS_SUCCESS.
    NTSTATUS CopyUpFile(const std::wstring& relativePath);

    // Builds a sparse shell of the lower file's size in the work directory,
    // with security, a copy-up record with the metacopy flag, and last the
    // attributes and times. Then one rename moves it to the upper path, as
    // in CopyUpFile. CompleteLazyCopyUp copies the data later. A shell
    // never carries the lower file's user streams, so a lower file with a
    // stream that IsUserAlternateStream accepts fails with
    // STATUS_INVALID_PARAMETER and writes nothing. Returns the error of a
    // stream list that cannot be read. When the upper reaches the entry
    // through a link, copies nothing and returns STATUS_SUCCESS with
    // stagedShell false.
    FileCopyUpResult CopyUpMetadataOnly(const std::wstring& relativePath);

    // Copies the lower file at relativePath up as a metacopy shell or with
    // its data. The data copies in full without sparse-file support on the
    // upper, for a reparse point that a copy-up clones, such as a file
    // symbolic link, for a file with a user alternate data stream, and for
    // a file no larger than shellOnlyAboveBytes or whose size cannot be
    // read. Fails with STATUS_OBJECT_NAME_NOT_FOUND when neither a lower
    // nor the upper holds relativePath. Returns the error of a reparse tag
    // or a stream list that cannot be read. When the upper reaches the
    // entry through a link, copies nothing and returns STATUS_SUCCESS with
    // stagedShell false.
    FileCopyUpResult CopyUpFileOrShell(const std::wstring& relativePath,
                                       std::optional<LONGLONG> shellOnlyAboveBytes);

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
    // the path. Also returns success and copies nothing when the copy-up of
    // the parent brings a lower link up and the upper then reaches the
    // entry through that link. Otherwise builds the directory, or the link
    // for a lower junction or directory symbolic link, in the work directory
    // and moves it to the upper path once it is complete. Any other directory
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
    // is a file, and with ENOENT when the overlay shows no parent. A lower
    // link on the parent's path copies up as a link, so the upper parent is
    // then in the link target.
    NTSTATUS EnsureUpperParent(const std::wstring& normalizedPath);

    // When FindLinkAbove finds a lower link above normalizedPath, copies
    // that link up as a link, with its ancestors as EnsureUpperParent does.
    // A write at normalizedPath then goes through the upper link into the
    // link target, as a write through a lower symlink does in overlayfs. For
    // any other path, including one with a component that FindLinkAbove
    // cannot read, returns STATUS_SUCCESS and writes nothing.
    NTSTATUS CopyUpLowerLinkAbove(const std::wstring& normalizedPath);

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
    // parent exist, and sets *target to that lower entry and to an upper
    // path named after it. Returns STATUS_OBJECT_NAME_NOT_FOUND when no
    // visible lower holds normalized. When the upper then reaches the entry
    // through a link on the parent's path, because the upper held the link
    // or EnsureUpperParent copied a lower link up, the entry is already in
    // the link target. Then returns STATUS_SUCCESS and leaves *target
    // empty, and the caller copies nothing.
    NTSTATUS PrepareCopyUpTarget(const std::wstring& normalized,
                                 std::optional<CopyUpTarget>* target);

    // Returns STATUS_INVALID_PARAMETER when the first visible lower that
    // holds normalized has a stream that IsUserAlternateStream accepts, and
    // the error of a stream list that cannot be read. Returns
    // STATUS_SUCCESS otherwise, also when no lower holds normalized.
    NTSTATUS LowerUserStreamStatus(const std::wstring& normalized);

    // Builds a metacopy shell of the file at sourcePath at workPath, with
    // its security and a copy-up record with the metacopy flag, and last
    // the attributes and times. A failure removes the staged file.
    NTSTATUS BuildMetacopyShell(const std::wstring& sourcePath, const std::wstring& workPath);

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
