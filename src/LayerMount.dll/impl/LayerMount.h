#pragma once

// NTSTATUS constants (STATUS_SUCCESS, STATUS_OBJECT_NAME_*, etc.).
// WIN32_NO_STATUS stops <windows.h> from importing the ~16 basic
// STATUS_* macros so <ntstatus.h> can provide the full set. A handful
// of newer codes (STATUS_ASSERTION_FAILURE, STATUS_ENCLAVE_VIOLATION,
// STATUS_INTERRUPTED, STATUS_THREAD_NOT_RUNNING, STATUS_ALREADY_REGISTERED,
// STATUS_SXS_EARLY_DEACTIVATION, STATUS_SXS_INVALID_DEACTIVATION) are
// still declared unconditionally by winnt.h, which collides with
// ntstatus.h. Suppress C4005 around the include to keep /WX clean.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#pragma warning(push)
#pragma warning(disable: 4005) // macro redefinition (STATUS_* known conflicts)
#include <ntstatus.h>
#pragma warning(pop)
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

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr const wchar_t* kWhiteoutPrefix    = L".wh.";
constexpr const wchar_t* kOpaqueMarkerFile  = L".wh..wh..opq";
constexpr const wchar_t* kLayerMountADSStream  = L":overlay";
constexpr const wchar_t* kOpaqueADSStream   = L":overlay.opaque";

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

enum class LayerSource {
    None,
    Upper,
    Lower
};

// ---------------------------------------------------------------------------
// Structs
// ---------------------------------------------------------------------------

struct LayerConfig {
    std::wstring upperPath;
    std::vector<std::wstring> lowerPaths;   // index 0 = highest priority lower
    std::wstring workDirPath;

    // Process tracking
    bool enableProcessTracking = false;
    std::wstring processRulesPath;          // Path to JSON rules file (empty = no rules)
    size_t accessLogCapacity = 10000;       // Circular buffer size

    // Path-resolver cache capacity (LRU bound). 0 means "use the engine
    // default" (keep behavior of older callers that did not set this).
    // Hosts with very large lower trees can raise this to avoid eviction
    // thrash during cold-start scans. The previous code path discarded
    // LM_CONFIG::pathCacheCapacity entirely so the value was always the
    // hard-coded engine default.
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

struct LayerMountStats {
    std::atomic<uint64_t> cacheHits{0};
    std::atomic<uint64_t> cacheMisses{0};
    std::atomic<uint64_t> copyUpCount{0};
    std::atomic<uint64_t> readCount{0};
    std::atomic<uint64_t> writeCount{0};
    std::atomic<uint64_t> activeHandles{0};
    std::atomic<uint64_t> bytesRead{0};
    std::atomic<uint64_t> bytesWritten{0};

    // Cleanup-time best-effort metadata updates that silently skipped due
    // to a failed copy-up or a denied attribute set. These can't be
    // surfaced through the void-returning SCleanup callback, but a non-zero
    // count is a signal that upper-layer metadata may have drifted from
    // what the caller staged on close.
    std::atomic<uint64_t> cleanupMetadataFailureCount{0};
};

// ---------------------------------------------------------------------------
// Per-open-handle file context (allocated in Create/Open, freed in Close)
// ---------------------------------------------------------------------------

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
    UINT32 grantedAccess = 0;       // Access mask from Create/Open — used when
                                    // the overlay needs to reopen the handle
                                    // internally (e.g., after lazy copy-up
                                    // completion) so the caller's original
                                    // access rights survive.
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

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

class PathResolver;
class WhiteoutManager;
class MetadataADS;
class Cache;
class CopyUp;
namespace VHD { class VHDLayerManager; }
namespace VSS { class VSSManager; }
namespace LayerImage { class LayerImageManager; }

} // namespace LayerMount

// ProcessTracker must be fully defined before the atomic shared_ptr
// member below is instantiated.
#include "ProcessTracker.h"

// CapabilityGate is consumed by the engine to gate optimized vs. fallback
// paths. Header-only; lives under abi/ because it wraps a public-header
// enum, but is freely included here -- the dependency edge from impl/ to
// abi/ is small and confined to this one type.
#include "../abi/CapabilityGate.h"

// EventEmitter owns the host-supplied LM_EVENT_CALLBACK slot. Engine
// code emits through it whenever a degraded-capability path runs or a
// notable overlay event fires. Until LayerMountSetEventCallback wires a
// real callback, every Emit is a silent no-op.
#include "../abi/EventEmitter.h"

