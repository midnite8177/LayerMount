#pragma once

#include "WindowsNtStatus.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <atomic>
#include <optional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <filesystem>

namespace LayerMount {

constexpr const wchar_t* kWhiteoutPrefix    = L".wh.";
constexpr const wchar_t* kOpaqueMarkerFile  = L".wh..wh..opq";
constexpr const wchar_t* kLayerMountADSStream  = L":overlay";
constexpr const wchar_t* kOpaqueADSStream   = L":overlay.opaque";

enum class LayerSource {
    None,
    Upper,
    Lower
};

struct LayerConfig {
    std::wstring upperPath;
    std::vector<std::wstring> lowerPaths;   // index 0 = highest priority lower
    std::wstring workDirPath;

    bool enableProcessTracking = false;
    std::wstring processRulesPath;          // Path to JSON rules file (empty = no rules)
    size_t accessLogCapacity = 10000;

    size_t pathCacheCapacity = 10000;

    // Host capabilities bitfield (LM_HOST_CAPABILITIES). Determines which
    // optimized paths the engine takes vs. which fallbacks it dispatches
    // to. Default 0 means "no capabilities" -- every fallback active.
    // Hosts that know the upper filesystem supports ADS / reparse /
    // sparse / NTFS ACLs should pass the appropriate bits to LayerMountCreate
    // so the engine takes the fast path.
    UINT32 hostCapabilities = 0;

    // Pure validation — no side effects. Returns false and sets error on failure.
    bool Validate(std::wstring& error) const;

    // Creates workDirPath if it doesn't exist. Call after Validate().
    bool Prepare(std::wstring& error);
};

// Keeps a reference to a LayerConfig. A temporary config would dangle, so
// the type refuses one.
class ConfigRef {
public:
    ConfigRef(const LayerConfig& config) noexcept : config_(&config) {}
    ConfigRef(const LayerConfig&& config) = delete;

    const LayerConfig& Get() const noexcept { return *config_; }

private:
    const LayerConfig* config_;
};

struct LayerMountMetadata {
    bool opaque = false;
    bool metacopy = false;
    std::wstring redirect;
    FILETIME copyUpTimestamp = {};
    std::wstring originLayer;
    bool hasStableIndexNumber = false;
    uint64_t stableIndexNumber = 0;
};

struct ResolvedPath {
    std::wstring absolutePath;
    LayerSource source = LayerSource::None;
    int lowerIndex = -1;        // which lower layer (0-based), -1 if upper or none
    bool isWhiteout = false;
    DWORD attributes = INVALID_FILE_ATTRIBUTES;

    bool Found() const {
        return !absolutePath.empty() && source != LayerSource::None;
    }
};

struct CreateResolution {
    // The overlay's hit, as ResolvePath returns it.
    ResolvedPath overlayHit;
    // True when the upper holds a whiteout for exactly this path.
    bool whiteoutAtPath = false;
    // The lower hit, as ResolveLowerPath returns it. An upper entry at this
    // path, a whiteout for exactly this path, or an opaque ancestor in the
    // upper does not hide it. A whiteout or a non-directory at an ancestor in
    // the upper does.
    ResolvedPath lower;
};

struct LayerMountStats {
    std::atomic<uint64_t> cacheHits{0};
    std::atomic<uint64_t> cacheMisses{0};
    std::atomic<uint64_t> copyUpCount{0};
    std::atomic<uint64_t> readCount{0};
    std::atomic<uint64_t> writeCount{0};
    std::atomic<uint64_t> activeHandles{0};
    std::atomic<uint64_t> bytesRead{0};
    std::atomic<uint64_t> bytesWritten{0};

