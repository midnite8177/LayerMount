#pragma once

#include "WindowsNtStatus.h"
#include "../abi/CapabilityGate.h"
#include <string>
#include <string_view>
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

    ::LayerMount::abi::CapabilityGate Capabilities() const noexcept {
        return ::LayerMount::abi::CapabilityGate(hostCapabilities);
    }

    // Checks the layout and writes and deletes a probe file in upperPath.
    // Returns false and sets error on failure.
    bool Validate(std::wstring& error) const;

    // Creates workDirPath if it doesn't exist and checks that it is on the
    // volume of upperPath. Call after Validate(). Returns E_FAIL when
    // workDirPath is empty, the work directory cannot be created or either
    // volume cannot be read, and E_INVALIDARG when the volumes differ, with
    // the reason in error.
    HRESULT Prepare(std::wstring& error);
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

// The kind of an entry. A Link is a directory reparse point whose reparse
// tag is a name surrogate, such as a junction or a directory symbolic link.
// Any other directory reparse point is a Directory. A rename or a delete does
// not follow a Link and treats it as a non-directory, as overlayfs treats a
// symlink. The one exception is that a Link rename source keeps the
// directory check that refuses a destination inside its own tree.
enum class EntryKind {
    File,
    Directory,
    Link,
};

// Whether a walk through the merged view follows a relative symbolic link
// at the last component of a path. A link at any other component is always
// followed. Open and Create follow it unless createOptions holds
// FILE_OPEN_REPARSE_POINT. MergeDirectoryEntries, EnumerateStreams,
// GetSecurity and SetSecurity follow it. The other calls that take a path
// keep it and act on the link itself.
enum class FinalLink {
    Follow,
    Keep,
};

// A path in the merged view, as PathResolver::ViewPathThroughLinks gives
// it. Only that walk makes one, so a call that takes a ViewPath cannot
// skip the walk.
class ViewPath {
public:
    // The path in the case of the caller's path and of the link targets,
    // with the caller's stream suffix.
    const std::wstring& Path() const { return path_; }

    // Path() in NormalizePath form.
    const std::wstring& Normalized() const { return normalized_; }

private:
    friend class PathResolver;

    ViewPath(std::wstring path, std::wstring normalized)
        : path_(std::move(path))
        , normalized_(std::move(normalized)) {
    }

    std::wstring path_;
    std::wstring normalized_;
};

// The status of a walk through the merged view and its path. The path is
// empty when the status is a failure.
struct ViewLookup {
    NTSTATUS status;
    ViewPath path;
};

// Whether CreateWhiteout or SetOpaque accepted the view path for a marker.
enum class MarkerPath {
    Accepted,
    Refused,
};

struct CreateResolution {
    // The overlay's hit, as ResolvePath returns it.
    ResolvedPath overlayHit;
    // True when the upper holds a whiteout for exactly this path.
    bool whiteoutAtPath = false;
    // The lower hit, as ResolveLowerPath returns it.
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
    bool entryIsReparsePoint = false; // The entry at actualPath is a reparse
                                      // point, such as a link. The open sets
                                      // it from the resolved entry.
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
enum class WhiteoutType;
class MetadataStore;
class Cache;
class CopyUp;
class DirectoryRename;
class FileRename;
class RenameRollback;
struct UpperRename;
struct MovedSource;
enum class RenameCopyUp;
struct MovedFile;
class UpperEntryRemover;
namespace VHD { class VHDLayerManager; }
namespace VSS { class VSSManager; }
namespace LayerImage { class LayerImageManager; }

}

#include "ProcessTracker.h"
#include "SecurityPolicy.h"

#include "../abi/EventEmitter.h"

