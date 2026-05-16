#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "MetadataADS.h"
#include "Cache.h"
#include "NtStatusUtil.h"

#include <winioctl.h>
#include <aclapi.h>

#pragma comment(lib, "advapi32.lib")

namespace {

// Enable a single privilege in the current process token. Returns true on
// successful adjust. Idempotent — re-enabling an already-enabled privilege is
// a successful no-op.
bool EnablePrivilege(LPCWSTR privName) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                            &token)) {
        return false;
    }
    LUID luid{};
    if (!::LookupPrivilegeValueW(nullptr, privName, &luid)) {
        ::CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL ok = ::AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp),
                                              nullptr, nullptr);
    // AdjustTokenPrivileges returns TRUE even for "not all assigned"; the
    // actual outcome is in GetLastError. ERROR_NOT_ALL_ASSIGNED means the
    // process token doesn't carry the privilege at all (standard user).
    const DWORD err = ::GetLastError();
    ::CloseHandle(token);
    return ok && err == ERROR_SUCCESS;
}

// One-shot enabler for the privileges copy-up needs to bypass user-level
// ACLs and place reparse points. Called at the top of any CopyUp entry point
// that may stage a symlink/junction or commit through a restrictive parent.
//
//   - SE_CREATE_SYMBOLIC_LINK_NAME: required by FSCTL_SET_REPARSE_POINT for
//     IO_REPARSE_TAG_SYMLINK. Without it, copying up a lower symlink fails
//     with ERROR_PRIVILEGE_NOT_HELD even when running elevated, because
//     elevated tokens carry the privilege DISABLED by default.
//   - SE_RESTORE_NAME / SE_BACKUP_NAME: lets FILE_FLAG_BACKUP_SEMANTICS opens
//     bypass DACL checks; needed when copy-up of a child commits into a
//     parent directory whose copied-up DACL denies write.
//   - SE_SECURITY_NAME: required to read/write SACL_SECURITY_INFORMATION
//     (audit ACEs). Without it, copy-up silently drops SACLs — compliance
//     controls (file-access audit rules) disappear on first modification.
//     Gracefully no-ops on standard-user tokens where the priv is not held.
bool IsSecurityPrivHeld() {
    static std::once_flag once;
    static bool held = false;
    std::call_once(once, []() { held = EnablePrivilege(SE_SECURITY_NAME); });
    return held;
}

void EnsureCopyUpPrivileges() {
    static std::once_flag once;
    std::call_once(once, []() {
        EnablePrivilege(SE_CREATE_SYMBOLIC_LINK_NAME);
        EnablePrivilege(SE_RESTORE_NAME);
        EnablePrivilege(SE_BACKUP_NAME);
        // SACL-bearing privilege is tracked separately so callers can skip
        // SACL reads/writes when it isn't held (standard-user process).
        (void)IsSecurityPrivHeld();
    });
}

uint64_t MakeIndexNumber(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<uint64_t>(info.nFileIndexHigh) << 32) |
           info.nFileIndexLow;
}

bool TryGetStableIndexNumberFromHandle(HANDLE h, uint64_t& outIndexNumber) {
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h, &info)) {
        return false;
    }

    outIndexNumber = MakeIndexNumber(info);
    return true;
}

bool TryGetStableIndexNumberFromPath(const std::wstring& path,
                                     uint64_t& outIndexNumber) {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    DWORD flags = 0;
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        flags |= FILE_FLAG_BACKUP_SEMANTICS;
    }
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }

    HANDLE h = ::CreateFileW(path.c_str(),
                             FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE,
                             nullptr,
                             OPEN_EXISTING,
                             flags,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    const bool ok = TryGetStableIndexNumberFromHandle(h, outIndexNumber);
    ::CloseHandle(h);
    return ok;
}

LayerMount::LayerMountMetadata MakeCopyUpMetadata(
    const std::wstring& sourcePath,
    HANDLE sourceHandle = INVALID_HANDLE_VALUE) {
    LayerMount::LayerMountMetadata metadata = {};
    ::GetSystemTimeAsFileTime(&metadata.copyUpTimestamp);
    metadata.originLayer = sourcePath;

    uint64_t stableIndexNumber = 0;
    if (TryGetStableIndexNumberFromHandle(sourceHandle, stableIndexNumber) ||
        TryGetStableIndexNumberFromPath(sourcePath, stableIndexNumber)) {
        metadata.hasStableIndexNumber = true;
        metadata.stableIndexNumber = stableIndexNumber;
    }

    return metadata;
}

// Is this stream name one of the overlay's reserved bookkeeping streams?
// NTFS stream names come back from FindFirstStreamW as ":name:$DATA" or
// ":name:$<type>". The overlay writes :overlay and :overlay.opaque — both
// should be re-generated by the copy-up flow itself, never mirrored from
// the lower layer.
bool IsReservedLayerMountStream(const std::wstring& streamName) {
    static constexpr std::wstring_view kLayerMount(L":overlay");
    if (streamName.size() < kLayerMount.size()) return false;
    // Compare case-insensitively via lowercase copy — stream names are
    // case-insensitive on NTFS.
    std::wstring lower(streamName.begin(), streamName.begin() + kLayerMount.size());
    ::CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    return lower.compare(0, kLayerMount.size(), kLayerMount) == 0;
}

// Copy a single alternate data stream from src to dst. srcPath and dstPath
// are the full paths to the underlying files; streamName is the `:name:$<T>`
// suffix as returned by FindFirstStreamW / FILE_STREAM_INFO.StreamName.
//
// Both opens use FILE_FLAG_BACKUP_SEMANTICS — combined with SE_BACKUP_NAME /
// SE_RESTORE_NAME (enabled in EnsureCopyUpPrivileges) this bypasses DACL
// checks, so a lower file inside a directory whose DACL denies our process
// (e.g. an inherited DENY-WRITE for Everyone) can still have its ADS copied
// up. Without backup semantics the destination open would fail because the
// just-committed upper file inherits the same restrictive DACL from its
// parent shadow, and silent ADS loss would result.
bool CopyAlternateStream(const std::wstring& srcPath,
                         const std::wstring& dstPath,
                         const std::wstring& streamName) {
    const std::wstring srcFull = srcPath + streamName;
    const std::wstring dstFull = dstPath + streamName;

    HANDLE srcH = ::CreateFileW(srcFull.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN |
                                      FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if (srcH == INVALID_HANDLE_VALUE) return false;

    HANDLE dstH = ::CreateFileW(dstFull.c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL |
                                      FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if (dstH == INVALID_HANDLE_VALUE) {
        ::CloseHandle(srcH);
        return false;
    }

    BYTE buf[64 * 1024];
    bool ok = true;
    for (;;) {
        DWORD r = 0;
        if (!::ReadFile(srcH, buf, sizeof(buf), &r, nullptr)) {
            ok = false;
            break;
        }
        if (r == 0) break;
        DWORD w = 0;
        if (!::WriteFile(dstH, buf, r, &w, nullptr) || w != r) {
            ok = false;
            break;
        }
    }

    ::CloseHandle(srcH);
    ::CloseHandle(dstH);

    if (!ok) {
        // Remove partially-written destination stream so callers don't observe
        // a truncated ADS on the upper layer.
        ::DeleteFileW(dstFull.c_str());
    }
    return ok;
}

// Enumerate user-visible alternate data streams on srcPath and copy each to
// dstPath. Skips the main `::$DATA` stream (that one is carried by the normal
// file-data copy) and the overlay's reserved `:overlay*` bookkeeping streams.
// Returns true iff every stream copied successfully. A failure to enumerate
// (FindFirstStreamW) when the source has no streams returns true; otherwise
// any per-stream copy failure surfaces here so callers don't silently lose
// Zone.Identifier or app-specific ADS during copy-up.
// Enumerate user-visible alternate data streams on srcPath and copy each to
// dstPath. Skips the main `::$DATA` stream (carried by the normal file-data
// copy) and the overlay's reserved `:overlay*` bookkeeping streams.
//
// Implementation note: uses handle-based GetFileInformationByHandleEx with
// FileStreamInfo rather than the path-based FindFirstStreamW. Handle-based
// enumeration honors FILE_FLAG_BACKUP_SEMANTICS on the source open, which
// (with SE_BACKUP_NAME enabled in EnsureCopyUpPrivileges) bypasses DACL
// checks so a lower file inside a directory whose ACL denies our process
// can still have its ADS enumerated. The path-based FindFirstStreamW does
// not carry backup semantics and would fail with ERROR_ACCESS_DENIED on
// such files — silently losing all ADS during copy-up.
//
// Returns true iff every stream copied successfully. A "no streams" outcome
// (rare; even an empty file usually has ::$DATA) is treated as success.
bool CopyUserAlternateDataStreams(const std::wstring& srcPath,
                                  const std::wstring& dstPath) {
    HANDLE srcH = ::CreateFileW(srcPath.c_str(),
                                  FILE_GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (srcH == INVALID_HANDLE_VALUE) {
        // ERROR_FILE_NOT_FOUND is a caller bug (file shouldn't have been
        // copied up if it doesn't exist). Anything else is an open failure
        // we shouldn't paper over — surface it.
        DWORD err = ::GetLastError();
        return err == ERROR_FILE_NOT_FOUND;
    }

    // 64 KB initial buffer holds thousands of stream entries — far more than
    // any sane file has. Real user files typically have 1-3 streams. If a
    // future use case hits a file with absurd numbers of streams the buffer
    // can be grown dynamically; not worth the complexity today.
    std::vector<BYTE> buf(64 * 1024);
    if (!::GetFileInformationByHandleEx(srcH, FileStreamInfo, buf.data(),
                                          static_cast<DWORD>(buf.size()))) {
        DWORD err = ::GetLastError();
        ::CloseHandle(srcH);
        // ERROR_HANDLE_EOF: file has no streams (uncommon — even an empty
        // file usually exposes ::$DATA via this API). ERROR_MORE_DATA would
        // indicate the buffer was too small; treat that as a real failure
        // so callers know the enumeration was incomplete rather than letting
        // an unenumerated stream silently disappear.
        return err == ERROR_HANDLE_EOF;
    }
    ::CloseHandle(srcH);

    bool ok = true;
    auto* p = reinterpret_cast<FILE_STREAM_INFO*>(buf.data());
    while (true) {
        // StreamName is NOT null-terminated; length is in bytes.
        const std::wstring name(p->StreamName,
                                p->StreamNameLength / sizeof(wchar_t));
        if (name != L"::$DATA" && !IsReservedLayerMountStream(name)) {
            if (!CopyAlternateStream(srcPath, dstPath, name)) {
                ok = false;
                // Don't break — keep trying remaining streams so a partial
                // result is at least as complete as possible. Caller decides
                // what to do with the failure (usually: tear down dst and
                // surface an error).
            }
        }
        if (p->NextEntryOffset == 0) break;
        p = reinterpret_cast<FILE_STREAM_INFO*>(
            reinterpret_cast<BYTE*>(p) + p->NextEntryOffset);
    }
    return ok;
}

// Propagate NTFS EFS state. Unlike sparse/compression, encryption is applied
// path-wise rather than via a writable handle. Run it once the destination
// path exists and no conflicting handle is open; if we cannot preserve the
// encrypted state, fail the copy-up rather than silently materializing
// plaintext in upper.
bool ApplyEncryptedStateIfNeeded(const std::wstring& path, DWORD attrs) {
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_ENCRYPTED) == 0) {
        return true;
    }
    if (::EncryptFileW(path.c_str())) {
        return true;
    }
    const DWORD encryptErr = ::GetLastError();
    const DWORD encryptedAttrs = ::GetFileAttributesW(path.c_str());
    if (encryptedAttrs != INVALID_FILE_ATTRIBUTES &&
        (encryptedAttrs & FILE_ATTRIBUTE_ENCRYPTED) != 0) {
        return true;
    }
    ::SetLastError(encryptErr ? encryptErr : ERROR_ACCESS_DENIED);
    return false;
}

// RAII reservation that serializes operations on a single relative path. Used
// by CopyUpFile / CopyUpMetadataOnly / CompleteLazyCopyUp so that two threads
// touching the same path can't both stage work and commit on top of each
// other. Constructor blocks until no other thread holds the reservation for
// `path`; destructor releases it and wakes waiters. The waiters then re-check
// their fast-path predicate (ExistsInUpper, !metacopy) and short-circuit.
class PathReservation {
public:
    PathReservation(std::mutex& m,
                    std::condition_variable& cv,
                    std::unordered_set<std::wstring>& s,
                    std::wstring p)
        : m_(m), cv_(cv), s_(s), p_(std::move(p)) {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&]() { return s_.find(p_) == s_.end(); });
        s_.insert(p_);
    }
    ~PathReservation() {
        {
            std::lock_guard<std::mutex> lock(m_);
            s_.erase(p_);
        }
        cv_.notify_all();
    }
    PathReservation(const PathReservation&) = delete;
    PathReservation& operator=(const PathReservation&) = delete;