    // Best-effort metadata updates at cleanup that the engine skipped after
    // a failed copy-up or a denied attribute set. A non-zero count means the
    // upper's metadata can differ from what the caller set before close.
    std::atomic<uint64_t> cleanupMetadataFailureCount{0};
};

struct FileContext {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::wstring actualPath;        // Physical filesystem path
    std::wstring workPath;          // Path in work directory during atomic ops
    std::wstring relativePath;      // Path within overlay namespace
    bool isDirectory = false;
    bool writable = false;
    bool isWhiteout = false;
    bool inWorkDir = false;         // True if file is being atomically created
    bool isMetacopyOnly = false;    // True if only metadata was copied up
    LARGE_INTEGER allocSize = {};
    DWORD ownerPid = 0;             // PID of process that opened this handle
    UINT32 grantedAccess = 0;
    UINT32 createOptions = 0;       // Original open flags; reopen paths must
                                    // preserve FILE_OPEN_REPARSE_POINT.
    bool handleNeedsReopen = false; // Rename retargets lazily on next use.
    std::wstring streamSuffix;      // Empty for main-stream handles. For ADS
                                    // handles holds the parsed suffix with
                                    // leading colon (`:secret` or
                                    // `:secret:$DATA`). `relativePath` stays
                                    // host-only so every path-keyed lookup
                                    // (cache, whiteout, tracker, resolver)
                                    // operates on the host; the suffix is
                                    // only consumed when re-deriving
                                    // `actualPath` or opening the underlying
                                    // NT handle.
};

class PathResolver;
class WhiteoutManager;
class MetadataStore;
class Cache;
class CopyUp;
namespace VHD { class VHDLayerManager; }
namespace VSS { class VSSManager; }
namespace LayerImage { class LayerImageManager; }

}

#include "ProcessTracker.h"
#include "SecurityPolicy.h"

#include "../abi/CapabilityGate.h"
#include "../abi/EventEmitter.h"

namespace LayerMount {

// Normalize a relative filesystem path: strip leading backslash, normalize
// separators to backslash, fold to lowercase for case-insensitive NTFS matching.
std::wstring NormalizePath(const std::wstring& path);

// Returns true if `normalized` is safe to combine with a layer root. Rejects
// empty input, drive/stream-qualified forms (any `:` character), and any `..`
// segment that would traverse out of the layer root when concatenated.
bool IsSafeRelativePath(const std::wstring& normalized);

// Returns true if `streamName` (the parsed stream name only, *without* the
// leading colon and without any `$TYPE` suffix) matches one of LayerMount's
// reserved internal streams. Reserved streams hold sidecar bookkeeping
// (metacopy / opaque markers) and must never be exposed to callers as
// creatable / openable / deletable names. Case-insensitive.
bool IsReservedStreamName(const std::wstring& streamName) noexcept;

// Parse a normalized relative path into <host>[:<stream>[:$DATA]] components.
// Returns true iff every rule holds:
//   - `normalized` is non-empty
//   - host portion (the substring before the first `:`) passes
//     `IsSafeRelativePath` (no embedded `:`, no `..`, no empty, no
//     drive-qualifier)
//   - if a stream is present: the stream name is non-empty, contains no `\`,
//     and is not a reserved name per `IsReservedStreamName`
//   - if a stream-type suffix is present: it is exactly `:$DATA`
//     (case-insensitive); other NTFS types (`$INDEX_ALLOCATION`, `$BITMAP`,
//     ...) are rejected
//   - there is no third `:` anywhere
// `outStreamSuffix` is the parsed canonical suffix with leading `:` (e.g.
// `:secret` or `:secret:$DATA`), or empty when `normalized` has no `:`.
// `outHostNorm` is the host substring (already normalized).
bool TryParseStreamPath(const std::wstring& normalized,
                        std::wstring& outHostNorm,
                        std::wstring& outStreamSuffix);

// Returns true for a path that the overlay never shows. That is the
// sidecar metadata subtree `.overlay` at the overlay root and anything
// beneath it, and any path with a segment that starts with `kWhiteoutPrefix`
// in any case, which covers whiteout markers, the opaque marker and anything
// beneath a marker-named directory. The sidecar store exists only at the
// root, so a `.overlay` below the root is an ordinary name. The `.overlay`
// compare is case-sensitive, so `normalized` must be the lowercased output
// of `NormalizePath`.
constexpr const wchar_t* kSidecarDirName = L".overlay";
bool IsReservedRelativePath(const std::wstring& normalized);

// Recursively create directories. Returns true on success or if already exists.
bool EnsureDirectoryExists(const std::wstring& path);

struct MergedEntry {
    WIN32_FIND_DATAW findData;
    LayerSource source;
};

// One directory's entries merged across the layers, keyed by lowercase
// filename. status is STATUS_SUCCESS, or the failed layer scan's status
// with entries empty.
struct MergedDirectory {
    NTSTATUS status;
    std::map<std::wstring, MergedEntry> entries;
};

struct InternalFileInfo {
    UINT32 FileAttributes;
    UINT32 ReparseTag;
    UINT64 AllocationSize;
    UINT64 FileSize;
    UINT64 CreationTime;
    UINT64 LastAccessTime;
    UINT64 LastWriteTime;
    UINT64 ChangeTime;
    UINT64 IndexNumber;
    UINT32 HardLinks;
    UINT32 EaSize;
};

// The mutations one set-info call carries. Each field keeps the public
// ABI's leave-unchanged sentinel: INVALID_FILE_ATTRIBUTES for the
// attributes, 0 for a time, UINT64_MAX for a size.
struct SetInfoRequest {
    UINT32 fileAttributes;
    UINT64 creationTime;
    UINT64 lastAccessTime;
    UINT64 lastWriteTime;
    // SetInfo ignores changeTime.
    UINT64 changeTime;
    UINT64 allocationSize;
    UINT64 fileSize;
};

inline UINT64 ComposeUInt64(DWORD high, DWORD low) {
    return (static_cast<UINT64>(high) << 32) | low;
}

constexpr UINT64 kAllocationGranularityBytes = 4096;

inline UINT64 AllocationSizeFor(UINT64 fileSize) {
    return (fileSize + kAllocationGranularityBytes - 1) & ~(kAllocationGranularityBytes - 1);
}

// Every producer of file info reports this value, so it is never below the
// file size.
inline UINT64 AllocationSizeFor(UINT64 fileSize, UINT64 realAllocation) {
    const UINT64 rounded = AllocationSizeFor(fileSize);
    return realAllocation > rounded ? realAllocation : rounded;
}

struct ResolvedSizes {
    UINT64 fileSize       = 0;
    UINT64 allocationSize = 0;
};

// Names carry NTFS's native form (e.g. ":mystream:$DATA").
struct InternalStreamInfo {
    std::wstring name;
    UINT64 streamSize;
    UINT64 allocationSize;
};

class LayerMount {
public:
    // Throws std::system_error carrying ERROR_INVALID_DATA when
    // config.enableProcessTracking is set and config.processRulesPath
    // names a rules file that is missing or does not parse.
    explicit LayerMount(LayerConfig config);
    ~LayerMount();