namespace LayerMount {

// Normalize a relative filesystem path: strip leading backslash, normalize
// separators to backslash, fold to lowercase for case-insensitive NTFS matching.
std::wstring NormalizePath(const std::wstring& path);

// Returns true if `normalized` is safe to combine with a layer root. Rejects
// empty input, drive/stream-qualified forms (any `:` character), and any `..`
// segment that would traverse out of the layer root when concatenated.
bool IsSafeRelativePath(const std::wstring& normalized);

// Returns true if `ntfsStreamName` is a user alternate data stream. The name
// is the NTFS form that stream enumeration gives, `:name:$TYPE`. The main
// stream `::$DATA` and the reserved streams return false. The reserved
// streams are `:overlay`, which is `kLayerMountADSStream`, and every stream
// whose name starts with `overlay.`, such as `:overlay.opaque`. The compare
// ignores case, as NTFS stream names do. A stream such as `:overlayNotes` is
// user data.
bool IsUserAlternateStream(std::wstring_view ntfsStreamName) noexcept;

// Parse a normalized relative path into <host>[:<stream>[:$DATA]] components.
// Returns true iff every rule holds:
//   - `normalized` is non-empty
//   - host portion (the substring before the first `:`) passes
//     `IsSafeRelativePath` (no embedded `:`, no `..`, no empty, no
//     drive-qualifier)
//   - if a stream is present: the stream name is non-empty, contains no `\`,
//     and is not a reserved name (`overlay` or a name under `overlay.`)
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

// A path split into its host and its stream suffix, in two forms. hostNorm
// and streamSuffix are lowercase, as TryParseStreamPath gives them for a
// normalized path, and serve as lookup keys. callerHost and
// callerStreamSuffix keep the case the caller wrote, and name a new entry.
// Both suffixes are empty when the path names no stream.
struct StreamPath {
    std::wstring hostNorm;
    std::wstring streamSuffix;
    std::wstring callerHost;
    std::wstring callerStreamSuffix;
};

// Parses relativePath in the caller's case under the rules of
// TryParseStreamPath, which ignore case. Returns std::nullopt where
// TryParseStreamPath returns false.
std::optional<StreamPath> ParseStreamPath(const std::wstring& relativePath);

// Returns true for a path that the overlay never shows. That is the
// sidecar metadata subtree `.overlay` at the overlay root and anything
// beneath it, and any path with a segment that starts with `kWhiteoutPrefix`
// in any case, which covers whiteout markers, the opaque marker and anything
// beneath a marker-named directory. The sidecar store exists only at the
// root, so a `.overlay` below the root is an ordinary name. The `.overlay`
// compare is case-sensitive, so `normalized` must be the lowercased output
// of `NormalizePath`. This is the string check only. A gate that resolves
// a caller's path in the overlay uses IsReservedOverlayPath, which also
// reads the layers.
constexpr const wchar_t* kSidecarDirName = L".overlay";
bool IsReservedRelativePath(const std::wstring& normalized);

// Whether `normalized` is the sidecar subtree `.overlay` at the overlay root
// or a path beneath it, as IsReservedRelativePath reads it.
bool IsRootSidecarPath(const std::wstring& normalized);

// The directory that holds the first segment of `normalized` that starts
// with `kWhiteoutPrefix` in any case, or no value when no segment does. An
// empty view is the overlay root. The view points into `normalized`.
std::optional<std::wstring_view> ParentOfFirstMarkerSegment(std::wstring_view normalized);

// Recursively create directories. Returns true on success or if already exists.
bool EnsureDirectoryExists(const std::wstring& path);

struct MergedEntry {
    WIN32_FIND_DATAW findData;
    LayerSource source;
    int lowerIndex;             // which lower layer (0-based), -1 if upper
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

