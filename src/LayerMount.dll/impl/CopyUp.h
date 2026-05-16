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

// RAII wrapper for Win32 HANDLEs
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
    CopyUp(const LayerConfig& config,
           PathResolver& pathResolver,
           WhiteoutManager& whiteoutMgr,
           Cache& cache,
           LayerMountStats& stats);

    // Host capability gate + event emitter, both wired post-construction
    // by LayerMount so the existing 5-arg test ctors still compile. When
    // unset, capabilities_ defaults to "all bits on" -- engine takes the
    // optimized path; events_ stays nullptr and Emit is a no-op.
    void SetCapabilityGate(::LayerMount::abi::CapabilityGate gate) noexcept { capabilities_ = gate; }
    void SetEventEmitter(::LayerMount::abi::EventEmitter* events) noexcept { events_ = events; }

    // Bump the copy-up stat counter and emit LM_EVT_COPY_UP to the host
    // callback (if installed). Called at every successful commit point
    // of CopyUpFile / CopyUpDirectory / CopyUpMetadataOnly /
    // CompleteLazyCopyUp so the counter and the event stay in lockstep.
    void RecordCopyUp(const std::wstring& relativePath);

    // --- Work directory management ---

    // Generate a unique temp path in the work directory.
    std::wstring GenerateWorkPath();

    // Purge stale temp files from the work directory. Called on startup.
    void CleanWorkDirectory();

    // Atomically move a file from the work directory to the upper layer.
    NTSTATUS CommitFromWorkDir(const std::wstring& workPath,
                               const std::wstring& finalUpperPath);

    // --- Full copy-up (3.2) ---

    // Copy a file from a lower layer to the upper layer atomically.
    // Creates parent directories as needed. Preserves security, timestamps, data.
    NTSTATUS CopyUpFile(const std::wstring& relativePath);

    // --- Lazy/metadata-only copy-up (3.3) ---

    // Copy only metadata (security, timestamps) as a sparse file. Data copied on demand.
    NTSTATUS CopyUpMetadataOnly(const std::wstring& relativePath);

    // Complete a lazy copy-up by copying actual file data from the lower layer.
    // Clears the metacopy ADS flag when done.
    NTSTATUS CompleteLazyCopyUp(const std::wstring& relativePath);

    // --- Directory copy-up (3.4) ---

    // Copy a directory entry (not contents) from a lower layer to the upper layer.
    NTSTATUS CopyUpDirectory(const std::wstring& relativePath);

    // --- Directory rename redirect (3.5) ---

    // Whether SE_SECURITY_NAME was successfully enabled on the FS process
    // token. When false, SACL reads/writes would fail with
    // ERROR_PRIVILEGE_NOT_HELD and must be skipped. Returning false means
    // audit ACEs will not round-trip through the overlay for this process.
    static bool IsSecurityPrivAvailable();

    // Handle directory rename across layers.
    // From lower: recursive copy + opaque + whiteout at old path.
    // Within upper: MoveFileExW + transfer opaque marker.
    //
    // replaceIfExists mirrors the Win32 ReplaceIfExists/MOVEFILE_REPLACE_EXISTING
    // semantics: when false, a rename whose destination already exists in
    // the merged view must fail with STATUS_OBJECT_NAME_COLLISION instead of
    // silently merging into (lower-source) or overwriting (upper-source) it.
    // Default is false to match the historical behaviour of NTAPI Win32
    // MoveFileW (no replace).
    NTSTATUS HandleDirectoryRename(const std::wstring& oldRelativePath,
                                   const std::wstring& newRelativePath,
                                   bool sourceIsInLower,
                                   bool replaceIfExists = false);

private:
    // Ensure all ancestor directories exist in the upper layer, copying up as needed.
    NTSTATUS EnsureParentDirectories(const std::wstring& relativePath);

    // Recursively copy a directory tree from srcAbs to dstAbs preserving child
    // metadata that std::filesystem::copy drops: reparse points (symlinks /
    // junctions) stay as links, sparse files stay sparse, ADS ride along with
    // their base file. Used by HandleDirectoryRename for lower→upper copies
    // of non-trivial trees. Entries already present at dstAbs are overwritten
    // (matches copy_options::overwrite_existing semantics) so this can also be
    // used to overlay an upper-shadow tree on top of a lower copy.
    NTSTATUS CopyTreePreservingMetadata(const std::wstring& srcAbs,
                                         const std::wstring& dstAbs);

    // Copy security descriptor from one path to another.
    bool CopySecurityDescriptor(const std::wstring& srcPath, const std::wstring& dstPath);

    // Copy timestamps from one file/dir handle to another.
    bool CopyTimestamps(HANDLE srcHandle, HANDLE dstHandle);

    // Copy file data in chunks.
    NTSTATUS CopyFileData(HANDLE srcHandle, HANDLE dstHandle);

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

    static constexpr DWORD kCopyBufferSize = 64 * 1024; // 64KB
};

} // namespace LayerMount