    LayerMount(const LayerMount&) = delete;
    LayerMount& operator=(const LayerMount&) = delete;

    const LayerMountStats& Stats() const { return stats_; }

    // Both sizes are zero for a directory, a whiteout, a path that does not
    // exist, and a path the engine cannot stat.
    ResolvedSizes SizesOf(const ResolvedPath& resolved) const;

    // Ensure a file is in the upper layer (copy-up if needed).
    // Updates FileContext on success.
    NTSTATUS EnsureInUpperLayer(const std::wstring& relativePath, FileContext* ctx);

    // Path-based copy-up. Triggers `CopyUp::CopyUpDirectory` /
    // `CopyUp::CopyUpFile` based on whether the entry is a directory.
    // Returns STATUS_OBJECT_NAME_NOT_FOUND if the entry doesn't exist
    // in any layer; STATUS_SUCCESS if it's already in upper or copy-up
    // succeeds.
    NTSTATUS EnsureInUpperLayer(const std::wstring& relativePath);

    // Volume-level disk free / total space taken from the upper layer's
    // hosting filesystem.
    NTSTATUS GetVolumeInfo(UINT64* outTotalSize, UINT64* outFreeSize) const;

    static NTSTATUS FillFileInfo(const std::wstring& path, InternalFileInfo* fileInfo);

    static NTSTATUS FillFileInfoFromHandle(HANDLE handle,
        InternalFileInfo* fileInfo,
        const std::wstring* pathHint = nullptr);