namespace LayerMount {

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------

// Normalize a relative filesystem path: strip leading backslash, normalize
// separators to backslash, fold to lowercase for case-insensitive NTFS matching.
// Shared by Cache and PathResolver to ensure consistent key normalization.
std::wstring NormalizePath(const std::wstring& path);

// Returns true if `normalized` is safe to combine with a layer root. Rejects
// empty input, drive/stream-qualified forms (any `:` character), and any `..`
// segment that would traverse out of the layer root when concatenated. Call
// this at every write-side entry point that *must not* accept stream
// qualifiers (CreateWhiteout, SetOpaque, Rename source/dest, ReadDirectory)
// before building paths with `GetUpperPath` / `BuildUpperPathPreserveCase`
// or passing results to `EnsureDirectoryExists`. For callsites that
// legitimately handle alternate data streams (Create / Open / Delete /
// UpdateContextPath), use `TryParseStreamPath` instead.
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

// Reserved internal subtree: sidecar metadata lives under `<upper>\.overlay\`
// on hosts without ADS. Exposing it through the merged namespace would let a
// user enumerate/modify/delete sidecar records and corrupt metacopy/opaque/
// origin state. Returns true for the exact name `.overlay` and any path
// beneath it. `normalized` is expected to be the output of `NormalizePath`
// (lowercased, no leading/trailing separators).
constexpr const wchar_t* kSidecarDirName = L".overlay";
bool IsReservedRelativePath(const std::wstring& normalized);

// Recursively create directories. Returns true on success or if already exists.
bool EnsureDirectoryExists(const std::wstring& path);

// ---------------------------------------------------------------------------
// MergedEntry — used by ReadDirectory and CanDelete to share merge logic
// ---------------------------------------------------------------------------

struct MergedEntry {
    WIN32_FIND_DATAW findData;
    LayerSource source;
};

// ---------------------------------------------------------------------------
// InternalFileInfo — host-agnostic file metadata used by the engine
// internally. Translated to LM_FILE_INFO at the ABI boundary.
// ---------------------------------------------------------------------------

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

// Single named-data-stream entry as observed against the resolved
// physical path of a file. Engine-internal; translated to LM_STREAM_INFO
// at the ABI boundary. Names carry NTFS's native form (e.g. ":mystream:$DATA").
struct InternalStreamInfo {
    std::wstring name;
    UINT64 streamSize;
    UINT64 allocationSize;
};

// ---------------------------------------------------------------------------
// LayerMount class — main overlay filesystem engine
// ---------------------------------------------------------------------------

class LayerMount {
public:
    explicit LayerMount(LayerConfig config);
    ~LayerMount();

    LayerMount(const LayerMount&) = delete;
    LayerMount& operator=(const LayerMount&) = delete;

    // Mount/unmount responsibilities live in the host adapter (host-driven
    // pattern). Callers drive the overlay via the primitives below;
    // filesystem-host integration lives in the adapter above the C ABI.

    // Statistics
    const LayerMountStats& Stats() const { return stats_; }

    // --- Convenience methods used by callbacks ---

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
    // hosting filesystem. The volume label is the DLL's choice (not
    // configurable today); the caller fills it.
    NTSTATUS GetVolumeInfo(UINT64* outTotalSize, UINT64* outFreeSize) const;

    // Populate InternalFileInfo from a file path.
    static NTSTATUS FillFileInfo(const std::wstring& path, InternalFileInfo* fileInfo);

    // Populate InternalFileInfo from an open handle.
    static NTSTATUS FillFileInfoFromHandle(HANDLE handle,
        InternalFileInfo* fileInfo,
        const std::wstring* pathHint = nullptr);

    // Merge directory entries from all visible layers.
    // Returns a sorted map: lowercase filename -> MergedEntry.
    std::map<std::wstring, MergedEntry> MergeDirectoryEntries(
        const std::wstring& dirRelativePath) const;

    // --- File-handle primitives ---
    //
    // Host-agnostic open / create / close. The C ABI shims in
    // abi/AbiFile.cpp are thin translators on top of these. callerPid is
    // used for ProcessTracker checks; pass 0 to skip tracking for this
    // call.
    //
    // Method names are deliberately Open/Create/Close rather than
    // OpenFile/CreateFile/CloseFile: <windows.h> defines `CreateFile` and
    // `OpenFile` as macros (UNICODE-aware redirectors), so a method by
    // those names gets silently renamed to CreateFileW/OpenFileW after
    // preprocessing and shadows the Win32 functions inside the class body.

