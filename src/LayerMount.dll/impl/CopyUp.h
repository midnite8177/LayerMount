#pragma once

#include "LayerMount.h"
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

enum class ReplaceExisting { No, Yes };

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
    NTSTATUS CopyUpDirectory(const std::wstring& relativePath);

    // Copies the lower tree and the old upper shadow to the new upper path,
    // marks it opaque, and whites out the old path. A junction or directory
    // symlink source, when the upper supports reparse points, is copied as
    // a link, without its upper shadow and without opacity.
    // ReplaceExisting::No fails with STATUS_OBJECT_NAME_COLLISION when the
    // destination exists in the merged view.
    NTSTATUS RenameLowerDirectory(const std::wstring& oldRelativePath,
                                  const std::wstring& newRelativePath,
                                  ReplaceExisting replace);

    // Moves the upper directory and carries its opaque marker.
    // ReplaceExisting::No fails with STATUS_OBJECT_NAME_COLLISION when the
    // destination exists in the merged view.
    NTSTATUS RenameUpperDirectory(const std::wstring& oldRelativePath,
                                  const std::wstring& newRelativePath,
                                  ReplaceExisting replace);

private:
    // A lower entry under a whiteout does not count.
    bool DestinationExistsInMerged(const std::wstring& normalizedPath) const;

    // Copies each old upper entry over newUpperPath. Skips the opaque marker
    // and deletes each whited-out name instead of copying its whiteout,
    // because the destination becomes opaque and those markers are redundant.
    NTSTATUS OverlayUpperShadow(const std::wstring& oldUpperPath,
                                const std::wstring& newUpperPath);

    // Ensure all ancestor directories exist in the upper layer, copying up as needed.
    NTSTATUS EnsureParentDirectories(const std::wstring& relativePath);

    // Recursively copy a directory tree from srcAbs to dstAbs preserving child
    // metadata that std::filesystem::copy drops: reparse points (symlinks /
    // junctions) stay as links, sparse files stay sparse, ADS ride along with
    // their base file.
    NTSTATUS CopyTreePreservingMetadata(const std::wstring& srcAbs,
                                         const std::wstring& dstAbs);

    bool CopySecurityDescriptor(const std::wstring& srcPath, const std::wstring& dstPath);

    bool CopyTimestamps(HANDLE srcHandle, HANDLE dstHandle);

    // Write the copy-up bookkeeping metadata to upperPath. On failure,
    // delete upperPath and return the failure status. Without this
    // metadata the upper file looks like a foreign creation to later
    // resolution, and a metacopy shell without its metacopy flag set
    // serves its zero-filled data as real content on the next read.
    NTSTATUS WriteCopyUpMetadataOrAbort(const std::wstring& upperPath,
                                        const LayerMountMetadata& metadata);

    // Mark dstHandle sparse so a metacopy placeholder allocates no data
    // blocks. On failure, close dstHandle, delete workPath, and return the
    // failure status. A non-sparse placeholder inflates the upper volume
    // and breaks the metacopy contract.
    NTSTATUS MarkPlaceholderSparseOrAbort(ScopedHandle& dstHandle,
                                          const std::wstring& workPath);

    // A Win32 call that removes one upper entry by path: DeleteFileW for
    // a file, RemoveDirectoryW for a directory.
    using RemoveUpperEntryFn = BOOL (WINAPI*)(LPCWSTR);

    // Copy a reparse-point source (symlink / junction) to the upper layer
    // as a link, write the copy-up metadata, invalidate the cache, and
    // record the copy-up. On a metadata failure, call removeUpperEntry
    // on the staged link and return the failure status.
    NTSTATUS CopyUpReparseEntry(const std::wstring& normalized,
                                const ResolvedPath& source,
                                RemoveUpperEntryFn removeUpperEntry);

    // Stage a full copy of the source at workPath. On failure after the
    // create, delete workPath and return the failure status.
    NTSTATUS StageFileInWorkDir(const std::wstring& sourcePath,
                                ScopedHandle& srcHandle,
                                DWORD srcAttrs,
                                const std::wstring& workPath);

    // Finish a committed upper file: copy the user alternate data
    // streams, write the copy-up metadata, and restore the source
    // timestamps and attributes through basicInfo. On failure, delete
    // upperPath and return the failure status.
    NTSTATUS FinishCommittedFile(const std::wstring& sourcePath,
                                 const std::wstring& upperPath,
                                 FileBasicInfoGuard& basicInfo);

    // Stage a metacopy shell at workPath. On failure after the create,
    // delete workPath and return the failure status.
    NTSTATUS StageMetacopyShellInWorkDir(const std::wstring& sourcePath,
                                         const WIN32_FILE_ATTRIBUTE_DATA& srcAttrs,
                                         const std::wstring& workPath);

    // Copy the origin data through srcHandle into the shell at upperPath,
    // clear the sparse attribute when the origin is not sparse, and close
    // both handles. On failure, return the failure status and leave the
    // shell in place with its metacopy flag set.
    NTSTATUS FillMetacopyShell(ScopedHandle& srcHandle,
                               const std::wstring& upperPath);

    // Copy the user alternate data streams from the origin to the filled
    // shell and clear the metacopy flag in metadata. On failure, return
    // the failure status and leave the metacopy flag set so the next
    // resolution retries the completion.
    NTSTATUS FinishFilledShell(const std::wstring& upperPath,
                               LayerMountMetadata& metadata);

    // Copy the security descriptor and write the copy-up metadata on the
    // upper directory. On failure, remove upperPath and return the
    // failure status.
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