    // Merges the directory's entries across the layers. A layer that holds
    // the directory but cannot list it gives the status of that scan's
    // Win32 error and no entries, whether it is the upper or a lower. An
    // unsafe or reserved path gives STATUS_SUCCESS and no entries.
    MergedDirectory MergeDirectoryEntries(const std::wstring& dirRelativePath) const;

    // Open and Create take a callerPid for the ProcessTracker check and record it
    // as ctx->ownerPid; pass 0 to skip tracking for that call. A
    // primitive that takes a FileContext checks the tracker against
    // ctx->ownerPid, the process that opened the handle.
    //
    // Method names are deliberately Open/Create/Close rather than
    // OpenFile/CreateFile/CloseFile: <windows.h> defines `CreateFile` and
    // `OpenFile` as macros (UNICODE-aware redirectors), so a method by
    // those names gets silently renamed to CreateFileW/OpenFileW after
    // preprocessing and shadows the Win32 functions inside the class body.

    // Open an existing file or directory in the overlay. Allocates a
    // FileContext, opens the underlying NT handle (copying up first when a
    // write-bearing access is requested against a lower-only entry), and
    // fills outInfo. Returns the new context via outCtx (caller takes
    // ownership).
    //
    // A metacopy shell fills before its handle opens when grantedAccess
    // asks for data: read, write, append, or execute. An open for
    // attributes, security, or delete keeps the shell sparse. A failed
    // fill returns its status, leaves *outCtx null, and opens no handle.
    NTSTATUS Open(const std::wstring& relativePath,
                  UINT32 grantedAccess,
                  UINT32 createOptions,
                  DWORD callerPid,
                  std::unique_ptr<FileContext>* outCtx,
                  InternalFileInfo* outInfo);

    struct CreateRequest {
        std::wstring relativePath;
        UINT32 createOptions;
        UINT32 grantedAccess;
        UINT32 fileAttributes;
        PSECURITY_DESCRIPTOR securityDescriptor;
        UINT64 allocationSize;
        DWORD callerPid;
    };

    // Create a new file or directory in the upper layer. CreateOptions
    // FILE_DIRECTORY_FILE selects directory creation. Applies the
    // self-relative security descriptor that SecurityPolicy picks, and
    // pre-allocates allocationSize bytes when non-zero. Marks a new
    // directory as opaque when a whiteout at the path or an opaque ancestor
    // hides a lower directory of the same name. Returns
    // STATUS_OBJECT_NAME_COLLISION when the overlay already holds a path
    // without a stream suffix, and for a stream create when the host file
    // that the overlay holds already has that stream. A stream that the
    // origin of a metacopy shell has counts, and the shell does not fill.
    NTSTATUS Create(const CreateRequest& request,
                    std::unique_ptr<FileContext>* outCtx,
                    InternalFileInfo* outInfo);

    // Close the NT handle inside ctx and decrement the active-handles
    // stat. Does NOT delete ctx; the caller owns the FileContext storage.
    void Close(FileContext* ctx);

    // Close the NT handle inside ctx and keep ctx alive. The active-handles
    // stat does not change. Each operation that calls EnsureHandleReady
    // reopens the file by its path with the granted access minus DELETE;
    // a granted mask with FILE_WRITE_DATA or FILE_APPEND_DATA reopens with
    // FILE_READ_DATA as well. A second call on a ctx with no handle does
    // nothing and succeeds.
    NTSTATUS Cleanup(FileContext* ctx);

    // Read up to `length` bytes from the open file at the given absolute
    // offset. Returns STATUS_END_OF_FILE on read past EOF (with
    // *bytesTransferred = 0).
    // The process tracker checks the read against ctx->ownerPid, the
    // process that opened the handle, not against the caller. A paging
    // read arrives from the system process and passes the opener's rules.
    NTSTATUS Read(FileContext* ctx,
                  void* buffer,
                  UINT64 offset,
                  ULONG length,
                  PULONG bytesTransferred);