    // Open an existing file or directory in the merged view. Allocates a
    // FileContext, opens the underlying NT handle (copying up first when a
    // write-bearing access is requested against a lower-only entry), and
    // fills outInfo. Returns the new context via outCtx (caller takes
    // ownership).
    NTSTATUS Open(const std::wstring& relativePath,
                  UINT32 grantedAccess,
                  UINT32 createOptions,
                  DWORD callerPid,
                  std::unique_ptr<FileContext>* outCtx,
                  InternalFileInfo* outInfo);

    // Create a new file or directory in the upper layer. CreateOptions
    // FILE_DIRECTORY_FILE selects directory creation. Applies the
    // (optional) self-relative security descriptor and pre-allocates
    // allocationSize bytes when non-zero. Marks a new directory as opaque
    // when an entry of the same name exists in any lower layer.
    NTSTATUS Create(const std::wstring& relativePath,
                    UINT32 createOptions,
                    UINT32 grantedAccess,
                    UINT32 fileAttributes,
                    PSECURITY_DESCRIPTOR securityDescriptor,
                    UINT64 allocationSize,
                    DWORD callerPid,
                    std::unique_ptr<FileContext>* outCtx,
                    InternalFileInfo* outInfo);

    // Close the NT handle inside ctx and decrement the active-handles
    // stat. Does NOT delete ctx; the caller (typically the FilePayload
    // holder in the handle table) owns the FileContext storage.
    void Close(FileContext* ctx);

    // Read up to `length` bytes from the open file at the given absolute
    // offset. Completes any deferred lazy copy-up before reading and
    // reopens the underlying NT handle with the caller's original
    // grantedAccess so subsequent writes still work. Returns
    // STATUS_END_OF_FILE on read past EOF (with *bytesTransferred = 0).
    // callerPid: 0 = use ctx->ownerPid (opener's PID) for access check;
    // non-zero = use this PID instead (host adapters thread the
    // originator PID in from their dispatch surface).
    NTSTATUS Read(FileContext* ctx,
                  void* buffer,
                  UINT64 offset,
                  ULONG length,
                  DWORD callerPid,
                  PULONG bytesTransferred);

    // Write `length` bytes to the open file. Triggers copy-up if the file
    // is still in a lower layer. writeToEnd=TRUE appends regardless of
    // offset; constrainedIo=TRUE truncates the write to whatever fits in
    // the current EOF (no extension). Fills outInfo with the post-write
    // file metadata when non-null. callerPid semantics as for Read.
    NTSTATUS Write(FileContext* ctx,
                   const void* buffer,
                   UINT64 offset,
                   ULONG length,
                   BOOLEAN writeToEnd,
                   BOOLEAN constrainedIo,
                   DWORD callerPid,
                   PULONG bytesTransferred,
                   InternalFileInfo* outInfo);

    // CREATE_ALWAYS-style truncate of an already-open file. Copies the
    // entry up to the upper layer if needed, deletes non-overlay ADS
    // streams (user content under CREATE_ALWAYS is destroyed but our
    // :overlay* bookkeeping survives), truncates to zero, applies
    // allocation, and either replaces or ORs the file attributes.
    NTSTATUS Overwrite(FileContext* ctx,
                       UINT32 fileAttributes,
                       BOOLEAN replaceAttributes,
                       UINT64 allocationSize,
                       DWORD callerPid,
                       InternalFileInfo* outInfo);

    // Flush buffered writes for the open file.
    NTSTATUS Flush(FileContext* ctx,
                   DWORD callerPid,
                   InternalFileInfo* outInfo);

    NTSTATUS EnsureHandleReady(FileContext* ctx);

    // Apply attribute / timestamp / size mutations to an open file. The
    // sentinel values in the public ABI map through here unchanged:
    //   fileAttributes == INVALID_FILE_ATTRIBUTES -> leave unchanged
    //   {creation,lastAccess,lastWrite,change}Time == 0 -> leave unchanged
    //   allocationSize == UINT64_MAX -> leave unchanged
    //   fileSize       == UINT64_MAX -> leave unchanged
    // Triggers copy-up if the file is still in a lower layer. Fills
    // outInfo with the post-mutation metadata when non-null.
    NTSTATUS SetInfo(FileContext* ctx,
                     UINT32 fileAttributes,
                     UINT64 creationTime,
                     UINT64 lastAccessTime,
                     UINT64 lastWriteTime,
                     UINT64 changeTime,
                     UINT64 allocationSize,
                     UINT64 fileSize,
                     InternalFileInfo* outInfo);