private:
    std::mutex& m_;
    std::condition_variable& cv_;
    std::unordered_set<std::wstring>& s_;
    std::wstring p_;
};

} // namespace

namespace LayerMount {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

CopyUp::CopyUp(const LayerConfig& config,
               PathResolver& pathResolver,
               WhiteoutManager& whiteoutMgr,
               Cache& cache,
               LayerMountStats& stats)
    : config_(config)
    , pathResolver_(pathResolver)
    , whiteoutMgr_(whiteoutMgr)
    , cache_(cache)
    , stats_(stats) {
    // Enable the privileges copy-up needs to (a) place reparse-point data
    // for symlinks/junctions and (b) bypass user-level ACL denies on a
    // freshly-copied-up parent directory when committing a child file.
    // Token-level enable is sticky and process-wide; the std::call_once
    // guard makes this a single, idempotent setup regardless of how many
    // CopyUp instances the test or service constructs.
    EnsureCopyUpPrivileges();
}

bool CopyUp::IsSecurityPrivAvailable() {
    // Called frequently from GetSecurity paths — the underlying once_flag
    // ensures this is a cheap load after first call.
    return IsSecurityPrivHeld();
}

void CopyUp::RecordCopyUp(const std::wstring& relativePath) {
    stats_.copyUpCount.fetch_add(1, std::memory_order_relaxed);
    if (events_ != nullptr) {
        events_->Emit(LM_EVT_COPY_UP, S_OK, relativePath.c_str(), nullptr);
    }
}

// ---------------------------------------------------------------------------
// 3.1 — Work directory management
// ---------------------------------------------------------------------------

std::wstring CopyUp::GenerateWorkPath() {
    uint64_t counter = workCounter_.fetch_add(1, std::memory_order_relaxed);
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t timestamp = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;

    // The temp suffix "#pid.tid.counter.timestamp.tmp" tops out at ~70 chars.
    // The previous fixed-MAX_PATH buffer would silently truncate when
    // workDirPath approached 200+ characters (deep deployment roots, NT
    // \\?\ prefix paths, paths under non-default profile dirs). Truncation
    // means two concurrent calls can produce IDENTICAL "unique" paths, and
    // CommitFromWorkDir's CREATE_NEW silently fails or worse — a winner
    // overwrites a loser's staged file mid-flight. std::wstring sizing
    // grows as needed and side-steps the buffer-overflow path entirely.
    wchar_t suffix[96];
    swprintf_s(suffix, L"\\#%lu.%lu.%llu.%llu.tmp",
               pid, tid, counter, timestamp);
    return config_.workDirPath + suffix;
}

void CopyUp::CleanWorkDirectory() {
    std::wstring searchPath = config_.workDirPath + L"\\#*.tmp";
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        std::wstring filePath = config_.workDirPath + L"\\" + findData.cFileName;
        DeleteFileW(filePath.c_str());
    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);
}