    // Write `length` bytes to the open file. Triggers copy-up if the file
    // is still in a lower layer. writeToEnd=TRUE appends regardless of
    // offset; constrainedIo=TRUE truncates the write to whatever fits in
    // the current EOF (no extension). Fills outInfo with the post-write
    // file metadata when non-null. The process tracker checks the write
    // against ctx->ownerPid, as for Read.
    NTSTATUS Write(FileContext* ctx,
                   const void* buffer,
                   UINT64 offset,
                   ULONG length,
                   BOOLEAN writeToEnd,
                   BOOLEAN constrainedIo,
                   PULONG bytesTransferred,
                   InternalFileInfo* outInfo);

    // CREATE_ALWAYS-style truncate of an already-open file. Copies the
    // entry up to the upper layer if needed, deletes non-overlay ADS
    // streams (user content under CREATE_ALWAYS is destroyed but our
    // :overlay* bookkeeping survives), truncates to zero, applies
    // allocation, and either replaces or ORs the file attributes. The
    // process tracker checks the overwrite against ctx->ownerPid, as for
    // Read.
    NTSTATUS Overwrite(FileContext* ctx,
                       UINT32 fileAttributes,
                       BOOLEAN replaceAttributes,
                       UINT64 allocationSize,
                       InternalFileInfo* outInfo);

    // Flush buffered writes for the open file. The process tracker checks
    // the flush against ctx->ownerPid with the read rule, as for Read.
    NTSTATUS Flush(FileContext* ctx,
                   InternalFileInfo* outInfo);

    NTSTATUS EnsureHandleReady(FileContext* ctx);

    // Apply the attribute, time, and size mutations in `request` to an
    // open file; a field at its sentinel stays unchanged. Triggers copy-up
    // if the file is still in a lower layer. Fills outInfo with the
    // post-mutation metadata when non-null.
    NTSTATUS SetInfo(FileContext* ctx,
                     const SetInfoRequest& request,
                     InternalFileInfo* outInfo);

    // Checks read-only and directory-not-empty and changes nothing. Returns
    // a failed directory scan's status, and STATUS_OBJECT_NAME_NOT_FOUND
    // when neither layer has the entry.
    NTSTATUS CanDelete(const std::wstring& relativePath, DWORD callerPid);
    NTSTATUS CanDelete(FileContext* ctx);

    // Calls CanDelete first. Removes the entry from the upper, and writes a
    // whiteout when a lower holds it.
    NTSTATUS Delete(const std::wstring& relativePath, DWORD callerPid);
    NTSTATUS Delete(FileContext* ctx);

    // Delete the alternate data stream that `ctx` opened. The host file
    // and its other streams stay.
    NTSTATUS DeleteStreamOnContext(FileContext* ctx);

    // Path-based rename: moves the entry within the overlay namespace.
    // Source is copied up if it lives only in lower. Rename drops a
    // whiteout at the old path when a lower still holds it. A rename to
    // the identical name succeeds and changes nothing.
    // replaceIfExists FALSE fails with STATUS_OBJECT_NAME_COLLISION when
    // the destination already exists. Otherwise a directory rename to a
    // path inside the directory's own tree fails with
    // STATUS_INVALID_PARAMETER, even when the destination is a non-empty
    // directory. A directory rename with replaceIfExists TRUE onto a
    // directory fails with STATUS_DIRECTORY_NOT_EMPTY when the merged view
    // shows a child of the destination. When it shows none, Rename moves
    // the upper destination into the work directory before the move, puts
    // it back when the move fails, and removes it when the move succeeds.
    // The FileContext overload makes the destination checks before it
    // closes ctx->handle, so a refused rename leaves the handle open.
    NTSTATUS Rename(const std::wstring& oldRelativePath,
                    const std::wstring& newRelativePath,
                    BOOLEAN replaceIfExists,
                    DWORD callerPid);
    NTSTATUS Rename(FileContext* ctx,
                    const std::wstring& newRelativePath,
                    BOOLEAN replaceIfExists,
                    DWORD callerPid);