    // Fills fileInfo from the handle. pathHint, when not null or empty, is
    // the path the handle was opened at, and pathHintIsReparsePoint tells
    // whether the entry there is a reparse point, such as a link. The
    // IndexNumber is the stable ID of the copy-up record of the entry the
    // handle is on, or the NTFS file ID when that entry has none. A handle
    // that followed a link at pathHint is on the link's target and takes the
    // target's record, never the link's. Such a handle costs two
    // GetFinalPathNameByHandleW calls on every fill.
    NTSTATUS FillFileInfoFromHandle(HANDLE handle,
        InternalFileInfo* fileInfo,
        const std::wstring* pathHint,
        bool pathHintIsReparsePoint) const;

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
    // A relative symbolic link at the last component of the path resolves
    // in the merged view unless createOptions holds FILE_OPEN_REPARSE_POINT.
    //
    // A metacopy shell fills before its handle opens when grantedAccess
    // asks for data: read, write, append, or execute. An open for
    // attributes, security, or delete, and an open of one of the shell's
    // streams, keep the shell sparse. A failed fill returns its status,
    // leaves *outCtx null, and opens no handle.
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
    // directory as opaque when a whiteout at the path hides a lower
    // directory of the same name. Returns
    // STATUS_OBJECT_NAME_COLLISION when the overlay already holds a path
    // without a stream suffix, and for a stream create when the host file
    // that the overlay holds already has that stream. A stream create on
    // a metacopy shell keeps the shell sparse.
    NTSTATUS Create(const CreateRequest& callerRequest,
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
    // entry up to the upper layer if needed, deletes the ADS streams
    // `IsUserAlternateStream` accepts, truncates to zero, applies
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

    // Makes the checks of CanDelete first. Removes the entry from the upper,
    // and writes a whiteout when a lower holds it. Under a lower junction or
    // absolute symbolic link that no higher layer holds, copies the link up
    // as a link first, so the delete removes the entry from the link target
    // and writes no whiteout.
    NTSTATUS Delete(const std::wstring& relativePath, DWORD callerPid);
    NTSTATUS Delete(FileContext* ctx);

    // Delete the alternate data stream that `ctx` opened. The host file
    // and its other streams stay.
    NTSTATUS DeleteStreamOnContext(FileContext* ctx);

    // Path-based rename: moves the entry within the overlay namespace.
    // Outside a link target, Rename copies up a source that lives only in a
    // lower, and drops a whiteout at the old path when a lower still holds
    // it. A rename to the identical name succeeds and changes nothing.
    // replaceIfExists FALSE fails with STATUS_OBJECT_NAME_COLLISION when
    // the destination already exists. Otherwise the rename of a directory
    // or a link to a path inside its own tree fails with
    // STATUS_INVALID_PARAMETER, even when the destination is a non-empty
    // directory. A directory rename with replaceIfExists TRUE onto a
    // directory fails with STATUS_DIRECTORY_NOT_EMPTY when the merged view
    // shows a child of the destination. A link, as EntryKind defines
    // it, is a non-directory on either side of a rename. With
    // replaceIfExists TRUE, a file or a link replaces a link, and a link
    // replaces a file. The link moves as a link, and its target stays
    // unchanged. Before the move, Rename moves the upper destination into the
    // work directory. It puts the destination back when the move fails and
    // removes it when the move succeeds. A replace of a read-only upper file
    // fails with STATUS_ACCESS_DENIED before any side effects. A failure to
    // read the reparse tag of a source or destination fails the rename with
    // that status before any change. The FileContext overload makes these
    // checks before it closes ctx->handle, so a refused rename leaves the
    // handle open. CheckRenameLinkBoundary refuses a rename whose source
    // and destination are not under the same link with
    // STATUS_NOT_SAME_DEVICE, and one with a component on either path that
    // it cannot read with STATUS_ACCESS_DENIED, before any change. A rename
    // within the target of one lower junction or absolute symbolic link,
    // into a destination parent that the merged view shows as a directory,
    // copies the link up as a link first, then renames the entry in the
    // target and writes no whiteout.
    NTSTATUS Rename(const std::wstring& callerOldPath,
                    const std::wstring& callerNewPath,
                    BOOLEAN replaceIfExists,
                    DWORD callerPid);
    NTSTATUS Rename(FileContext* ctx,
                    const std::wstring& callerNewPath,
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
    // path (see `PathResolver::IsReservedPath`). The open handle is left
    // unchanged on rejection so the caller can surface the error without
    // losing state.
    NTSTATUS UpdateContextPath(FileContext* ctx,
                               const std::wstring& callerNewPath);

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
    // unnamed stream (`::$DATA`) and every stream that
    // `IsUserAlternateStream` rejects.
    // Returns STATUS_OBJECT_NAME_NOT_FOUND when the file is absent in
    // every layer. Returns STATUS_SUCCESS with an empty `out` when the
    // file exists but carries no user-visible streams.
    NTSTATUS EnumerateStreams(const std::wstring& relativePath,
                              std::vector<InternalStreamInfo>& out);

    // WhiteoutManager::CreateWhiteout for the view path of relativePath.
    // Sets *markerPath to Refused, writes nothing and returns
    // STATUS_OBJECT_NAME_INVALID when IsSafeRelativePath refuses the view
    // path or IsReservedRelativePath names it. Otherwise sets it to
    // Accepted and returns the status of the walk or of the write.
    NTSTATUS CreateWhiteout(const std::wstring& relativePath,
                            WhiteoutType type,
                            MarkerPath* markerPath);

    // WhiteoutManager::SetOpaque for the view path of dirRelativePath, with
    // the path checks of CreateWhiteout.
    NTSTATUS SetOpaque(const std::wstring& dirRelativePath, MarkerPath* markerPath);

    // Sets *resolved to PathResolver::ResolvePath of the view path of
    // relativePath. Returns the walk's failure and leaves *resolved
    // unchanged when the walk fails.
    NTSTATUS ResolvePath(const std::wstring& relativePath, ResolvedPath* resolved) const;

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

    // PathResolver::ViewPathThroughLinks of a caller's path. Each public
    // call that takes a path starts with it.
    ViewLookup WalkCallerPath(const std::wstring& callerPath, FinalLink finalLink) const;

    // The path-based rename after the walk, for paths that are already
    // view paths.
    NTSTATUS RenameViewPaths(const std::wstring& oldRelativePath,
                             const std::wstring& newRelativePath,
                             BOOLEAN replaceIfExists,
                             DWORD callerPid);

    NTSTATUS OpenRoot(UINT32 grantedAccess,
                      UINT32 createOptions,
                      DWORD callerPid,
                      std::unique_ptr<FileContext>* outCtx,
                      InternalFileInfo* outInfo);

    // What Create resolved for a new entry, and the caller's settings for it.
    struct UpperCreate {
        StreamPath path;
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

    // Copies the host file of a stream create up with its data and streams
    // when the overlay shows it from a lower. A host that a whiteout or an
    // opaque ancestor hides stays absent, and the stream create then makes
    // an empty host. An upper host, a metacopy shell included, stays as it
    // is. Runs before the stream create, so the copy-up cannot bring the
    // lower's streams in over the new one.
    NTSTATUS PrepareStreamHost(const UpperCreate& create);

    // Sets ctx->isMetacopyOnly when it stages a shell.
    NTSTATUS CopyUpForWriteOpen(const std::wstring& hostNorm, FileContext* ctx);

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

    // The delete check of both CanDelete overloads. An entry that
    // EntryKindOf names a Directory must be empty in the merged view. A
    // File or a Link passes, because what lies below a link is not its
    // child. Returns the error of a reparse tag that cannot be read.
    NTSTATUS CanDeleteEntry(const std::wstring& hostNorm,
                            const std::wstring& streamSuffix,
                            DWORD callerPid);

    // Deletes the alternate data stream streamSuffix of the upper file at
    // hostNorm. Returns STATUS_OBJECT_NAME_NOT_FOUND when the upper does
    // not hold the host file.
    NTSTATUS DeleteStreamByPath(const std::wstring& hostNorm,
                                const std::wstring& streamSuffix);

    NTSTATUS RemoveEntry(const std::wstring& hostNorm);

    // The source and destination paths of a rename, in NormalizePath form.
    struct RenamePaths {
        const std::wstring& oldNorm;
        const std::wstring& newNorm;
    };

    // The checks of a rename that need no lookup of the source or the
    // destination: stream names, reserved paths and the access tracker.
    NTSTATUS CheckRenameRequest(const RenamePaths& paths, DWORD callerPid);

    // The kinds of a rename's source and, when the merged view shows one,
    // its destination.
    struct RenameKinds {
        EntryKind source;
        std::optional<EntryKind> destination;
    };

    // Runs the checks in the order that overlayfs uses, and sets
    // kinds->destination.
    NTSTATUS CheckRenameDestination(const RenamePaths& paths,
                                    BOOLEAN replaceIfExists,
                                    RenameKinds* kinds) const;

    // The kinds and the links of a rename that passed CheckRename.
    struct CheckedRename;

    // The checks of both Rename overloads, in order: CheckRenameRequest, the
    // lookup of the source, CheckRenameLinkBoundary, the kind of the source
    // and, for a new path, CheckRenameDestination. Fills *checked and
    // changes nothing.
    NTSTATUS CheckRename(const RenamePaths& paths,
                         BOOLEAN replaceIfExists,
                         DWORD callerPid,
                         CheckedRename* checked);

    // How a rename moves a directory or a link.
    enum class DirectoryRenameRoute {
        // Copies the lower up and merges the upper entry into the copy.
        MergeLower,
        // No lower entry has the old name.
        MoveUpper,
        // An upper entry hides a lower entry at the old name.
        MoveUpperAndWhiteout,
    };

    // Sets *route as overlayfs decides it. The top layer decides the kind.
    // The rename merges the lower only when the upper has no entry at
    // oldNorm, or when the upper entry is a directory without the opaque
    // marker over a lower directory. Any other lower entry stays hidden.
    // When the reparse tag of the lower entry cannot be read, returns that
    // error and leaves *route unchanged.
    NTSTATUS DirectoryRenameRouteOf(const std::wstring& oldNorm,
                                    EntryKind sourceKind,
                                    DirectoryRenameRoute* route) const;

    // The status of a rename, whether it left a metacopy shell at the new
    // name, and whether its source was under a link, where the rename moved
    // the entry within the link target and copied nothing up.
    struct RenameResult {
        NTSTATUS status;
        bool stagedShell;
        bool sourceInLinkTarget;
    };

    // The part of a rename after its checks passed. Copies up the lower link
    // that checked names first. copyUpMode chooses how a file that only a
    // lower holds copies up. When the upper has a whiteout at the new name
    // that cannot go, the rename moves its entry back and fails with the
    // error of that removal.
    RenameResult RenameCheckedEntry(const std::wstring& oldRelativePath,
                                    const std::wstring& newRelativePath,
                                    BOOLEAN replaceIfExists,
                                    const CheckedRename& checked,
                                    RenameCopyUp copyUpMode);

    // Moves the source to newRelativePath with RenameFileEntry,
    // RenameDirectoryEntry or, when the paths differ only in case,
    // CopyUp::RenameDirectoryCase. route applies to a Directory or a Link
    // sourceKind. The result's rename names the source's upper path before
    // the move.
    MovedSource MoveRenameSource(const std::wstring& oldRelativePath,
                                 const std::wstring& newRelativePath,
                                 BOOLEAN replaceIfExists,
                                 EntryKind sourceKind,
                                 DirectoryRenameRoute route,
                                 RenameCopyUp copyUpMode);

    // Moves a file to newRelativePath in the upper. When a lower holds the
    // source, a whiteout then hides it at the old name. The result's status
    // is that of the whiteout write when the move succeeded.
    MovedFile RenameFileEntry(const std::wstring& oldRelativePath,
                              const std::wstring& newRelativePath,
                              BOOLEAN replaceIfExists,
                              RenameCopyUp copyUpMode);

    // Moves a directory or a link along route, then writes the whiteout at
    // the old name for the MergeLower and MoveUpperAndWhiteout routes. When
    // that write fails, RenameRollback::WhiteOutSource moves the entry back
    // as rename describes.
    NTSTATUS RenameDirectoryEntry(const std::wstring& oldRelativePath,
                                  const std::wstring& newRelativePath,
                                  EntryKind sourceKind,
                                  DirectoryRenameRoute route,
                                  BOOLEAN replaceIfExists,
                                  const UpperRename& rename);

    LayerConfig config_;
    // The final path of config_.upperPath, read once at mount, in the
    // extended form that GetFinalPathNameByHandleW gives. Empty when the
    // mount could not read it.
    std::wstring upperFinalPath_;
    SecurityPolicy                    securityPolicy_;
    ::LayerMount::abi::EventEmitter   events_;
    std::unique_ptr<Cache> cache_;
    std::unique_ptr<WhiteoutManager> whiteoutMgr_;
    std::unique_ptr<PathResolver> pathResolver_;
    LayerMountStats stats_;
    std::unique_ptr<CopyUp> copyUp_;
    std::unique_ptr<DirectoryRename> directoryRename_;
    std::unique_ptr<FileRename> fileRename_;
    std::unique_ptr<RenameRollback> renameRollback_;
    std::unique_ptr<UpperEntryRemover> upperEntryRemover_;
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