NTSTATUS CopyUp::CommitFromWorkDir(const std::wstring& workPath,
                                    const std::wstring& finalUpperPath) {
    // Ensure parent directory exists
    std::filesystem::path parentDir = std::filesystem::path(finalUpperPath).parent_path();
    if (!parentDir.empty()) {
        EnsureDirectoryExists(parentDir.wstring());
    }

    // MOVEFILE_COPY_ALLOWED lets cross-volume work_dir layouts succeed via
    // copy+delete fallback. Same-volume layouts are unaffected (rename is
    // still atomic). Without this flag MoveFileExW returns ERROR_NOT_SAME_DEVICE
    // when work_dir lives on a different volume from upper, breaking copy-up
    // entirely in those configs. Atomicity is best-effort cross-volume — the
    // copy is not crash-safe — but functional correctness wins over a hard
    // failure.
    DWORD flags = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH |
                  MOVEFILE_COPY_ALLOWED;
    if (MoveFileExW(workPath.c_str(), finalUpperPath.c_str(), flags)) {
        return STATUS_SUCCESS;
    }

    const DWORD err = GetLastError();
    if (err != ERROR_ACCESS_DENIED) {
        DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err);
    }

    // Fallback path for restrictive parent ACLs. When CopyUpDirectory copied
    // the lower parent's DACL up (e.g. an inherited DENY-WRITE for Everyone),
    // upper\<parent> ends up denying WRITE to our own process — even though
    // we created and own that directory. MoveFileExW then fails at the
    // destination's ACL check with ERROR_ACCESS_DENIED.
    //
    // SE_RESTORE_NAME (enabled in EnsureCopyUpPrivileges) lets a backup-
    // semantics-opened source handle perform a rename via FileRenameInfo
    // that bypasses the destination directory's DACL. This is the same
    // mechanism backup/restore tools use to write into protected paths.
    ScopedHandle src(CreateFileW(workPath.c_str(),
                                  GENERIC_READ | DELETE | SYNCHRONIZE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!src.IsValid()) {
        const DWORD openErr = GetLastError();
        DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(openErr);
    }

    // FILE_RENAME_INFO is a variable-length struct: a fixed header plus the
    // destination path as UTF-16 (byte length in FileNameLength, NOT
    // null-terminated). Allocate one contiguous buffer so the kernel can
    // walk it without a separate allocation.
    const size_t pathBytes = finalUpperPath.size() * sizeof(wchar_t);
    std::vector<BYTE> buf(sizeof(FILE_RENAME_INFO) + pathBytes);
    auto* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
    ri->ReplaceIfExists = TRUE;
    ri->RootDirectory = nullptr;
    ri->FileNameLength = static_cast<DWORD>(pathBytes);
    memcpy(ri->FileName, finalUpperPath.data(), pathBytes);

    if (!SetFileInformationByHandle(src.Get(), FileRenameInfo, ri,
                                      static_cast<DWORD>(buf.size()))) {
        const DWORD renameErr = GetLastError();
        src.Reset();
        DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(renameErr);
    }

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// 3.2 — Full copy-up
// ---------------------------------------------------------------------------

// Copy a reparse-point entry (symlink / junction) from lower to upper,
// preserving the reparse tag + data rather than following the link.
static NTSTATUS CopyUpReparsePointEntry(const std::wstring& srcAbsolute,
                                        const std::wstring& dstAbsolute,
                                        DWORD srcAttrs) {
    // Open source with reparse-point semantics to see the link itself.
    HANDLE srcH = CreateFileW(
        srcAbsolute.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (srcH == INVALID_HANDLE_VALUE) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    // Reparse buffer can be up to MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 KiB).
    BYTE reparseBuf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE]{};
    DWORD returned = 0;
    if (!DeviceIoControl(srcH, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                          reparseBuf, sizeof(reparseBuf), &returned, nullptr)) {
        DWORD err = GetLastError();
        CloseHandle(srcH);
        return ::LayerMount::NtStatusFromWin32(err);
    }
    CloseHandle(srcH);

    // Create destination as the matching type. For directory reparse points
    // (junctions, dir symlinks) the destination is a directory; for file
    // symlinks, an empty file.
    const bool isDir = (srcAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (isDir) {
        if (!CreateDirectoryW(dstAbsolute.c_str(), nullptr)) {
            DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                return ::LayerMount::NtStatusFromWin32(err);
            }
        }
    } else {
        HANDLE dstCreate = CreateFileW(dstAbsolute.c_str(), GENERIC_WRITE, 0,
                                         nullptr, CREATE_ALWAYS,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (dstCreate == INVALID_HANDLE_VALUE) {
            return ::LayerMount::NtStatusFromWin32(GetLastError());
        }
        CloseHandle(dstCreate);
    }

    // Apply the reparse data to the new entry.
    HANDLE dstH = CreateFileW(
        dstAbsolute.c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (dstH == INVALID_HANDLE_VALUE) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    DWORD setReturned = 0;
    if (!DeviceIoControl(dstH, FSCTL_SET_REPARSE_POINT, reparseBuf, returned,
                          nullptr, 0, &setReturned, nullptr)) {
        DWORD err = GetLastError();
        CloseHandle(dstH);
        return ::LayerMount::NtStatusFromWin32(err);
    }
    CloseHandle(dstH);
    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::CopyUpFile(const std::wstring& relativePath) {
    std::wstring normalized = NormalizePath(relativePath);

    // Serialize concurrent copy-ups to the same path. The reservation blocks
    // any second thread until ours completes; on wake-up the second thread
    // re-checks ExistsInUpper below and short-circuits to STATUS_SUCCESS
    // because our commit has already landed. Without serialization, racers
    // would all fight at MoveFileExW commit time (losers see ACCESS_DENIED
    // or sharing violations from the just-placed target).
    PathReservation reservation(copyUpMutex_, copyUpCV_, inFlightCopyUps_,
                                normalized);

    // Check if already in upper layer (could have been copied by concurrent thread)
    if (pathResolver_.ExistsInUpper(normalized)) {
        return STATUS_SUCCESS;
    }

    // Resolve source in lower layers
    ResolvedPath source = pathResolver_.ResolveLowerPath(normalized);
    if (!source.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    // Ensure parent directories exist in upper layer
    NTSTATUS status = EnsureParentDirectories(normalized);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Reparse-point short-circuit: if the source carries a reparse tag, we
    // want to carry the TAG up (preserving symlink/junction semantics), not
    // copy the data behind the link. Regular file-data copy would follow the
    // reparse point on Windows and land an opaque file full of target data.
    if ((source.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        std::wstring upperPath = pathResolver_.GetUpperPath(normalized);
        NTSTATUS reparseStatus =
            CopyUpReparsePointEntry(source.absolutePath, upperPath, source.attributes);
        if (!NT_SUCCESS(reparseStatus)) {
            return reparseStatus;
        }

        // Write bookkeeping metadata + cache invalidation + stats, mirroring
        // the regular copy-up flow's tail. Timestamps/attrs are inherited by
        // the link entry when FSCTL_SET_REPARSE_POINT lands. Metadata
        // persistence is part of the copy-up transaction: without a valid
        // origin/stable-id record, later resolution treats the reparse as a
        // foreign creation, breaking rename-fanout and stable-file-id
        // reporting. On failure, tear down the staged reparse point so the
        // caller can retry from a clean state.
        LayerMountMetadata metadata = MakeCopyUpMetadata(source.absolutePath);
        if (!MetadataADS::WriteLayerMountMetadata(upperPath, metadata, &config_)) {
            const DWORD err = ::GetLastError();
            ::DeleteFileW(upperPath.c_str());
            return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
        }

        cache_.InvalidateWithAncestors(normalized);
        RecordCopyUp(normalized);

        return STATUS_SUCCESS;
    }

    // Open source file
    ScopedHandle srcHandle(CreateFileW(
        source.absolutePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr));

    if (!srcHandle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    // Capture source attributes and timestamps once — re-applied at the very end
    // so neither the work-dir create (which uses FILE_ATTRIBUTE_NORMAL) nor the
    // ADS write (which updates NTFS LastWriteTime) can corrupt them.
    DWORD srcAttrs = GetFileAttributesW(source.absolutePath.c_str());
    FILETIME srcCreation = {}, srcAccess = {}, srcWrite = {};
    GetFileTime(srcHandle.Get(), &srcCreation, &srcAccess, &srcWrite);

    // Generate work path and create temp file
    std::wstring workPath = GenerateWorkPath();
    ScopedHandle dstHandle(CreateFileW(
        workPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));

    if (!dstHandle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    // If the source is a sparse file, mark the destination sparse BEFORE
    // writing data. Sparse state is a file-layout property, not something
    // SetFileAttributes can fix after the fact — the FSCTL must run on the
    // handle while the file is still empty.
    if (srcAttrs != INVALID_FILE_ATTRIBUTES &&
        (srcAttrs & FILE_ATTRIBUTE_SPARSE_FILE) != 0) {
        DWORD bytesReturned = 0;
        FILE_SET_SPARSE_BUFFER sparseBuf{TRUE};
        DeviceIoControl(dstHandle.Get(), FSCTL_SET_SPARSE, &sparseBuf,
                        sizeof(sparseBuf), nullptr, 0, &bytesReturned, nullptr);
    }

    // Propagate NTFS compression. Like sparse, compression is a file-layout
    // property and must be applied BEFORE data is written so the written
    // bytes get compressed in place. Without this, a compressed lower file
    // that occupies e.g. 2 MB expands to 10 MB in upper after copy-up —
    // silent storage inflation for every layered modification.
    if (srcAttrs != INVALID_FILE_ATTRIBUTES &&
        (srcAttrs & FILE_ATTRIBUTE_COMPRESSED) != 0) {
        DWORD bytesReturned = 0;
        USHORT cmpFormat = COMPRESSION_FORMAT_DEFAULT;
        DeviceIoControl(dstHandle.Get(), FSCTL_SET_COMPRESSION,
                        &cmpFormat, sizeof(cmpFormat),
                        nullptr, 0, &bytesReturned, nullptr);
    }

    // Copy file data
    status = CopyFileData(srcHandle.Get(), dstHandle.Get());
    if (!NT_SUCCESS(status)) {
        dstHandle.Reset();
        DeleteFileW(workPath.c_str());
        return status;
    }

    // Close handles before commit (MoveFileEx needs exclusive access)
    srcHandle.Reset();
    dstHandle.Reset();

    if (!ApplyEncryptedStateIfNeeded(workPath, srcAttrs)) {
        DWORD err = ::GetLastError();
        ::DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    // Copy security descriptor (path-based, handles already closed).
    // Security-descriptor failures are fatal: committing with inherited or
    // default DACL can broaden access relative to the source, silently
    // changing access-control semantics after the first write. Tear down
    // the staged work-dir copy so the caller retries from a clean state.
    if (!CopySecurityDescriptor(source.absolutePath, workPath)) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    // Atomic commit from work dir to upper layer
    std::wstring upperPath = pathResolver_.GetUpperPath(normalized);
    status = CommitFromWorkDir(workPath, upperPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Copy user alternate data streams (zone.identifier, custom metadata, etc.)
    // Must run BEFORE WriteLayerMountMetadata so the overlay's own :overlay stream
    // is authoritative and not overwritten by whatever the lower file had.
    // ADS failures are fatal: if Zone.Identifier or an app-specific stream
    // can't be carried up, the upper file would silently lose security or
    // app metadata that the source had — better to fail the copy-up so the
    // caller can retry or report than to commit a half-faithful copy. Tear
    // down the just-committed upper file so a retry starts fresh.
    if (!CopyUserAlternateDataStreams(source.absolutePath, upperPath)) {
        DWORD adsErr = ::GetLastError();
        if (adsErr == 0) adsErr = ERROR_INVALID_DATA;
        ::DeleteFileW(upperPath.c_str());
        return ::LayerMount::NtStatusFromWin32(adsErr);
    }

    // Write ADS metadata. Failure here is fatal: the overlay's authoritative
    // copy-up fingerprint (origin/stable-id/metacopy markers) drives later
    // resolution; without it the upper file looks like a foreign creation and
    // lazy copy-up or rename fanout can misbehave.
    LayerMountMetadata metadata = MakeCopyUpMetadata(source.absolutePath, srcHandle.Get());
    if (!MetadataADS::WriteLayerMountMetadata(upperPath, metadata, &config_)) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(upperPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
    }

    // Re-apply captured attributes + timestamps through a single handle
    // opened with FILE_FLAG_BACKUP_SEMANTICS. Path-based SetFileAttributesW
    // and a plain CreateFileW(FILE_WRITE_ATTRIBUTES) both do DACL checks,
    // so a lower file that inherited a DENY-WRITE from its parent would
    // block restoration even though EnsureCopyUpPrivileges already enabled
    // SE_BACKUP_NAME / SE_RESTORE_NAME. Backup semantics honor those
    // privileges and bypass the DACL check, matching the ADS copy helpers
    // above. SetFileInformationByHandle(FileBasicInfo) writes the attribute
    // bits and all four timestamps atomically.
    if (srcAttrs != INVALID_FILE_ATTRIBUTES) {
        ScopedHandle mdHandle(CreateFileW(
            upperPath.c_str(),
            FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!mdHandle.IsValid()) {
            const DWORD err = ::GetLastError();
            ::DeleteFileW(upperPath.c_str());
            return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
        }
        FILE_BASIC_INFO basic{};
        basic.CreationTime.LowPart    = srcCreation.dwLowDateTime;
        basic.CreationTime.HighPart   = static_cast<LONG>(srcCreation.dwHighDateTime);
        basic.LastAccessTime.LowPart  = srcAccess.dwLowDateTime;
        basic.LastAccessTime.HighPart = static_cast<LONG>(srcAccess.dwHighDateTime);
        basic.LastWriteTime.LowPart   = srcWrite.dwLowDateTime;
        basic.LastWriteTime.HighPart  = static_cast<LONG>(srcWrite.dwHighDateTime);
        // -1 is the documented "don't change" sentinel for FILE_BASIC_INFO
        // time fields. Zero would write the Windows epoch (1601-01-01) and
        // SetFileInformationByHandle rejects the whole call as invalid.
        basic.ChangeTime.QuadPart     = -1;
        basic.FileAttributes          = srcAttrs;
        if (!SetFileInformationByHandle(mdHandle.Get(), FileBasicInfo,
                                        &basic, sizeof(basic))) {
            const DWORD err = ::GetLastError();
            mdHandle.Reset();
            ::DeleteFileW(upperPath.c_str());
            return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
        }
    } else {
        // No captured attributes — still need to apply timestamps.
        ScopedHandle tsHandle(CreateFileW(
            upperPath.c_str(),
            FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!tsHandle.IsValid()) {
            const DWORD err = ::GetLastError();
            ::DeleteFileW(upperPath.c_str());
            return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
        }
        if (!SetFileTime(tsHandle.Get(), &srcCreation, &srcAccess, &srcWrite)) {
            const DWORD err = ::GetLastError();
            tsHandle.Reset();
            ::DeleteFileW(upperPath.c_str());
            return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
        }
    }

    // Invalidate cache
    cache_.InvalidateWithAncestors(normalized);

    // Update stats
    RecordCopyUp(normalized);

    // Reservation released by RAII at scope exit; waiters then re-check
    // ExistsInUpper and short-circuit with SUCCESS — our commit is done.
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// 3.3 — Lazy/metadata-only copy-up
// ---------------------------------------------------------------------------

NTSTATUS CopyUp::CopyUpMetadataOnly(const std::wstring& relativePath) {
    std::wstring normalized = NormalizePath(relativePath);

    // Fast-path: already committed before we even check the reservation.
    if (pathResolver_.ExistsInUpper(normalized)) {
        return STATUS_SUCCESS;
    }

    // Serialize concurrent metacopy stages on the same path. Without this,
    // two threads can both stage a sparse shell into the work dir and race
    // at MoveFileExW commit — the loser sees ACCESS_DENIED and leaves an
    // orphaned work file behind, while the winner's metacopy can later be
    // overwritten by an interleaved second commit. Same invariant CopyUpFile
    // enforces.
    PathReservation reservation(copyUpMutex_, copyUpCV_, inFlightCopyUps_,
                                normalized);

    // Re-check after acquiring the reservation — a winner may have committed
    // while we waited, in which case there's nothing to do.
    if (pathResolver_.ExistsInUpper(normalized)) {
        return STATUS_SUCCESS;
    }

    ResolvedPath source = pathResolver_.ResolveLowerPath(normalized);
    if (!source.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    NTSTATUS status = EnsureParentDirectories(normalized);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Get source file info for size and timestamps
    WIN32_FILE_ATTRIBUTE_DATA srcAttrs;
    if (!GetFileAttributesExW(source.absolutePath.c_str(), GetFileExInfoStandard, &srcAttrs)) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    // Create sparse file in work directory
    std::wstring workPath = GenerateWorkPath();
    ScopedHandle dstHandle(CreateFileW(
        workPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        srcAttrs.dwFileAttributes,
        nullptr));

    if (!dstHandle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    // Mark as sparse. Sparse is load-bearing for metacopy: the placeholder
    // allocates no data blocks and the overlay's lazy-copy-up semantics depend
    // on it. If it fails we must abort -- committing a non-sparse placeholder
    // would inflate the upper volume and break the metacopy contract.
    DWORD bytesReturned;
    if (!DeviceIoControl(dstHandle.Get(), FSCTL_SET_SPARSE, nullptr, 0,
                         nullptr, 0, &bytesReturned, nullptr)) {
        DWORD err = ::GetLastError();
        dstHandle.Reset();
        ::DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    // NTFS compression is a per-file FSCTL, not a CreateFileW attribute flag.
    // srcAttrs.dwFileAttributes passed at CreateFileW above is silently
    // ignored for FILE_ATTRIBUTE_COMPRESSED. Apply it explicitly here so a
    // compressed lower file doesn't balloon uncompressed in upper. Order
    // matters: compression must be set BEFORE SetEndOfFile and any data
    // writes (same invariant CopyFilePreservingMetadata relies on). A
    // failure here also aborts -- without compression the metacopy shell
    // would diverge from source in allocation semantics.
    if ((srcAttrs.dwFileAttributes & FILE_ATTRIBUTE_COMPRESSED) != 0) {
        USHORT cmpFormat = COMPRESSION_FORMAT_DEFAULT;
        if (!DeviceIoControl(dstHandle.Get(), FSCTL_SET_COMPRESSION,
                             &cmpFormat, sizeof(cmpFormat),
                             nullptr, 0, &bytesReturned, nullptr)) {
            DWORD err = ::GetLastError();
            dstHandle.Reset();
            ::DeleteFileW(workPath.c_str());
            return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
        }
    }

    // Set file size without allocating disk space
    LARGE_INTEGER fileSize;
    fileSize.LowPart = srcAttrs.nFileSizeLow;
    fileSize.HighPart = static_cast<LONG>(srcAttrs.nFileSizeHigh);
    SetFilePointerEx(dstHandle.Get(), fileSize, nullptr, FILE_BEGIN);
    SetEndOfFile(dstHandle.Get());

    dstHandle.Reset();

    if (!ApplyEncryptedStateIfNeeded(workPath, srcAttrs.dwFileAttributes)) {
        DWORD err = ::GetLastError();
        ::DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    // Copy security descriptor. Fatal on failure -- see CopyUpFile above
    // for the reasoning. Tear down the staged work-dir copy on failure.
    if (!CopySecurityDescriptor(source.absolutePath, workPath)) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(workPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    // Atomic commit
    std::wstring upperPath = pathResolver_.GetUpperPath(normalized);
    status = CommitFromWorkDir(workPath, upperPath);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Write metacopy ADS metadata. Fatal on failure: without the metacopy
    // flag, later reads won't know to complete the lazy copy-up and will
    // serve the zero-filled shell as if it were real content. Tear down
    // the staged upper so the caller can retry cleanly.
    LayerMountMetadata metadata = MakeCopyUpMetadata(source.absolutePath);
    metadata.metacopy = true;
    if (!MetadataADS::WriteLayerMountMetadata(upperPath, metadata, &config_)) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(upperPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
    }

    // Re-apply timestamps LAST — ADS write updates NTFS LastWriteTime, so
    // timestamps must be the final operation to preserve source truth.
    // (Attributes already correctly set by CreateFileW above.)
    {
        ScopedHandle tsHandle(CreateFileW(
            upperPath.c_str(),
            FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr));
        if (tsHandle.IsValid()) {
            SetFileTime(tsHandle.Get(),
                        &srcAttrs.ftCreationTime,
                        &srcAttrs.ftLastAccessTime,
                        &srcAttrs.ftLastWriteTime);
        }
    }

    cache_.InvalidateWithAncestors(normalized);
    RecordCopyUp(normalized);

    return STATUS_SUCCESS;
}

NTSTATUS CopyUp::CompleteLazyCopyUp(const std::wstring& relativePath) {
    std::wstring normalized = NormalizePath(relativePath);
    std::wstring upperPath = pathResolver_.GetUpperPath(normalized);

    // Serialize concurrent completions on the same path. Without this, two
    // handles' SRead/SWrite paths can both enter here and both copy lower
    // bytes into the same upper file. The loser's still-running CopyFileData
    // can clobber a user-write that landed between the two completions, or
    // simply duplicate writes onto the same handle range. Reservation makes
    // the second caller wait, then re-check `metacopy` and short-circuit.
    PathReservation reservation(copyUpMutex_, copyUpCV_, inFlightCopyUps_,
                                normalized);

    // Read metadata AFTER acquiring the reservation. A winner that ran
    // before us has already cleared the metacopy flag and may have applied
    // a user-write into upper; re-copying lower bytes here would clobber
    // that write. The post-lock read guarantees we see the winner's commit.
    LayerMountMetadata metadata = MetadataADS::ReadLayerMountMetadata(upperPath, &config_);
    if (!metadata.metacopy) {
        return STATUS_SUCCESS; // Not a metacopy, nothing to do
    }

    // Open source file from the origin layer
    ScopedHandle srcHandle(CreateFileW(
        metadata.originLayer.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));

    if (!srcHandle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    // Capture source timestamps NOW, before our own writes bump them on
    // upper. CopyUpFile does the same in the eager path so the copy-up
    // looks byte-for-byte equivalent to the source; the lazy path needs
    // the same guarantee or consumers see the completion moment instead
    // of the source write moment.
    FILETIME srcCreation{}, srcAccess{}, srcWrite{};
    GetFileTime(srcHandle.Get(), &srcCreation, &srcAccess, &srcWrite);

    // Open destination (upper layer file) for writing. Share modes must
    // include SHARE_READ | SHARE_WRITE | SHARE_DELETE so a caller that is
    // already holding the file via SOpen (GrantedAccess RW) doesn't deadlock
    // us with a sharing violation when we run inside SRead/SWrite.
    ScopedHandle dstHandle(CreateFileW(
        upperPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr));

    if (!dstHandle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(GetLastError());
    }

    // Seek to beginning of both files
    LARGE_INTEGER zero = {};
    SetFilePointerEx(srcHandle.Get(), zero, nullptr, FILE_BEGIN);
    SetFilePointerEx(dstHandle.Get(), zero, nullptr, FILE_BEGIN);

    // Copy all data
    NTSTATUS status = CopyFileData(srcHandle.Get(), dstHandle.Get());
    if (!NT_SUCCESS(status)) {
        return status;
    }

    srcHandle.Reset();
    dstHandle.Reset();

    // Carry user alternate data streams (zone.identifier, custom metadata)
    // from the lower file up to the upper. The eager copy-up (CopyUpFile)
    // does this right after commit; the lazy path has to do it here because
    // CopyUpMetadataOnly only staged a sparse data shell without streams.
    // Run BEFORE WriteLayerMountMetadata so the overlay's own :overlay stream
    // stays authoritative.
    //
    // ADS failures are fatal: if a stream can't be carried up, leave the
    // metacopy flag set and surface the error so the caller can retry.
    // Clearing metacopy with a partial ADS set would commit a corrupted
    // upper that silently misses a stream the source had.
    if (!CopyUserAlternateDataStreams(metadata.originLayer, upperPath)) {
        DWORD adsErr = ::GetLastError();
        if (adsErr == 0) adsErr = ERROR_INVALID_DATA;
        return ::LayerMount::NtStatusFromWin32(adsErr);
    }

    // Clear metacopy flag. Metadata persistence is the atomic commit point
    // for the completion: if it fails, the upper file's data is in place
    // but the metacopy flag is still set (we haven't re-entered ADS yet),
    // so subsequent resolutions will attempt the completion again. That
    // is the correct retry shape. Do NOT delete upperPath here -- the
    // data has been copied, and another handle may already be holding it
    // open; tearing it down would clobber user writes that may have
    // landed between CopyFileData and here. Surface the failure so the
    // caller sees the completion did not commit.
    metadata.metacopy = false;
    if (!MetadataADS::WriteLayerMountMetadata(upperPath, metadata, &config_)) {
        const DWORD err = ::GetLastError();
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
    }

    // Re-apply captured source timestamps LAST — both CopyFileData and
    // WriteLayerMountMetadata bump NTFS LastWriteTime on their respective
    // streams. Without this, a consumer that indexes by mtime would see
    // the completion-moment stamp instead of the lower file's truth.
    {
        ScopedHandle tsHandle(CreateFileW(
            upperPath.c_str(),
            FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr));
        if (tsHandle.IsValid()) {
            SetFileTime(tsHandle.Get(), &srcCreation, &srcAccess, &srcWrite);
        }
    }

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// 3.4 — Directory copy-up
// ---------------------------------------------------------------------------

NTSTATUS CopyUp::CopyUpDirectory(const std::wstring& relativePath) {
    std::wstring normalized = NormalizePath(relativePath);

    if (pathResolver_.ExistsInUpper(normalized)) {
        return STATUS_SUCCESS;
    }

    ResolvedPath source = pathResolver_.ResolveLowerPath(normalized);
    if (!source.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    // Ensure parent directories
    NTSTATUS status = EnsureParentDirectories(normalized);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Reparse-point short-circuit (mirror CopyUpFile's symlink handling for
    // directory reparses — junctions / dir-symlinks). Without this branch a
    // lower junction copies up as a plain empty directory: the reparse tag
    // is silently dropped, FSCTL_GET_REPARSE_POINT on the upper entry
    // returns nothing, and any client expecting the upper to behave as a
    // junction (or attempting FSCTL_DELETE_REPARSE_POINT on it) sees
    // ERROR_NOT_A_REPARSE_POINT. The fix is to route directory-reparse
    // sources through CopyUpReparsePointEntry, which preserves the tag +
    // reparse data verbatim.
    if ((source.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        std::wstring upperPath = pathResolver_.GetUpperPath(normalized);
        NTSTATUS reparseStatus =
            CopyUpReparsePointEntry(source.absolutePath, upperPath, source.attributes);
        if (!NT_SUCCESS(reparseStatus)) {
            return reparseStatus;
        }

        // Metadata persistence is part of the copy-up transaction (see
        // the file-reparse branch above). On failure, remove the staged
        // reparse directory so the caller retries from a clean state.
        LayerMountMetadata metadata = MakeCopyUpMetadata(source.absolutePath);
        if (!MetadataADS::WriteLayerMountMetadata(upperPath, metadata, &config_)) {
            const DWORD err = ::GetLastError();
            ::RemoveDirectoryW(upperPath.c_str());
            return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
        }

        cache_.InvalidateWithAncestors(normalized);
        RecordCopyUp(normalized);
        return STATUS_SUCCESS;
    }

    // Capture source directory attributes and timestamps once — re-applied at
    // the very end so neither CreateDirectoryW (which doesn't take attrs) nor
    // the ADS write (which updates NTFS LastWriteTime) can corrupt them.
    DWORD srcAttrs = GetFileAttributesW(source.absolutePath.c_str());
    FILETIME srcCreation = {}, srcAccess = {}, srcWrite = {};
    {
        ScopedHandle srcHandle(CreateFileW(
            source.absolutePath.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            nullptr));
        if (srcHandle.IsValid()) {
            GetFileTime(srcHandle.Get(), &srcCreation, &srcAccess, &srcWrite);
        }
    }

    // Create directory in upper layer (no work-dir atomic rename for dirs)
    std::wstring upperPath = pathResolver_.GetUpperPath(normalized);
    if (!CreateDirectoryW(upperPath.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            return ::LayerMount::NtStatusFromWin32(err);
        }
    }

    // Propagate NTFS compression on the directory. NTFS dirs can carry the
    // COMPRESSED attribute, which sets the default layout for NEW children
    // created inside them. Without propagation, files created into a lower-
    // compressed directory via the mount land uncompressed in upper.
    if (srcAttrs != INVALID_FILE_ATTRIBUTES &&
        (srcAttrs & FILE_ATTRIBUTE_COMPRESSED) != 0) {
        HANDLE cmpH = CreateFileW(upperPath.c_str(),
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (cmpH != INVALID_HANDLE_VALUE) {
            DWORD bytesReturned = 0;
            USHORT cmpFormat = COMPRESSION_FORMAT_DEFAULT;
            DeviceIoControl(cmpH, FSCTL_SET_COMPRESSION,
                            &cmpFormat, sizeof(cmpFormat),
                            nullptr, 0, &bytesReturned, nullptr);
            CloseHandle(cmpH);
        }
    }

    if (!ApplyEncryptedStateIfNeeded(upperPath, srcAttrs)) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }

    // Copy security descriptor. Fatal on failure: the directory's DACL
    // is also the template for auto-inheritance onto children created
    // inside it, so silently dropping the source's DACL would broaden
    // or narrow access on every subsequently created child. Tear down
    // the staged upper directory so the caller retries from a clean
    // state.
    if (!CopySecurityDescriptor(source.absolutePath, upperPath)) {
        const DWORD err = ::GetLastError();
        ::RemoveDirectoryW(upperPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_ACCESS_DENIED);
    }

    // Write ADS metadata. Fatal on failure: without origin/stable-id
    // the upper directory looks like a foreign creation and later
    // resolution can misbehave. Tear down the staged upper directory.
    LayerMountMetadata metadata = MakeCopyUpMetadata(source.absolutePath);
    if (!MetadataADS::WriteLayerMountMetadata(upperPath, metadata, &config_)) {
        const DWORD err = ::GetLastError();
        ::RemoveDirectoryW(upperPath.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
    }

    // Re-apply captured attributes (CreateDirectoryW doesn't copy source attrs).
    if (srcAttrs != INVALID_FILE_ATTRIBUTES) {
        SetFileAttributesW(upperPath.c_str(), srcAttrs);
    }

    // Re-apply captured timestamps LAST (ADS write updates NTFS LastWriteTime).
    {
        ScopedHandle tsHandle(CreateFileW(
            upperPath.c_str(),
            FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (tsHandle.IsValid()) {
            SetFileTime(tsHandle.Get(), &srcCreation, &srcAccess, &srcWrite);
        }
    }

    cache_.InvalidateWithAncestors(normalized);
    RecordCopyUp(normalized);

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// 3.5 — Directory rename redirect
// ---------------------------------------------------------------------------

NTSTATUS CopyUp::HandleDirectoryRename(const std::wstring& oldRelativePath,
                                        const std::wstring& newRelativePath,
                                        bool sourceIsInLower,
                                        bool replaceIfExists) {
    std::wstring oldNorm = NormalizePath(oldRelativePath);
    std::wstring newNorm = NormalizePath(newRelativePath);
    std::wstring newUpperPath = pathResolver_.GetUpperPath(newNorm);

    // Collision guard. Before any mutation we must decide whether the
    // destination already exists in the merged view. If it does, a rename
    // without replaceIfExists is a failure — NOT a silent merge (lower→dst)
    // or overwrite (upper→dst). Historical behaviour ignored this flag
    // entirely; integration tests and Win32 callers that rely on it (e.g.
    // git, MSBuild, installers) saw corrupt overlay state on collision.
    if (!replaceIfExists) {
        const bool destInUpper = pathResolver_.ExistsInUpper(newNorm);
        const bool destWhitedOut =
            whiteoutMgr_.HasWhiteout(newNorm, config_.upperPath);
        const bool destInLower =
            pathResolver_.ResolveLowerPath(newNorm).Found();
        // A whiteout in upper hides the lower entry, so the destination
        // does NOT exist in the merged view even if lower has content.
        const bool destExistsInMerged =
            destInUpper || (destInLower && !destWhitedOut);
        if (destExistsInMerged) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }

    if (sourceIsInLower) {
        // Resolve source in lower layers
        ResolvedPath source = pathResolver_.ResolveLowerPath(oldNorm);
        if (!source.Found()) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }

        // Ensure parent of new path exists
        EnsureDirectoryExists(std::filesystem::path(newUpperPath).parent_path().wstring());

        // Reparse-point short-circuit (junction / directory-symlink source).
        // The opacity + recursive-merge dance below is meaningless for a
        // reparse object — it has no overlay-shadow children, and SetOpaque
        // on a junction would write a marker file INSIDE the link, which
        // follows through to the external target and mutates state outside
        // the overlay. Just copy the reparse entry, drop any prior upper
        // shadow at the old path, and create the whiteout.
        //
        // Capability gate: an upper layer that
        // doesn't support reparse points cannot HOLD a junction/symlink at
        // all. Skip the short-circuit and fall through to the recursive
        // CopyTreePreservingMetadata branch -- that copies the link's
        // *target* contents into a regular directory, losing the link
        // semantics but preserving the data. Emit one LM_EVT_WARNING per
        // affected rename so observability tooling can surface the
        // degradation.
        if ((source.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
            capabilities_.HasReparsePoints()) {
            NTSTATUS rpStatus = CopyUpReparsePointEntry(
                source.absolutePath, newUpperPath, source.attributes);
            if (!NT_SUCCESS(rpStatus)) {
                return rpStatus;
            }

            // Remove the upper shadow at the old path if one was previously
            // copied up (e.g. from the rename's own SOpen). For a reparse
            // entry, RemoveDirectoryW on the link removes just the link
            // object (kernel-level), not the external target.
            const std::wstring oldUpperPath = pathResolver_.GetUpperPath(oldNorm);
            const DWORD oldAttrs = ::GetFileAttributesW(oldUpperPath.c_str());
            if (oldAttrs != INVALID_FILE_ATTRIBUTES) {
                if ((oldAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    ::RemoveDirectoryW(oldUpperPath.c_str());
                } else {
                    ::DeleteFileW(oldUpperPath.c_str());
                }
            }

            // Without the old-path whiteout, the lower-layer directory will
            // resurface after the rename — callers saw success but the tree
            // reappears. Surface the failure instead of silently completing.
            if (!whiteoutMgr_.CreateWhiteout(oldNorm, WhiteoutType::Directory)) {
                const DWORD whErr = ::GetLastError();
                cache_.InvalidateWithAncestors(oldNorm);
                cache_.InvalidateWithAncestors(newNorm);
                return whErr ? ::LayerMount::NtStatusFromWin32(whErr)
                             : STATUS_ACCESS_DENIED;
            }
            cache_.InvalidateWithAncestors(oldNorm);
            cache_.InvalidateWithAncestors(newNorm);
            return STATUS_SUCCESS;
        }

        // If the source is a reparse point but the host's upper layer
        // doesn't support reparse points, we just fell through here from
        // the gated short-circuit above. Surface the degradation so
        // hosts can log/audit it.
        if ((source.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
            !capabilities_.HasReparsePoints() && events_ != nullptr) {
            events_->Emit(LM_EVT_WARNING, S_OK, oldNorm.c_str(),
                L"Directory rename of a reparse-point source forced full "
                L"recursive copy-up: upper layer lacks LM_CAP_REPARSE_POINTS, "
                L"link semantics will not be preserved.");
        }

        // Recursive copy from lower to new upper location. Use a custom walker
        // so reparse points (symlinks/junctions) and sparse files preserve
        // their tags/sparsity — std::filesystem::copy follows symlinks and
        // copies sparse files as dense, which silently drops tree fidelity.
        NTSTATUS copyStatus = CopyTreePreservingMetadata(
            source.absolutePath, newUpperPath);
        if (!NT_SUCCESS(copyStatus)) {
            // Tear down the half-built destination. CopyTree now surfaces
            // real child failures (file-over-dir collision, ACL denial,
            // I/O error); leaving partial state at newUpperPath would
            // poison merged-view reads even though the rename returned
            // failure to the caller.
            std::error_code ec;
            std::filesystem::remove_all(newUpperPath, ec);
            return copyStatus;
        }

        // LayerMount the upper shadow (if any) on top of the lower copy so any
        // pre-existing overlay state carries through to the renamed path.
        // Without this, `upper/src/.wh.child` stays behind at the old path
        // while `upper/dst` shows `child` resurrected from lower.
        //
        // Two special child kinds need careful handling:
        //   - ".wh.<name>" whiteouts: instead of copying the whiteout file
        //     verbatim (which would leave an inconsistent upper state where
        //     both `<name>` and `.wh.<name>` coexist), delete the matching
        //     `<name>` from the destination. The destination is opaque after
        //     the rename (SetOpaque below), so the whiteout marker itself is
        //     redundant and we don't need to carry it over.
        //   - ".wh..wh..opq" opaque marker + the overlay's :overlay.opaque ADS:
        //     we re-apply opacity via SetOpaque(newNorm) ourselves, so skip
        //     the marker to avoid double bookkeeping.
        //
        // Non-whiteout upper content (a real shadow file that was written in
        // upper and thus shadows the lower) is just overwritten on top of the
        // lower copy — that's the existing "upper wins" semantic.
        std::wstring oldUpperPath = pathResolver_.GetUpperPath(oldNorm);
        DWORD oldUpperAttrs = GetFileAttributesW(oldUpperPath.c_str());
        if (oldUpperAttrs != INVALID_FILE_ATTRIBUTES &&
            (oldUpperAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            static constexpr std::wstring_view kWhPrefix(L".wh.");
            static constexpr std::wstring_view kOpqMarker(L".wh..wh..opq");

            WIN32_FIND_DATAW fd{};
            const std::wstring pattern = oldUpperPath + L"\\*";
            HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.cFileName[0] == L'.' &&
                        (fd.cFileName[1] == 0 ||
                         (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;

                    const std::wstring name = fd.cFileName;
                    const std::wstring childSrc = oldUpperPath + L"\\" + name;
                    const std::wstring childDst = newUpperPath + L"\\" + name;

                    // Skip the opaque marker; SetOpaque below handles opacity.
                    if (name == kOpqMarker) continue;

                    // Whiteout: delete the shadowed file from the destination
                    // instead of carrying the marker over.
                    if (name.size() > kWhPrefix.size() &&
                        std::equal(kWhPrefix.begin(), kWhPrefix.end(),
                                    name.begin())) {
                        const std::wstring target =
                            newUpperPath + L"\\" + name.substr(kWhPrefix.size());
                        DWORD a = ::GetFileAttributesW(target.c_str());
                        if (a != INVALID_FILE_ATTRIBUTES) {
                            if (a & FILE_ATTRIBUTE_DIRECTORY) {
                                std::error_code ec;
                                std::filesystem::remove_all(target, ec);
                            } else {
                                ::DeleteFileW(target.c_str());
                            }
                        }
                        continue;
                    }

                    // Real upper shadow file/dir: overlay on top of the lower
                    // copy. If a child copy fails (e.g., file-over-directory
                    // type conflict), the rename cannot complete cleanly —
                    // tear down the half-built destination tree and surface
                    // the error. Previously (void)-cast; that let a rename
                    // "succeed" with corrupt state.
                    NTSTATUS childStatus = STATUS_SUCCESS;
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                        DWORD targetAttrs = ::GetFileAttributesW(childDst.c_str());
                        if (targetAttrs != INVALID_FILE_ATTRIBUTES) {
                            if (targetAttrs & FILE_ATTRIBUTE_DIRECTORY) {
                                std::error_code ec;
                                std::filesystem::remove_all(childDst, ec);
                            } else {
                                ::DeleteFileW(childDst.c_str());
                            }
                        }
                        childStatus = CopyTreePreservingMetadata(childSrc, childDst);
                    } else if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                        childStatus = CopyTreePreservingMetadata(childSrc, childDst);
                    } else {
                        childStatus = CopyTreePreservingMetadata(childSrc, childDst);
                    }

                    if (!NT_SUCCESS(childStatus)) {
                        ::FindClose(hFind);
                        std::error_code ec;
                        std::filesystem::remove_all(newUpperPath, ec);
                        return childStatus;
                    }
                } while (::FindNextFileW(hFind, &fd));
                ::FindClose(hFind);
            }
        }

        // Mark new location as opaque (hide lower layer contents at new path)
        whiteoutMgr_.SetOpaque(newNorm);

        // Tear down the upper shadow at the old path. std::filesystem::remove_all
        // succeeds for an empty or a non-existent directory, which is exactly
        // the "RemoveDirectoryW silently no-ops on a missing path" contract we
        // had before — just now it also handles shadows that contained whiteout
        // files. ADS on the directory itself don't count as contents so this
        // is fine for a plain shallow stub too.
        std::error_code ecRemove;
        std::filesystem::remove_all(oldUpperPath, ecRemove);

        // Create whiteout at old location so the lower subtree stops surfacing.
        // Surface the failure if the marker can't be persisted — otherwise the
        // lower directory tree resurfaces after a "successful" rename.
        if (!whiteoutMgr_.CreateWhiteout(oldNorm, WhiteoutType::Directory)) {
            const DWORD whErr = ::GetLastError();
            cache_.InvalidateWithAncestors(oldNorm);
            cache_.InvalidateWithAncestors(newNorm);
            return whErr ? ::LayerMount::NtStatusFromWin32(whErr)
                         : STATUS_ACCESS_DENIED;
        }
    } else {
        // Renaming within upper layer
        std::wstring oldUpperPath = pathResolver_.GetUpperPath(oldNorm);

        // Check if old location was opaque
        bool wasOpaque = whiteoutMgr_.IsOpaque(oldNorm);

        // Honor replaceIfExists. Previously this path hard-coded
        // MOVEFILE_REPLACE_EXISTING, which silently overwrote a pre-existing
        // destination directory even when the caller requested
        // rename-without-replace. The merged-view collision check above
        // already rejected the no-replace case where the destination
        // exists, so reaching this point with replaceIfExists==false means
        // no destination exists and the flag is a no-op — but it stays
        // consistent with the caller's intent.
        DWORD flags = replaceIfExists ? MOVEFILE_REPLACE_EXISTING : 0;
        if (!MoveFileExW(oldUpperPath.c_str(), newUpperPath.c_str(), flags)) {
            return ::LayerMount::NtStatusFromWin32(GetLastError());
        }

        // Transfer opaque marker
        if (wasOpaque) {
            whiteoutMgr_.RemoveOpaque(oldNorm);
            whiteoutMgr_.SetOpaque(newNorm);
        }
    }

    // Invalidate cache for both paths
    cache_.InvalidateWithAncestors(oldNorm);
    cache_.InvalidateWithAncestors(newNorm);

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Tree copy preserving reparse points, sparse files, and ADS
// ---------------------------------------------------------------------------

namespace {

// Copy a single regular file preserving sparse state + ADS. Mirrors the tail
// half of CopyUpFile but without the work-dir atomic-commit dance, because
// the caller is already operating inside a new upper-layer subtree and
// doesn't need crash-safety against the destination appearing half-formed
// (the whole subtree gets removed on failure at a higher level).
NTSTATUS CopyFilePreservingMetadata(const std::wstring& srcAbs,
                                     const std::wstring& dstAbs,
                                     const LayerConfig* config) {
    HANDLE srcH = ::CreateFileW(srcAbs.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN |
                                      FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if (srcH == INVALID_HANDLE_VALUE) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }

    DWORD srcAttrs = ::GetFileAttributesW(srcAbs.c_str());
    FILETIME ftCreate{}, ftAccess{}, ftWrite{};
    ::GetFileTime(srcH, &ftCreate, &ftAccess, &ftWrite);

    HANDLE dstH = ::CreateFileW(dstAbs.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dstH == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        ::CloseHandle(srcH);
        return ::LayerMount::NtStatusFromWin32(err);
    }

    // Mark sparse BEFORE writing — sparse state is a layout property that
    // can only be set on an empty file.
    if (srcAttrs != INVALID_FILE_ATTRIBUTES &&
        (srcAttrs & FILE_ATTRIBUTE_SPARSE_FILE) != 0) {
        DWORD br = 0;
        FILE_SET_SPARSE_BUFFER sb{TRUE};
        ::DeviceIoControl(dstH, FSCTL_SET_SPARSE, &sb, sizeof(sb),
                          nullptr, 0, &br, nullptr);
    }

    // Same treatment for NTFS compression — must be applied BEFORE data so
    // the writes land compressed. See CopyUp::CopyUpFile for the rationale.
    if (srcAttrs != INVALID_FILE_ATTRIBUTES &&
        (srcAttrs & FILE_ATTRIBUTE_COMPRESSED) != 0) {
        DWORD br = 0;
        USHORT cmpFormat = COMPRESSION_FORMAT_DEFAULT;
        ::DeviceIoControl(dstH, FSCTL_SET_COMPRESSION,
                          &cmpFormat, sizeof(cmpFormat),
                          nullptr, 0, &br, nullptr);
    }

    // Copy data.
    BYTE buf[64 * 1024];
    for (;;) {
        DWORD r = 0;
        if (!::ReadFile(srcH, buf, sizeof(buf), &r, nullptr)) {
            DWORD err = ::GetLastError();
            ::CloseHandle(srcH);
            ::CloseHandle(dstH);
            return ::LayerMount::NtStatusFromWin32(err);
        }
        if (r == 0) break;
        DWORD w = 0;
        if (!::WriteFile(dstH, buf, r, &w, nullptr) || w != r) {
            DWORD err = ::GetLastError();
            ::CloseHandle(srcH);
            ::CloseHandle(dstH);
            return ::LayerMount::NtStatusFromWin32(err);
        }
    }

    ::CloseHandle(srcH);
    ::CloseHandle(dstH);

    if (!ApplyEncryptedStateIfNeeded(dstAbs, srcAttrs)) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }

    // Copy user ADS (file-namespace alternate data streams). Skips ::$DATA
    // and the overlay's own :overlay bookkeeping streams.
    CopyUserAlternateDataStreams(srcAbs, dstAbs);

    // Security propagation: copy only OWNER+GROUP (and preserve any explicit
    // ACEs in the source by merging them via SetNamedSecurityInfoW with the
    // UNPROTECTED flag so the destination also auto-inherits from its parent.
    // Plain SetFileSecurityW with a DACL containing INHERITED_ACE-flagged
    // entries writes them as explicit non-inherited ACEs, which bypasses
    // the kernel's inheritance semantics and leaves children with a
    // snapshot-in-time ACL that can diverge from their parent.
    //
    // The approach:
    //   1. Copy owner + group verbatim (these are identity, not inheritable).
    //   2. Copy DACL via SetNamedSecurityInfoW with UNPROTECTED so Windows
    //      drops the INHERITED bit on the source's inherited ACEs and
    //      re-derives them from the new parent's inheritable ACEs. Explicit
    //      (non-inherited) ACEs from the source are preserved as explicit.
    //   3. Copy SACL (audit ACEs) verbatim iff SE_SECURITY_NAME is held —
    //      otherwise the GetFileSecurityW(SACL) call fails with
    //      ERROR_PRIVILEGE_NOT_HELD and we silently drop audit rules.
    // Treat set-security failures as fatal for this copy path: the
    // destination file has been committed, and silently dropping to default
    // DACL inheritance would broaden or narrow access relative to the source
    // -- a security boundary regression. On failure, delete the destination
    // so the caller can retry from a clean state and return the mapped error.
    {
        SECURITY_INFORMATION ogInfo = OWNER_SECURITY_INFORMATION |
                                        GROUP_SECURITY_INFORMATION;
        DWORD sdSize = 0;
        ::GetFileSecurityW(srcAbs.c_str(), ogInfo, nullptr, 0, &sdSize);
        if (sdSize > 0) {
            std::vector<BYTE> sdBuf(sdSize);
            auto* sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(sdBuf.data());
            if (::GetFileSecurityW(srcAbs.c_str(), ogInfo, sd, sdSize, &sdSize)) {
                if (!::SetFileSecurityW(dstAbs.c_str(), ogInfo, sd)) {
                    DWORD err = ::GetLastError();
                    ::DeleteFileW(dstAbs.c_str());
                    return ::LayerMount::NtStatusFromWin32(
                        err ? err : ERROR_ACCESS_DENIED);
                }
            }
        }
        // DACL: pull it out, filter to explicit ACEs only, apply as
        // UNPROTECTED so parent inheritance still contributes.
        DWORD dSize = 0;
        ::GetFileSecurityW(srcAbs.c_str(), DACL_SECURITY_INFORMATION,
                            nullptr, 0, &dSize);
        if (dSize > 0) {
            std::vector<BYTE> dBuf(dSize);
            auto* dsd = reinterpret_cast<PSECURITY_DESCRIPTOR>(dBuf.data());
            if (::GetFileSecurityW(srcAbs.c_str(), DACL_SECURITY_INFORMATION,
                                      dsd, dSize, &dSize)) {
                BOOL present = FALSE, defaulted = FALSE;
                PACL dacl = nullptr;
                if (::GetSecurityDescriptorDacl(dsd, &present, &dacl, &defaulted) &&
                    present && dacl) {
                    DWORD rc = ::SetNamedSecurityInfoW(
                        const_cast<LPWSTR>(dstAbs.c_str()),
                        SE_FILE_OBJECT,
                        DACL_SECURITY_INFORMATION |
                            UNPROTECTED_DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, dacl, nullptr);
                    if (rc != ERROR_SUCCESS) {
                        ::DeleteFileW(dstAbs.c_str());
                        return ::LayerMount::NtStatusFromWin32(rc);
                    }
                }
            }
        }
        if (IsSecurityPrivHeld()) {
            DWORD ssSize = 0;
            ::GetFileSecurityW(srcAbs.c_str(), SACL_SECURITY_INFORMATION,
                                nullptr, 0, &ssSize);
            if (ssSize > 0) {
                std::vector<BYTE> ssBuf(ssSize);
                auto* ssd = reinterpret_cast<PSECURITY_DESCRIPTOR>(ssBuf.data());
                if (::GetFileSecurityW(srcAbs.c_str(),
                                         SACL_SECURITY_INFORMATION,
                                         ssd, ssSize, &ssSize)) {
                    BOOL present = FALSE, defaulted = FALSE;
                    PACL sacl = nullptr;
                    if (::GetSecurityDescriptorSacl(ssd, &present, &sacl,
                                                      &defaulted) &&
                        present && sacl) {
                        DWORD rc = ::SetNamedSecurityInfoW(
                            const_cast<LPWSTR>(dstAbs.c_str()),
                            SE_FILE_OBJECT,
                            SACL_SECURITY_INFORMATION |
                                UNPROTECTED_SACL_SECURITY_INFORMATION,
                            nullptr, nullptr, nullptr, sacl);
                        if (rc != ERROR_SUCCESS) {
                            ::DeleteFileW(dstAbs.c_str());
                            return ::LayerMount::NtStatusFromWin32(rc);
                        }
                    }
                }
            }
        }
    }

    // Persist the source object's visible file ID so apps that key off
    // IndexNumber keep seeing the same logical object after lower->upper
    // materialization. Pass the caller's LayerConfig so hosts without ADS
    // (!LM_CAP_ADS) fall through to the sidecar store instead of silently
    // dropping the metadata write. Fatal on failure: without the stable-id
    // metadata, post-rename IndexNumber reporting diverges from the source
    // and apps that key off it (build caches, git) see the file as a
    // different object. Tear down the staged destination so the caller
    // can retry from a clean state.
    if (!MetadataADS::WriteLayerMountMetadata(dstAbs, MakeCopyUpMetadata(srcAbs), config)) {
        const DWORD err = ::GetLastError();
        ::DeleteFileW(dstAbs.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
    }

    // Re-apply attributes + timestamps last (both are perturbed by ADS writes
    // on NTFS: WriteFile bumps LastWrite, and the dest was created with
    // FILE_ATTRIBUTE_NORMAL).
    if (srcAttrs != INVALID_FILE_ATTRIBUTES) {
        ::SetFileAttributesW(dstAbs.c_str(), srcAttrs);
    }
    HANDLE tsH = ::CreateFileW(dstAbs.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (tsH != INVALID_HANDLE_VALUE) {
        ::SetFileTime(tsH, &ftCreate, &ftAccess, &ftWrite);
        ::CloseHandle(tsH);
    }

    return STATUS_SUCCESS;
}

// Copy a directory entry (not its contents) preserving attrs/timestamps.
NTSTATUS CopyDirectoryShell(const std::wstring& srcAbs,
                             const std::wstring& dstAbs,
                             const LayerConfig* config) {
    if (!::CreateDirectoryW(dstAbs.c_str(), nullptr)) {
        DWORD err = ::GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            return ::LayerMount::NtStatusFromWin32(err);
        }
        // ERROR_ALREADY_EXISTS is returned both for an existing DIRECTORY
        // (benign — we overlay into it) and an existing FILE (a type
        // collision — we cannot recurse into a file). Without this check,
        // a tree rename into a destination where `dst\child` is already a
        // file would treat the file as a "directory" and then fail every
        // subsequent child copy with PATH_NOT_FOUND. Because the caller
        // previously discarded those errors, the rename appeared to
        // succeed with a corrupt partial tree. Detect the type conflict
        // up front and surface STATUS_OBJECT_NAME_COLLISION so the caller
        // can tear down.
        DWORD existingAttrs = ::GetFileAttributesW(dstAbs.c_str());
        if (existingAttrs == INVALID_FILE_ATTRIBUTES) {
            return ::LayerMount::NtStatusFromWin32(::GetLastError());
        }
        if ((existingAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }

    DWORD srcAttrs = ::GetFileAttributesW(srcAbs.c_str());
    if (srcAttrs != INVALID_FILE_ATTRIBUTES) {
        ::SetFileAttributesW(dstAbs.c_str(), srcAttrs);
    }

    // SetFileAttributes silently ignores FILE_ATTRIBUTE_COMPRESSED — the
    // only way to set it is via FSCTL_SET_COMPRESSION. Propagate it so
    // children created under this directory inherit compression.
    if (srcAttrs != INVALID_FILE_ATTRIBUTES &&
        (srcAttrs & FILE_ATTRIBUTE_COMPRESSED) != 0) {
        HANDLE cmpH = ::CreateFileW(dstAbs.c_str(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (cmpH != INVALID_HANDLE_VALUE) {
            DWORD br = 0;
            USHORT cmpFormat = COMPRESSION_FORMAT_DEFAULT;
            ::DeviceIoControl(cmpH, FSCTL_SET_COMPRESSION,
                              &cmpFormat, sizeof(cmpFormat),
                              nullptr, 0, &br, nullptr);
            ::CloseHandle(cmpH);
        }
    }

    if (!ApplyEncryptedStateIfNeeded(dstAbs, srcAttrs)) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }

    // Copy security descriptor on the directory itself so inheritable
    // ACEs propagate to children created inside. Without this, children
    // recursively copied into an opaque upper directory lose the access-
    // control state that the source parent was enforcing.
    //
    // Use SetNamedSecurityInfoW instead of SetFileSecurityW so Windows
    // handles the inheritance semantics correctly: the inheritable ACEs
    // copied in will re-inherit onto subsequently-created children via
    // the kernel's own auto-inheritance path. SetFileSecurityW bypasses
    // that logic and would leave children without the inherited ACEs.
    {
        DWORD sdSize = 0;
        const SECURITY_INFORMATION ogInfo = OWNER_SECURITY_INFORMATION |
                                              GROUP_SECURITY_INFORMATION;
        ::GetFileSecurityW(srcAbs.c_str(), ogInfo, nullptr, 0, &sdSize);
        if (sdSize > 0) {
            std::vector<BYTE> sdBuf(sdSize);
            auto* sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(sdBuf.data());
            if (::GetFileSecurityW(srcAbs.c_str(), ogInfo, sd, sdSize, &sdSize)) {
                ::SetFileSecurityW(dstAbs.c_str(), ogInfo, sd);
            }
        }
        DWORD dSize = 0;
        ::GetFileSecurityW(srcAbs.c_str(), DACL_SECURITY_INFORMATION,
                            nullptr, 0, &dSize);
        if (dSize > 0) {
            std::vector<BYTE> dBuf(dSize);
            auto* dsd = reinterpret_cast<PSECURITY_DESCRIPTOR>(dBuf.data());
            if (::GetFileSecurityW(srcAbs.c_str(), DACL_SECURITY_INFORMATION,
                                      dsd, dSize, &dSize)) {
                BOOL present = FALSE, defaulted = FALSE;
                PACL dacl = nullptr;
                if (::GetSecurityDescriptorDacl(dsd, &present, &dacl, &defaulted) &&
                    present && dacl) {
                    ::SetNamedSecurityInfoW(
                        const_cast<LPWSTR>(dstAbs.c_str()),
                        SE_FILE_OBJECT,
                        DACL_SECURITY_INFORMATION |
                            UNPROTECTED_DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, dacl, nullptr);
                }
            }
        }
        // SACL: audit ACEs. Gated on SE_SECURITY_NAME — without the
        // privilege, GetFileSecurityW(SACL) fails with
        // ERROR_PRIVILEGE_NOT_HELD; querying would silently lose it, and
        // requesting SACL bits in the OWNER|GROUP call above would fail
        // the whole descriptor fetch. Keep this as a best-effort pass.
        if (IsSecurityPrivHeld()) {
            DWORD ssSize = 0;
            ::GetFileSecurityW(srcAbs.c_str(), SACL_SECURITY_INFORMATION,
                                nullptr, 0, &ssSize);
            if (ssSize > 0) {
                std::vector<BYTE> ssBuf(ssSize);
                auto* ssd = reinterpret_cast<PSECURITY_DESCRIPTOR>(ssBuf.data());
                if (::GetFileSecurityW(srcAbs.c_str(),
                                         SACL_SECURITY_INFORMATION,
                                         ssd, ssSize, &ssSize)) {
                    BOOL present = FALSE, defaulted = FALSE;
                    PACL sacl = nullptr;
                    if (::GetSecurityDescriptorSacl(ssd, &present, &sacl,
                                                      &defaulted) &&
                        present && sacl) {
                        ::SetNamedSecurityInfoW(
                            const_cast<LPWSTR>(dstAbs.c_str()),
                            SE_FILE_OBJECT,
                            SACL_SECURITY_INFORMATION |
                                UNPROTECTED_SACL_SECURITY_INFORMATION,
                            nullptr, nullptr, nullptr, sacl);
                    }
                }
            }
        }
    }

    // Pass the caller's LayerConfig so hosts without ADS (!LM_CAP_ADS)
    // fall through to the sidecar store. Without this the directory's
    // copy-up metadata (origin layer, stable id) is silently dropped.
    // Fatal on failure: a directory shell without origin/stable-id looks
    // like a foreign creation to later resolution, which can misroute
    // child lookups during subsequent rename fanout. Tear down the
    // staged destination directory so the caller retries cleanly.
    if (!MetadataADS::WriteLayerMountMetadata(dstAbs, MakeCopyUpMetadata(srcAbs), config)) {
        const DWORD err = ::GetLastError();
        ::RemoveDirectoryW(dstAbs.c_str());
        return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
    }

    HANDLE srcH = ::CreateFileW(srcAbs.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (srcH != INVALID_HANDLE_VALUE) {
        FILETIME c{}, a{}, w{};
        ::GetFileTime(srcH, &c, &a, &w);
        ::CloseHandle(srcH);
        HANDLE dstH = ::CreateFileW(dstAbs.c_str(), FILE_WRITE_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (dstH != INVALID_HANDLE_VALUE) {
            ::SetFileTime(dstH, &c, &a, &w);
            ::CloseHandle(dstH);
        }
    }
    return STATUS_SUCCESS;
}

} // namespace

NTSTATUS CopyUp::CopyTreePreservingMetadata(const std::wstring& srcAbs,
                                              const std::wstring& dstAbs) {
    // Top-level reparse point: treat the whole tree as a single link.
    DWORD topAttrs = ::GetFileAttributesW(srcAbs.c_str());
    if (topAttrs != INVALID_FILE_ATTRIBUTES &&
        (topAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return CopyUpReparsePointEntry(srcAbs, dstAbs, topAttrs);
    }

    // Top-level regular file: copy directly.
    if (topAttrs != INVALID_FILE_ATTRIBUTES &&
        (topAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return CopyFilePreservingMetadata(srcAbs, dstAbs, &config_);
    }

    // Top-level directory: ensure dst exists, then recurse.
    NTSTATUS status = CopyDirectoryShell(srcAbs, dstAbs, &config_);
    if (!NT_SUCCESS(status)) return status;

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = srcAbs + L"\\*";
    HANDLE hFind = ::FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        // Empty dir (or I/O error) — not a fatal state for the walker.
        return STATUS_SUCCESS;
    }

    NTSTATUS walkStatus = STATUS_SUCCESS;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 ||
             (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;

        const std::wstring childSrc = srcAbs + L"\\" + fd.cFileName;
        const std::wstring childDst = dstAbs + L"\\" + fd.cFileName;

        // Capture child-copy status. Previously return values were dropped;
        // a type conflict (file-over-dir) or I/O error on a single descendant
        // left the tree in a partial state while the caller saw success. Now
        // we propagate the first failure so HandleDirectoryRename (or any
        // other caller) can tear down the half-built destination.
        NTSTATUS childStatus = STATUS_SUCCESS;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            childStatus = CopyUpReparsePointEntry(childSrc, childDst,
                                                    fd.dwFileAttributes);
        } else if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            childStatus = CopyTreePreservingMetadata(childSrc, childDst);
        } else {
            childStatus = CopyFilePreservingMetadata(childSrc, childDst, &config_);
        }
        if (!NT_SUCCESS(childStatus)) {
            walkStatus = childStatus;
            break;
        }
    } while (::FindNextFileW(hFind, &fd));
    ::FindClose(hFind);

    return walkStatus;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

NTSTATUS CopyUp::EnsureParentDirectories(const std::wstring& relativePath) {
    std::filesystem::path relPath(relativePath);
    if (!relPath.has_parent_path()) {
        return STATUS_SUCCESS;
    }

    std::wstring parentRel = relPath.parent_path().wstring();
    if (parentRel.empty()) {
        return STATUS_SUCCESS;
    }

    std::wstring parentNorm = NormalizePath(parentRel);
    if (parentNorm.empty()) {
        return STATUS_SUCCESS;
    }

    // Check if parent already exists in upper
    if (pathResolver_.ExistsInUpper(parentNorm)) {
        return STATUS_SUCCESS;
    }

    // Recursively ensure grandparent exists, then copy-up parent
    NTSTATUS status = EnsureParentDirectories(parentNorm);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Check if parent exists in a lower layer — if so, copy-up the directory
    ResolvedPath parentResolved = pathResolver_.ResolveLowerPath(parentNorm);
    if (parentResolved.Found() &&
        (parentResolved.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return CopyUpDirectory(parentNorm);
    }

    // Parent doesn't exist anywhere — just create it in upper layer
    std::wstring parentUpperPath = pathResolver_.GetUpperPath(parentNorm);
    EnsureDirectoryExists(parentUpperPath);
    return STATUS_SUCCESS;
}

bool CopyUp::CopySecurityDescriptor(const std::wstring& srcPath,
                                     const std::wstring& dstPath) {
    // Request SACL_SECURITY_INFORMATION iff SE_SECURITY_NAME is held.
    // ERROR_PRIVILEGE_NOT_HELD aborts the entire GetFileSecurityW call, so
    // silently gate SACL behind the priv rather than retrying on failure.
    // Audit ACEs survive copy-up only for filesystem services with the priv.
    const bool sacl = IsSecurityPrivHeld();
    SECURITY_INFORMATION secInfo =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION;
    if (sacl) secInfo |= SACL_SECURITY_INFORMATION;

    DWORD sdSize = 0;
    GetFileSecurityW(srcPath.c_str(), secInfo, nullptr, 0, &sdSize);
    if (sdSize == 0) {
        return false;
    }

    std::vector<BYTE> sdBuffer(sdSize);
    auto* sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(sdBuffer.data());

    if (!GetFileSecurityW(srcPath.c_str(), secInfo, sd, sdSize, &sdSize)) {
        return false;
    }

    return SetFileSecurityW(dstPath.c_str(), secInfo, sd) != FALSE;
}

bool CopyUp::CopyTimestamps(HANDLE srcHandle, HANDLE dstHandle) {
    FILETIME creation, access, write;
    if (!GetFileTime(srcHandle, &creation, &access, &write)) {
        return false;
    }
    return SetFileTime(dstHandle, &creation, &access, &write) != FALSE;
}

NTSTATUS CopyUp::CopyFileData(HANDLE srcHandle, HANDLE dstHandle) {
    BYTE buffer[kCopyBufferSize];
    DWORD bytesRead, bytesWritten;

    for (;;) {
        if (!ReadFile(srcHandle, buffer, kCopyBufferSize, &bytesRead, nullptr)) {
            return ::LayerMount::NtStatusFromWin32(GetLastError());
        }
        if (bytesRead == 0) {
            break; // EOF
        }
        // Check BOTH the API-success bit AND a full-count write. WriteFile can
        // succeed with bytesWritten < bytesRead on ENOSPC near the quota wall
        // (write returns partial), on network volumes, and on some raw-device
        // targets. A silent short write here previously would produce a
        // truncated upper file committed atomically — data loss hidden by a
        // "successful" copy-up.
        if (!WriteFile(dstHandle, buffer, bytesRead, &bytesWritten, nullptr) ||
            bytesWritten != bytesRead) {
            DWORD err = ::GetLastError();
            if (err == 0) err = ERROR_WRITE_FAULT; // short write w/ no error
            return ::LayerMount::NtStatusFromWin32(err);
        }
    }

    return STATUS_SUCCESS;
}

} // namespace LayerMount