    // Bookkeeping-only path update for an open FileContext after some
    // OTHER actor (typically a path-based Rename via a sibling handle)
    // has moved the underlying file. Updates relativePath + actualPath
    // and marks the NT handle for lazy reopen at next use; performs no
    // I/O. Hosts call this from their rename-fanout when a path-based
    // rename affects a concurrent open handle on the source path.
    // Returns STATUS_INVALID_PARAMETER if the new path is empty, a drive
    // or stream-qualified form, contains `..` traversal, or is a reserved
    // path (see `IsReservedRelativePath`). The open handle is left unchanged on
    // rejection so the caller can surface the error without losing state.
    NTSTATUS UpdateContextPath(FileContext* ctx,
                               const std::wstring& newRelativePath);

    // Path-based security accessor. Two-call buffer pattern:
    //   sd == nullptr or sdBytes == 0 -> writes needed bytes to
    //                                     *requiredBytes, returns S_OK
    //   sdBytes >= needed             -> fills sd, writes needed to
    //                                     *requiredBytes, returns S_OK
    //   sdBytes < needed              -> writes needed, returns
    //                                     STATUS_BUFFER_OVERFLOW
    // securityInformation names the descriptor sections the caller
    // wants: owner, group, DACL, or SACL. GetSecurity drops SACL
    // unless the FS process holds SE_SECURITY_NAME, even when the
    // caller asks for it.
    // A requested value of 0 is not an error. It writes
    // *requiredBytes = 0 and returns STATUS_SUCCESS with an empty
    // descriptor.
    // outAttributes (optional) receives the file's Win32 attributes.
    NTSTATUS GetSecurity(const std::wstring& relativePath,
                         UINT32 securityInformation,
                         PUINT32 outAttributes,
                         PSECURITY_DESCRIPTOR sd,
                         SIZE_T sdBytes,
                         SIZE_T* requiredBytes);

    // Path-based security mutator. Triggers copy-up to the upper layer if
    // the entry lives only in lower. Applies via path-based
    // ::SetFileSecurityW.
    NTSTATUS SetSecurity(const std::wstring& relativePath,
                         UINT32 securityInformation,
                         PSECURITY_DESCRIPTOR sd,
                         DWORD callerPid);

    // Reparse-point primitives. Each opens the underlying NT handle with
    // FILE_FLAG_OPEN_REPARSE_POINT so we see the link itself rather than
    // the target.
    //
    // GetReparsePoint: STATUS_NOT_A_REPARSE_POINT if the entry doesn't
    // carry FILE_ATTRIBUTE_REPARSE_POINT. Two-call pattern via
    // *requiredBytes; STATUS_BUFFER_OVERFLOW when the caller's buffer is
    // smaller than the reparse data.
    //
    // SetReparsePoint / DeleteReparsePoint: trigger copy-up if the entry
    // is in lower-only, then DeviceIoControl on a freshly opened handle.
    NTSTATUS GetReparsePoint(const std::wstring& relativePath,
                             PVOID buffer,
                             SIZE_T bufferBytes,
                             SIZE_T* requiredBytes);

    NTSTATUS SetReparsePoint(const std::wstring& relativePath,
                             const void* buffer,
                             SIZE_T bufferBytes,
                             DWORD callerPid);

    NTSTATUS DeleteReparsePoint(const std::wstring& relativePath,
                                const void* buffer,
                                SIZE_T bufferBytes,
                                DWORD callerPid);

    // Enumerate the named data streams of the file at `relativePath`.
    // Resolves the path through the overlay; reads streams from the
    // resolved physical layer via ::FindFirstStreamW. Filters the main
    // unnamed stream (`::$DATA`) — that's the file's own content, not
    // a separate "stream" in the ADS sense — and LayerMount's reserved
    // metadata streams (`:overlay:$DATA`, `:overlay.opaque:$DATA`).
    // Returns STATUS_OBJECT_NAME_NOT_FOUND when the file is absent in
    // every layer. Returns STATUS_SUCCESS with an empty `out` when the
    // file exists but carries no user-visible streams.
    NTSTATUS EnumerateStreams(const std::wstring& relativePath,
                              std::vector<InternalStreamInfo>& out);

    PathResolver& Resolver() { return *pathResolver_; }
    WhiteoutManager& Whiteouts() { return *whiteoutMgr_; }
    CopyUp& CopyUpEngine() { return *copyUp_; }
    Cache& PathCache() { return *cache_; }