    // Path-based delete: remove the entry from the upper layer (or simply
    // whiteout if it lives only in lower). Validates read-only and
    // directory-not-empty before mutating, mirroring the legacy SCanDelete
    // checks. Returns STATUS_OBJECT_NAME_NOT_FOUND when neither layer
    // has it.
    NTSTATUS CanDelete(const std::wstring& relativePath, DWORD callerPid);
    NTSTATUS Delete(const std::wstring& relativePath, DWORD callerPid);
    NTSTATUS CanDelete(FileContext* ctx, DWORD callerPid);
    NTSTATUS Delete(FileContext* ctx, DWORD callerPid);

    // Path-based rename: moves the entry within the overlay namespace.
    // Source is copied up if it lives only in lower; if lower retains a
    // copy at the old path the helper drops a whiteout there to suppress
    // it from the merged view. Directory renames go through
    // CopyUp::HandleDirectoryRename. replaceIfExists FALSE fails with
    // STATUS_OBJECT_NAME_COLLISION when the destination already exists.
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
    // or stream-qualified form, contains `..` traversal, or lands in a
    // reserved internal subtree. The open handle is left unchanged on
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
    // outAttributes (optional) receives the file's Win32 attributes.
    NTSTATUS GetSecurity(const std::wstring& relativePath,
                         PUINT32 outAttributes,
                         PSECURITY_DESCRIPTOR sd,
                         SIZE_T sdBytes,
                         SIZE_T* requiredBytes);

    // Path-based security mutator. Triggers copy-up to the upper layer if
    // the entry lives only in lower. Applies via path-based
    // ::SetFileSecurityW; the legacy file-context-based path used
    // SetKernelObjectSecurity on an open handle but this shim has no
    // handle to leverage.
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

    // --- Accessors for callbacks ---
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

    // Capability gate. Engine code queries this to pick the optimized
    // vs. fallback path for ADS metadata, reparse copy-up, sparse
    // copy-up, and NTFS ACL semantics.
    const ::LayerMount::abi::CapabilityGate& Capabilities() const noexcept {
        return capabilities_;
    }

    // Event emitter. The ABI shim LayerMountSetEventCallback installs the
    // host's callback here; engine code emits through it.
    ::LayerMount::abi::EventEmitter& Events() noexcept { return events_; }
    const ::LayerMount::abi::EventEmitter& Events() const noexcept { return events_; }

    // VHD subsystem. Lazy-constructed on first call because most overlays
    // never exercise VHD paths and VHDLayerManager's ctor acquires a
    // cross-process ManifestLock. Thread-safe to call concurrently; the
    // returned reference is stable for the overlay's lifetime.
    VHD::VHDLayerManager& Vhd();

    // VSS subsystem. Lazy-constructed like Vhd(). VSSManager's methods
    // require an active ComScope on the caller's thread; the VSS ABI
    // shims open one on entry. Thread-safe.
    VSS::VSSManager& Vss();

    // Layer image subsystem. Stateless today (every method takes paths
    // and is a one-shot operation), but owned by the engine for lifecycle
    // symmetry with Vhd()/Vss() and so future per-overlay caches can
    // attach without breaking the ABI.
    LayerImage::LayerImageManager& Images();

    // Runtime process-tracker toggle. TRUE constructs the tracker (if
    // absent) using the overlay's accessLogCapacity; FALSE tears it down.
    // Returns S_OK on state change or no-op; callers observe the effect
    // via subsequent ABI calls. Safe against concurrent
    // TrackerSnapshot() callers -- they see either the old tracker (and
    // hold the last reference until they release it) or the new state.
    HRESULT SetProcessTrackerEnabled(bool enabled);

private:
    // --- Members (declared in construction order) ---

    LayerConfig config_;
    ::LayerMount::abi::CapabilityGate capabilities_;
    ::LayerMount::abi::EventEmitter   events_;
    std::unique_ptr<Cache> cache_;
    std::unique_ptr<WhiteoutManager> whiteoutMgr_;
    std::unique_ptr<PathResolver> pathResolver_;
    LayerMountStats stats_;
    std::unique_ptr<CopyUp> copyUp_;
    // Shared ownership so callback readers can pin the tracker for
    // the duration of a check without coordinating with concurrent
    // SetProcessTrackerEnabled(false) toggles. The shared_mutex below
    // guards read/write access to this pointer; readers take shared
    // lock, copy out the shared_ptr (atomic from their POV), and
    // release. Toggles take unique lock.
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

} // namespace LayerMount