    // Returns a pinned snapshot of the current process tracker, or
    // nullptr if tracking is disabled. Callers must hold the returned
    // shared_ptr for the duration of their use so a concurrent
    // SetProcessTrackerEnabled(false) cannot destroy the tracker under
    // them. Prefer the idiom:
    //   if (auto t = Tracker(); t && pid != 0) {
    //       if (!t->CheckAccess(...)) return STATUS_ACCESS_DENIED;
    //   }
    std::shared_ptr<ProcessTracker> Tracker() const {
        std::shared_lock lock(processTrackerMutex_);
        return processTracker_;
    }

    const ::LayerMount::abi::CapabilityGate& Capabilities() const noexcept {
        return capabilities_;
    }

    ::LayerMount::abi::EventEmitter& Events() noexcept { return events_; }
    const ::LayerMount::abi::EventEmitter& Events() const noexcept { return events_; }

    // Thread-safe to call concurrently; the returned reference is stable
    // for the overlay's lifetime.
    VHD::VHDLayerManager& Vhd();

    // VSSManager's methods require an active ComScope on the caller's
    // thread. Thread-safe.
    VSS::VSSManager& Vss();

    LayerImage::LayerImageManager& Images();

    // TRUE constructs the tracker when there is none, with the overlay's
    // accessLogCapacity and processRulesPath; FALSE tears it down.
    // Returns HRESULT_FROM_WIN32(ERROR_INVALID_DATA) when the rules file
    // does not load, else S_OK on state change or no-op. Safe against
    // concurrent Tracker() callers. Each one sees either the old tracker,
    // and holds the last reference until it releases it, or the new state.
    HRESULT SetProcessTrackerEnabled(bool enabled);

private:
    std::shared_ptr<ProcessTracker> TryMakeProcessTracker();

    NTSTATUS RenameFileInUpper(const std::wstring& oldRelativePath,
                               const std::wstring& newRelativePath,
                               BOOLEAN replaceIfExists,
                               bool destHadWhiteout);

    NTSTATUS OpenRoot(UINT32 grantedAccess,
                      UINT32 createOptions,
                      DWORD callerPid,
                      std::unique_ptr<FileContext>* outCtx,
                      InternalFileInfo* outInfo);

    // What Create resolved for a new entry, and the caller's settings for it.
    struct UpperCreate {
        std::wstring normalized;
        std::wstring hostNorm;
        std::wstring streamSuffix;
        std::wstring upperPath;
        bool lowerIsDirectory = false;
        bool lowerIsVisible = false;
        UINT32 grantedAccess = 0;
        UINT32 fileAttributes = 0;
        PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
        UINT64 allocationSize = 0;
    };

    // Copies the caller's settings from request into create, whose path
    // fields Create has resolved, and returns the context of the new entry.
    std::unique_ptr<FileContext> BuildCreate(const CreateRequest& request,
                                             UpperCreate* create) const;

    // The checks Create makes before it writes anything, for the parsed
    // path in create. Returns STATUS_ACCESS_DENIED for a reserved path or a
    // process tracker denial, STATUS_FILE_IS_A_DIRECTORY for a directory
    // create with a stream suffix and for a stream create whose host is a
    // directory in the upper or in the overlay, and
    // STATUS_OBJECT_NAME_COLLISION for a collision as Create describes it.
    // A lower directory that a whiteout or an opaque ancestor hides is not
    // a directory host. Resolves the path into *resolution once the
    // reserved-path, tracker and directory-stream checks pass.
    NTSTATUS CheckCreatePreconditions(const CreateRequest& request,
                                      const UpperCreate& create,
                                      CreateResolution* resolution) const;

    // The directory half of Create. Makes the directory in the upper,
    // marks it opaque when lowerIsDirectory, applies the caller's
    // security descriptor, and opens ctx->handle. An upper directory that
    // already exists fails the create. A failure after CreateDirectoryW
    // made the directory removes it again.
    NTSTATUS CreateDirectoryInUpper(const UpperCreate& create, FileContext* ctx);

    // The file half of Create. Creates the file, or the stream named by
    // streamSuffix on its host file, in the upper and opens ctx->handle.
    // The security descriptor and allocationSize apply to a host file,
    // never to a stream. A failure after CreateFileW made the entry
    // deletes it again.
    NTSTATUS CreateFileInUpper(const UpperCreate& create, FileContext* ctx);

    // Makes the host file of a stream create a full file in the upper: a
    // host that the overlay shows from a lower copies up, and a
    // metacopy shell fills. A host that a whiteout or an opaque ancestor
    // hides stays absent, and the stream create then makes an empty host.
    // Runs before the stream create, so a copy-up or a fill cannot bring
    // the lower's streams in over the new one.
    NTSTATUS PrepareStreamHost(const UpperCreate& create, FileContext* ctx);

    // Copies a lower file or directory up for a write-capable open and
    // chooses between a full copy and a metacopy shell. Sets
    // ctx->isMetacopyOnly when it stages a shell.
    NTSTATUS CopyUpForWriteOpen(const std::wstring& hostNorm,
                                const ResolvedPath& resolved,
                                FileContext* ctx);

    // Fill a metacopy shell from its recorded origin. Returns the fill's
    // status on failure and clears ctx->isMetacopyOnly on success.
    NTSTATUS FillShell(const std::wstring& hostNorm, FileContext* ctx);

    // Fills a shell that a handle without data access left sparse, then
    // reopens the handle. The fill sets the origin's timestamps on the
    // upper file as its last step.
    NTSTATUS EnsureMetacopyMaterialized(FileContext* ctx);

    // Merges the directory's entries. Returns a failed scan's status,
    // STATUS_DIRECTORY_NOT_EMPTY when any entry is visible, and
    // STATUS_SUCCESS otherwise.
    NTSTATUS DirectoryEmptinessStatus(const std::wstring& dirNorm) const;

    // The source and destination paths of a rename, in NormalizePath form.
    struct RenamePaths {
        const std::wstring& oldNorm;
        const std::wstring& newNorm;
    };

    // The checks of a rename that need no lookup of the source or the
    // destination: stream names, reserved paths and the access tracker.
    NTSTATUS CheckRenameRequest(const RenamePaths& paths, DWORD callerPid);

    // The merged-view checks of a rename destination, in the order that
    // overlayfs uses. Returns STATUS_OBJECT_NAME_COLLISION when
    // replaceIfExists is FALSE and the destination exists, then
    // STATUS_INVALID_PARAMETER for a directory whose destination lies
    // inside its own tree, then DirectoryEmptinessStatus for a directory
    // onto a directory. Changes nothing.
    NTSTATUS CheckRenameDestination(const RenamePaths& paths,
                                    bool isDirectory,
                                    BOOLEAN replaceIfExists) const;

    LayerConfig config_;
    ::LayerMount::abi::CapabilityGate capabilities_;
    SecurityPolicy                    securityPolicy_;
    ::LayerMount::abi::EventEmitter   events_;
    std::unique_ptr<Cache> cache_;
    std::unique_ptr<WhiteoutManager> whiteoutMgr_;
    std::unique_ptr<PathResolver> pathResolver_;
    LayerMountStats stats_;
    std::unique_ptr<CopyUp> copyUp_;
    std::shared_ptr<ProcessTracker> processTracker_;

    // Lazy VHD/VSS/LayerImage subsystems. Each subsystem's mutex guards
    // first-use construction; once the unique_ptr is populated it stays
    // valid for the overlay's lifetime.
    std::mutex                                    vhdMutex_;
    std::unique_ptr<VHD::VHDLayerManager>         vhd_;
    std::mutex                                    vssMutex_;
    std::unique_ptr<VSS::VSSManager>              vss_;
    std::mutex                                    imagesMutex_;
    std::unique_ptr<LayerImage::LayerImageManager> images_;

    // Guards reads + toggles of processTracker_. Readers acquire
    // shared; SetProcessTrackerEnabled acquires exclusive. Readers copy
    // the shared_ptr out under the shared lock and release before use,
    // so the tracker stays alive for the caller even if a concurrent
    // toggle resets the engine's reference.
    mutable std::shared_mutex                     processTrackerMutex_;
};

}
