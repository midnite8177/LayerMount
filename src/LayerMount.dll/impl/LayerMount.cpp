#include "LayerMount.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "MetadataADS.h"
#include "Cache.h"
#include "CopyUp.h"
#include "ProcessTracker.h"
#include "NtStatusUtil.h"
#include "vhd/VHDLayerManager.h"
#include "vss/VSSManager.h"
#include "image/LayerImageManager.h"

#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

#include <algorithm>
#include <climits>
#include <cstring>
#include <cwctype>
#include <string_view>

namespace LayerMount {

namespace {
NTSTATUS ReopenContextHandle(FileContext* ctx);

// Opens a transient kernel handle to the upper-layer file at
// <paramref name="path"/> with the requested access mask. Used by
// SetInfo / Overwrite when the caller-supplied handle lacks the access
// required by SetFileTime / SetFileInformationByHandle (e.g. a handle
// opened with DELETE only — common for del-style flows). Returns an
// invalid ScopedHandle on failure; callers fall back to surfacing the
// original access-denied error. Directory status comes from the
// caller's FileContext rather than a path-based query, which would
// fail silently for DELETE_PENDING files and lose
// FILE_FLAG_BACKUP_SEMANTICS.
ScopedHandle OpenTransientWritableHandle(const std::wstring& path,
                                         DWORD desiredAccess,
                                         bool isDirectory) {
    DWORD flags = isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0;
    HANDLE h = ::CreateFileW(
        path.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr);
    return ScopedHandle{h};
}
}

// ---------------------------------------------------------------------------
// LayerConfig
// ---------------------------------------------------------------------------

bool LayerConfig::Validate(std::wstring& error) const {
    // Check upper layer exists and is a directory
    DWORD upperAttrs = GetFileAttributesW(upperPath.c_str());
    if (upperAttrs == INVALID_FILE_ATTRIBUTES) {
        error = L"Upper layer path does not exist: " + upperPath;
        return false;
    }
    if (!(upperAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        error = L"Upper layer path is not a directory: " + upperPath;
        return false;
    }

    // Test that upper layer is writable
    std::wstring testFile = upperPath + L"\\.layermount_write_test";
    HANDLE hTest = CreateFileW(
        testFile.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr);
    if (hTest == INVALID_HANDLE_VALUE) {
        error = L"Upper layer is not writable: " + upperPath;
        return false;
    }
    CloseHandle(hTest);

    // Check all lower layer paths exist
    for (size_t i = 0; i < lowerPaths.size(); ++i) {
        DWORD lowerAttrs = GetFileAttributesW(lowerPaths[i].c_str());
        if (lowerAttrs == INVALID_FILE_ATTRIBUTES) {
            error = L"Lower layer path does not exist: " + lowerPaths[i];
            return false;
        }
        if (!(lowerAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
            error = L"Lower layer path is not a directory: " + lowerPaths[i];
            return false;
        }
    }

    // Warn (but don't fail) if upper layer is not on NTFS
    wchar_t volumeRoot[MAX_PATH] = {};
    if (GetVolumePathNameW(upperPath.c_str(), volumeRoot, MAX_PATH)) {
        wchar_t fsName[MAX_PATH] = {};
        if (GetVolumeInformationW(volumeRoot, nullptr, 0, nullptr, nullptr, nullptr,
                                   fsName, MAX_PATH)) {
            if (_wcsicmp(fsName, L"NTFS") != 0 && _wcsicmp(fsName, L"ReFS") != 0) {
                OutputDebugStringW(
                    L"[LayerMount] WARNING: Upper layer is not on NTFS/ReFS. "
                    L"NTFS Alternate Data Streams (ADS) will not be available.\n");
            }
        }
    }

    return true;
}

bool LayerConfig::Prepare(std::wstring& error) {
    if (workDirPath.empty()) {
        error = L"Work directory path is empty";
        return false;
    }

    if (!EnsureDirectoryExists(workDirPath)) {
        error = L"Failed to create work directory: " + workDirPath;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// NormalizePath
// ---------------------------------------------------------------------------

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    std::wstring result = path;

    // Replace forward slashes with backslashes
    std::replace(result.begin(), result.end(), L'/', L'\\');

    // Strip leading backslash(es)
    size_t start = 0;
    while (start < result.size() && result[start] == L'\\') {
        ++start;
    }
    if (start > 0) {
        result = result.substr(start);
    }

    // Strip trailing backslash(es)
    while (!result.empty() && result.back() == L'\\') {
        result.pop_back();
    }

    // Fold to lowercase for case-insensitive NTFS matching
    if (!result.empty()) {
        CharLowerBuffW(result.data(), static_cast<DWORD>(result.size()));
    }

    return result;
}

// ---------------------------------------------------------------------------
// IsSafeRelativePath
// ---------------------------------------------------------------------------
//
// Shared guard for every write-side entry point. A path coming from an ABI
// caller is untrusted; once we concatenate it onto `config_.upperPath` and
// hand the result to `CreateFileW` / `MoveFileExW` / `CreateDirectoryW`,
// Windows canonicalizes the combined string and any `..` segment escapes
// the overlay root. A colon anywhere in the path likewise lets the caller
// inject a drive letter (`c:\escape`) or an alternate-data-stream suffix
// that would be written to the wrong target.

bool IsSafeRelativePath(const std::wstring& normalized) {
    if (normalized.empty()) {
        return false;
    }
    if (normalized.find(L':') != std::wstring::npos) {
        return false;
    }
    size_t start = 0;
    while (start <= normalized.size()) {
        size_t end = normalized.find(L'\\', start);
        if (end == std::wstring::npos) {
            end = normalized.size();
        }
        if (end - start == 2 &&
            normalized[start] == L'.' &&
            normalized[start + 1] == L'.') {
            return false;
        }
        if (end == normalized.size()) {
            break;
        }
        start = end + 1;
    }
    return true;
}

// ---------------------------------------------------------------------------
// IsReservedStreamName / TryParseStreamPath
// ---------------------------------------------------------------------------
//
// Stream-aware parsing for the subset of write-side entry points that
// legitimately accept Alternate Data Streams (Create, Open, Delete,
// UpdateContextPath). The rest of the engine still calls `IsSafeRelativePath`
// directly and rejects any `:` in the input.

bool IsReservedStreamName(const std::wstring& streamName) noexcept {
    return ::_wcsicmp(streamName.c_str(), L"overlay") == 0
        || ::_wcsicmp(streamName.c_str(), L"overlay.opaque") == 0;
}

bool TryParseStreamPath(const std::wstring& normalized,
                        std::wstring& outHostNorm,
                        std::wstring& outStreamSuffix) {
    outHostNorm.clear();
    outStreamSuffix.clear();

    if (normalized.empty()) {
        return false;
    }

    const size_t firstColon = normalized.find(L':');
    if (firstColon == std::wstring::npos) {
        // No stream qualifier. Host-only validation.
        if (!IsSafeRelativePath(normalized)) {
            return false;
        }
        outHostNorm = normalized;
        return true;
    }

    // Host is everything before the first colon. Must be a valid relative
    // path on its own (catches empty host like `:rogue`, drive-qualified
    // forms by way of the empty-host check, and `..` traversal).
    std::wstring host = normalized.substr(0, firstColon);
    if (!IsSafeRelativePath(host)) {
        return false;
    }

    // Stream name spans from the character after the first colon up to the
    // next colon (the optional `$TYPE` separator) or end-of-string.
    const size_t streamStart = firstColon + 1;
    const size_t secondColon = normalized.find(L':', streamStart);
    const size_t streamEnd =
        (secondColon == std::wstring::npos) ? normalized.size() : secondColon;
    const std::wstring streamName =
        normalized.substr(streamStart, streamEnd - streamStart);

    if (streamName.empty()) {
        return false;  // `host:` with nothing after the colon.
    }
    // Stream names cannot embed path separators; that would be a smuggled
    // relative path inside the stream suffix.
    if (streamName.find(L'\\') != std::wstring::npos) {
        return false;
    }
    if (IsReservedStreamName(streamName)) {
        return false;
    }

    std::wstring streamType;
    if (secondColon != std::wstring::npos) {
        // Type suffix is everything after the second colon. NTFS spells it
        // with a leading dollar sign (e.g. `$DATA`). Reject empty, additional
        // colons, and any type other than `$DATA`.
        streamType = normalized.substr(secondColon + 1);
        if (streamType.empty()) {
            return false;  // `host:stream:` -- empty type.
        }
        if (streamType.find(L':') != std::wstring::npos) {
            return false;  // Third colon anywhere -- `host:stream:$DATA:extra`.
        }
        if (::_wcsicmp(streamType.c_str(), L"$DATA") != 0) {
            return false;  // Other NTFS types are off-limits at this surface.
        }
    }

    outHostNorm = std::move(host);
    outStreamSuffix.reserve(1 + streamName.size() +
                            (streamType.empty() ? 0 : 1 + streamType.size()));
    outStreamSuffix.push_back(L':');
    outStreamSuffix.append(streamName);
    if (!streamType.empty()) {
        outStreamSuffix.push_back(L':');
        outStreamSuffix.append(streamType);
    }
    return true;
}

// ---------------------------------------------------------------------------
// IsReservedRelativePath
// ---------------------------------------------------------------------------
//
// Sidecar metadata on non-ADS hosts lives at `<upper>\.overlay\...`. Without a
// reservation, that directory would show up in the merged namespace: a user
// could list it, delete entries, or overwrite the .meta.json files that carry
// stable-id / opaque / origin state. Treat `.overlay` as internal and reject
// it at every callback that touches the merged view (resolver, merge,
// Create/Rename/etc.).

bool IsReservedRelativePath(const std::wstring& normalized) {
    // NormalizePath lowercases, so `kSidecarDirName` matches verbatim.
    const std::wstring_view reserved(kSidecarDirName);
    if (normalized.size() < reserved.size()) return false;
    if (normalized.compare(0, reserved.size(), reserved) != 0) return false;
    if (normalized.size() == reserved.size()) return true;
    return normalized[reserved.size()] == L'\\';
}

// ---------------------------------------------------------------------------
// EnsureDirectoryExists
// ---------------------------------------------------------------------------

bool EnsureDirectoryExists(const std::wstring& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Filesystem-host binding lives in the host adapter above this DLL. The
// Mount/Unmount lifecycle, mount-point directory policy, and the
// host-kernel dispatch table all live in the adapter -- the engine only
// exposes primitives through the C ABI.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// LayerMount class implementation
// ---------------------------------------------------------------------------

LayerMount::LayerMount(LayerConfig config)
    : config_(std::move(config))
    , capabilities_(config_.hostCapabilities)
    , events_()
    , cache_(std::make_unique<Cache>(config_.pathCacheCapacity))
    , whiteoutMgr_(std::make_unique<WhiteoutManager>(config_, cache_.get()))
    , pathResolver_(std::make_unique<PathResolver>(config_, *whiteoutMgr_, *cache_))
    , stats_()
    , copyUp_(std::make_unique<CopyUp>(config_, *pathResolver_, *whiteoutMgr_, *cache_, stats_)) {
    // Wire the engine's CopyUp into the per-overlay capability gate +
    // event emitter. Done post-construction so CopyUp's ctor signature
    // stays stable for the in-tree tests that build it directly.
    copyUp_->SetCapabilityGate(capabilities_);
    copyUp_->SetEventEmitter(&events_);
    whiteoutMgr_->SetEventEmitter(&events_);
    if (config_.enableProcessTracking) {
        auto tracker = std::make_shared<ProcessTracker>(config_.accessLogCapacity);
        tracker->SetEventEmitter(&events_);
        // No lock needed here: ctor runs before any other thread can
        // observe this instance.
        processTracker_ = std::move(tracker);
    }
}

LayerMount::~LayerMount() = default;

VHD::VHDLayerManager& LayerMount::Vhd() {
    std::lock_guard<std::mutex> lock(vhdMutex_);
    if (!vhd_) {
        vhd_ = std::make_unique<VHD::VHDLayerManager>(config_.workDirPath);
    }
    return *vhd_;
}

VSS::VSSManager& LayerMount::Vss() {
    std::lock_guard<std::mutex> lock(vssMutex_);
    if (!vss_) {
        // VHD-manifest integration is intentionally not wired here: VSS
        // entries in the VHD manifest are a VHD-layer convenience, and
        // forcing Vhd() init from Vss() would pull in ManifestLock for
        // VSS-only consumers. If both subsystems are used the caller's
        // VHD flow records its own manifest state.
        vss_ = std::make_unique<VSS::VSSManager>(/*manifest*/ nullptr);
    }
    return *vss_;
}

LayerImage::LayerImageManager& LayerMount::Images() {
    std::lock_guard<std::mutex> lock(imagesMutex_);
    if (!images_) {
        images_ = std::make_unique<LayerImage::LayerImageManager>();
    }
    return *images_;
}

HRESULT LayerMount::SetProcessTrackerEnabled(bool enabled) {
    if (enabled) {
        // Build outside the lock so the allocation / LoadRules I/O does
        // not delay readers.
        std::shared_ptr<ProcessTracker> newTracker;
        {
            std::shared_lock readLock(processTrackerMutex_);
            if (processTracker_ != nullptr) return S_OK; // already on
        }
        auto candidate = std::make_shared<ProcessTracker>(config_.accessLogCapacity);
        candidate->SetEventEmitter(&events_);
        if (!config_.processRulesPath.empty()) {
            // No silent scope drop: if the host configured a rules file
            // and we can't load it (missing, unreadable, malformed),
            // refuse to enable the tracker rather than coming up with an
            // empty rule set. An empty rule set means every access is
            // allowed, so silently accepting the failure would mask the
            // gate the operator asked for. Caller can clear
            // processRulesPath and retry if rules-less tracking is the
            // intent.
            if (!candidate->LoadRules(config_.processRulesPath)) {
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
        }
        std::unique_lock writeLock(processTrackerMutex_);
        if (processTracker_ == nullptr) {
            processTracker_ = std::move(candidate);
        }
        // else: another thread won the race; discard our candidate.
    } else {
        // Release our reference; in-flight readers keep their snapshot
        // alive until they drop it, so destruction is deferred safely.
        std::unique_lock writeLock(processTrackerMutex_);
        processTracker_.reset();
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// EnsureInUpperLayer
// ---------------------------------------------------------------------------

NTSTATUS LayerMount::EnsureInUpperLayer(const std::wstring& relativePath,
                                        FileContext* ctx) {
    if (!ctx) {
        return STATUS_INVALID_PARAMETER;
    }

    // Fast path: ctx already represents an upper-layer file with a valid,
    // non-stale kernel handle. The handle itself is proof the file exists
    // in upper; redoing the path-based ExistsInUpper check here is both
    // redundant and incorrect when the upper-layer file is in
    // DELETE_PENDING state from another handle (path-based queries report
    // the file as missing once any handle has set FileDispositionInfo,
    // even though existing handles are still valid). Callers that hold
    // a valid handle should be able to continue mutating attributes /
    // timestamps / sizes through it until they close.
    if (ctx->writable &&
        ctx->handle != INVALID_HANDLE_VALUE &&
        !ctx->handleNeedsReopen) {
        return STATUS_SUCCESS;
    }

    // Already in upper layer — nothing to do
    std::wstring normalized = NormalizePath(relativePath);
    if (pathResolver_->ExistsInUpper(normalized)) {
        // Update context to point to upper path if it wasn't already.
        // Append the stream suffix so a stream context whose handle is
        // about to be reopened lands on the stream, not the main file.
        const std::wstring upperHostPath = pathResolver_->GetUpperPath(normalized);
        const std::wstring upperFullPath = upperHostPath + ctx->streamSuffix;
        if (ctx->actualPath != upperFullPath) {
            ctx->actualPath = upperFullPath;
            ctx->writable = true;
            NTSTATUS reopenStatus = ReopenContextHandle(ctx);
            if (!NT_SUCCESS(reopenStatus)) {
                return reopenStatus;
            }
        }
        return STATUS_SUCCESS;
    }

    // Perform copy-up
    NTSTATUS status;
    if (ctx->isDirectory) {
        status = copyUp_->CopyUpDirectory(normalized);
    } else {
        status = copyUp_->CopyUpFile(normalized);
    }

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx->actualPath = pathResolver_->GetUpperPath(normalized) + ctx->streamSuffix;
    ctx->writable = true;
    NTSTATUS reopenStatus = ReopenContextHandle(ctx);
    if (!NT_SUCCESS(reopenStatus)) {
        return reopenStatus;
    }

    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::EnsureInUpperLayer(const std::wstring& relativePath) {
    std::wstring normalized = NormalizePath(relativePath);
    if (pathResolver_->ExistsInUpper(normalized)) {
        return STATUS_SUCCESS;
    }
    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if ((resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return copyUp_->CopyUpDirectory(normalized);
    }
    return copyUp_->CopyUpFile(normalized);
}

NTSTATUS LayerMount::GetVolumeInfo(UINT64* outTotalSize, UINT64* outFreeSize) const {
    ULARGE_INTEGER freeAvail{}, total{}, totalFree{};
    if (!::GetDiskFreeSpaceExW(config_.upperPath.c_str(),
                                 &freeAvail, &total, &totalFree)) {
        return NtStatusFromWin32(::GetLastError());
    }
    if (outTotalSize != nullptr) *outTotalSize = total.QuadPart;
    if (outFreeSize  != nullptr) *outFreeSize  = totalFree.QuadPart;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// FillFileInfo helpers
// ---------------------------------------------------------------------------

static inline UINT64 FileTimeToUInt64(const FILETIME& ft) {
    return (static_cast<UINT64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

static inline UINT64 MakeIndexNumber(const BY_HANDLE_FILE_INFORMATION& info) {
    return (static_cast<UINT64>(info.nFileIndexHigh) << 32) |
           info.nFileIndexLow;
}

NTSTATUS LayerMount::FillFileInfo(const std::wstring& path,
                                  InternalFileInfo* fileInfo) {
    memset(fileInfo, 0, sizeof(*fileInfo));

    WIN32_FILE_ATTRIBUTE_DATA attrData;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrData)) {
        return NtStatusFromWin32(GetLastError());
    }

    fileInfo->FileAttributes = attrData.dwFileAttributes;
    fileInfo->CreationTime   = FileTimeToUInt64(attrData.ftCreationTime);
    fileInfo->LastAccessTime = FileTimeToUInt64(attrData.ftLastAccessTime);
    fileInfo->LastWriteTime  = FileTimeToUInt64(attrData.ftLastWriteTime);
    fileInfo->ChangeTime     = fileInfo->LastWriteTime; // Windows doesn't expose ChangeTime
    fileInfo->FileSize       = (static_cast<UINT64>(attrData.nFileSizeHigh) << 32) |
                               attrData.nFileSizeLow;
    fileInfo->AllocationSize = (fileInfo->FileSize + 4095) & ~static_cast<UINT64>(4095);
    fileInfo->HardLinks      = 0;
    fileInfo->EaSize         = 0;
    fileInfo->IndexNumber    = 0;

    // If this path is a reparse point, read the tag so the host adapter
    // can advertise the correct reparse class (symlink vs. junction vs.
    // vendor-specific).
    fileInfo->ReparseTag = 0;
    if ((attrData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        WIN32_FIND_DATAW fd{};
        HANDLE fh = ::FindFirstFileW(path.c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            ::FindClose(fh);
            fileInfo->ReparseTag = fd.dwReserved0; // reparse tag
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::FillFileInfoFromHandle(HANDLE handle,
                                            InternalFileInfo* fileInfo,
                                            const std::wstring* pathHint) {
    memset(fileInfo, 0, sizeof(*fileInfo));

    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info)) {
        return NtStatusFromWin32(GetLastError());
    }

    fileInfo->FileAttributes = info.dwFileAttributes;
    fileInfo->CreationTime   = FileTimeToUInt64(info.ftCreationTime);
    fileInfo->LastAccessTime = FileTimeToUInt64(info.ftLastAccessTime);
    fileInfo->LastWriteTime  = FileTimeToUInt64(info.ftLastWriteTime);
    fileInfo->ChangeTime     = fileInfo->LastWriteTime;
    fileInfo->FileSize       = (static_cast<UINT64>(info.nFileSizeHigh) << 32) |
                               info.nFileSizeLow;
    // Query the real on-disk allocation size instead of rounding FileSize up to
    // a 4K boundary. NTFS preallocation (SetFileInformationByHandle with
    // FileAllocationInfo) only persists on an open handle, and callers who set
    // it expect GetFileInformationByHandleEx to report the preallocation back.
    // The old rounding heuristic silently clipped allocation down to EOF, so
    // FileAllocationInfo appeared to have no effect through the mount.
    {
        FILE_STANDARD_INFO si{};
        if (::GetFileInformationByHandleEx(handle, FileStandardInfo,
                                             &si, sizeof(si))) {
            fileInfo->AllocationSize = static_cast<UINT64>(si.AllocationSize.QuadPart);
        } else {
            fileInfo->AllocationSize =
                (fileInfo->FileSize + 4095) & ~static_cast<UINT64>(4095);
        }
    }
    fileInfo->HardLinks      = 0;
    fileInfo->EaSize         = 0;
    fileInfo->IndexNumber    = MakeIndexNumber(info);

    if (pathHint && !pathHint->empty()) {
        const LayerMountMetadata metadata = MetadataADS::ReadLayerMountMetadata(*pathHint);
        if (metadata.hasStableIndexNumber) {
            fileInfo->IndexNumber = metadata.stableIndexNumber;
        }
    }

    // Populate ReparseTag when the handle is on a reparse-point entry.
    // GetFileInformationByHandle doesn't include the tag directly, so query
    // the attribute tag via FileAttributeTagInfo.
    fileInfo->ReparseTag = 0;
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        FILE_ATTRIBUTE_TAG_INFO tagInfo{};
        if (::GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                             &tagInfo, sizeof(tagInfo))) {
            fileInfo->ReparseTag = tagInfo.ReparseTag;
        }
    }

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// MergeDirectoryEntries — shared by ReadDirectory and CanDelete
// ---------------------------------------------------------------------------

std::map<std::wstring, MergedEntry> LayerMount::MergeDirectoryEntries(
    const std::wstring& dirRelativePath) const {

    std::map<std::wstring, MergedEntry> merged;
    std::wstring dirNorm = NormalizePath(dirRelativePath);

    // Reject traversal and reserved-subtree paths before touching the filesystem.
    // Without this, `..` segments in dirRelativePath are canonicalized by Windows
    // during FindFirstFileW and can enumerate outside the overlay root.
    if (!dirNorm.empty() && !IsSafeRelativePath(dirNorm)) {
        return merged;
    }
    if (IsReservedRelativePath(dirNorm)) {
        return merged;
    }

    // Collect whiteout names (original names hidden by whiteout markers)
    std::unordered_set<std::wstring> whitedOutNames;

    // Check if this directory is opaque in the upper layer
    bool isOpaque = whiteoutMgr_->IsOpaque(dirNorm);

    // --- Enumerate upper layer ---
    std::wstring upperDir = config_.upperPath + L"\\" + dirNorm;
    if (dirNorm.empty()) {
        upperDir = config_.upperPath;
    }

    WIN32_FIND_DATAW findData;
    std::wstring searchPath = upperDir + L"\\*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = findData.cFileName;

            // Skip . and ..
            if (name == L"." || name == L"..") continue;

            // Track whiteout names but don't include them in listing
            if (WhiteoutManager::IsWhiteoutName(name)) {
                // Strip .wh. prefix to get the hidden name
                std::wstring hiddenName = name.substr(4);
                std::wstring hiddenNorm = hiddenName;
                CharLowerBuffW(hiddenNorm.data(), static_cast<DWORD>(hiddenNorm.size()));
                whitedOutNames.insert(hiddenNorm);
                continue;
            }

            // Skip opaque marker files
            if (name == kOpaqueMarkerFile) continue;

            std::wstring nameNorm = name;
            CharLowerBuffW(nameNorm.data(), static_cast<DWORD>(nameNorm.size()));

            // Hide the sidecar metadata subtree from merged-view listings.
            // Only possible at the overlay root (SidecarBase places `.overlay`
            // directly under `<upper>`); deeper directories will never enumerate
            // a `.overlay` child unless the user legitimately named one, which
            // would still collide with the reserved name and must stay hidden.
            if (nameNorm == kSidecarDirName) continue;

            MergedEntry entry;
            entry.findData = findData;
            entry.source = LayerSource::Upper;
            merged[nameNorm] = entry;
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }

    // --- Enumerate lower layers (if not opaque) ---
    if (!isOpaque) {
        for (size_t i = 0; i < config_.lowerPaths.size(); ++i) {
            std::wstring lowerDir = config_.lowerPaths[i] + L"\\" + dirNorm;
            if (dirNorm.empty()) {
                lowerDir = config_.lowerPaths[i];
            }

            // Check if this directory is opaque in this lower layer
            std::wstring dirRelNorm = NormalizePath(dirRelativePath);
            if (whiteoutMgr_->IsOpaqueInLayer(dirRelNorm, config_.lowerPaths[i])) {
                break; // Opaque in this layer — skip this and all lower layers
            }

            searchPath = lowerDir + L"\\*";
            hFind = FindFirstFileW(searchPath.c_str(), &findData);
            if (hFind == INVALID_HANDLE_VALUE) continue;

            // Collect per-layer whiteout names. If enumeration failed
            // mid-stream we cannot trust the partial list: a lower entry we
            // would expose might actually be whited-out by a marker we never
            // saw. Conservatively treat enumeration failure like an opaque
            // marker -- close this find handle and stop descending through
            // further lower layers so we don't leak already-deleted entries.
            bool whiteoutEnumOk = true;
            std::vector<std::wstring> layerWhiteouts =
                whiteoutMgr_->ListWhiteoutsInDirectory(
                    dirRelNorm, config_.lowerPaths[i], &whiteoutEnumOk);
            if (!whiteoutEnumOk) {
                FindClose(hFind);
                break;
            }
            for (const auto& wo : layerWhiteouts) {
                std::wstring woNorm = wo;
                CharLowerBuffW(woNorm.data(), static_cast<DWORD>(woNorm.size()));
                whitedOutNames.insert(woNorm);
            }

            do {
                std::wstring name = findData.cFileName;
                if (name == L"." || name == L"..") continue;
                if (WhiteoutManager::IsWhiteoutName(name)) continue;
                if (name == kOpaqueMarkerFile) continue;

                std::wstring nameNorm = name;
                CharLowerBuffW(nameNorm.data(), static_cast<DWORD>(nameNorm.size()));

                // Same reserved-name hide as the upper-layer pass above.
                if (nameNorm == kSidecarDirName) continue;

                // Skip if already present (upper layer wins)
                if (merged.count(nameNorm)) continue;

                // Skip if whited out
                if (whitedOutNames.count(nameNorm)) continue;

                MergedEntry entry;
                entry.findData = findData;
                entry.source = LayerSource::Lower;
                merged[nameNorm] = entry;
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }
    }

    return merged;
}

// ---------------------------------------------------------------------------
// File-handle primitives — host-agnostic OpenFile / CreateFile / CloseFile.
// ---------------------------------------------------------------------------

namespace {

inline bool HasWriteAccess(UINT32 access) {
    return (access & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES |
                      FILE_WRITE_EA | WRITE_DAC | WRITE_OWNER)) != 0;
}

std::wstring NormalizePathPreserveCase(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    std::wstring result = path;
    std::replace(result.begin(), result.end(), L'/', L'\\');

    size_t start = 0;
    while (start < result.size() && result[start] == L'\\') {
        ++start;
    }
    if (start > 0) {
        result = result.substr(start);
    }

    while (!result.empty() && result.back() == L'\\') {
        result.pop_back();
    }

    return result;
}

std::wstring BuildUpperPathPreserveCase(const std::wstring& upperRoot,
                                        const std::wstring& relativePath) {
    std::wstring preserved = NormalizePathPreserveCase(relativePath);
    if (preserved.empty()) return upperRoot;
    return upperRoot + L"\\" + preserved;
}

std::wstring GetExistingPathDisplayCase(const std::wstring& absolutePath) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW(absolutePath.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return absolutePath;
    }
    ::FindClose(find);

    std::filesystem::path p(absolutePath);
    std::filesystem::path parent = p.parent_path();
    if (parent.empty()) {
        return fd.cFileName;
    }
    return (parent / fd.cFileName).wstring();
}

UINT32 ComputeHandleReopenAccess(const FileContext& ctx) {
    UINT32 reopenAccess = ctx.grantedAccess & ~static_cast<UINT32>(DELETE);
    if (reopenAccess == 0) {
        reopenAccess = ctx.isDirectory ? FILE_READ_ATTRIBUTES
                                       : FILE_READ_ATTRIBUTES;
    }
    return reopenAccess;
}

DWORD ComputeHandleReopenFlags(const FileContext& ctx) {
    DWORD flags = ctx.isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0;
    if ((ctx.createOptions & FILE_OPEN_REPARSE_POINT) != 0) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    return flags;
}

NTSTATUS OpenContextHandleWithAccess(FileContext* ctx, UINT32 accessMask) {
    if (ctx == nullptr) return STATUS_INVALID_PARAMETER;

    if (ctx->handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(ctx->handle);
        ctx->handle = INVALID_HANDLE_VALUE;
    }

    ctx->handle = ::CreateFileW(ctx->actualPath.c_str(),
        accessMask,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, ComputeHandleReopenFlags(*ctx),
        nullptr);
    if (ctx->handle == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }

    ctx->handleNeedsReopen = false;
    return STATUS_SUCCESS;
}

NTSTATUS ReopenContextHandle(FileContext* ctx) {
    return OpenContextHandleWithAccess(ctx, ComputeHandleReopenAccess(*ctx));
}

} // namespace

NTSTATUS LayerMount::Open(const std::wstring& relativePath,
                         UINT32 grantedAccess,
                         UINT32 createOptions,
                         DWORD callerPid,
                         std::unique_ptr<FileContext>* outCtx,
                         InternalFileInfo* outInfo) {
    if (outCtx == nullptr || outInfo == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    *outCtx = nullptr;

    std::wstring normalized = NormalizePath(relativePath);

    // Stream-aware validation. The empty-path root-open below is the one
    // legitimate case where `normalized` is empty -- handle that first, then
    // parse for everything else.
    std::wstring hostNorm;
    std::wstring streamSuffix;
    if (!normalized.empty()) {
        if (!TryParseStreamPath(normalized, hostNorm, streamSuffix)) {
            return STATUS_OBJECT_NAME_INVALID;
        }
    }

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        OperationType openOp = HasWriteAccess(grantedAccess)
            ? OperationType::Write : OperationType::Open;
        // Tracker is host-keyed: stream operations inherit the host's
        // access decision.
        if (!tracker->CheckAccess(callerPid, hostNorm, openOp)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    auto ctx = std::make_unique<FileContext>();

    // Root directory: open the upper layer directly.
    if (normalized.empty()) {
        ctx->relativePath = normalized;
        ctx->actualPath = config_.upperPath;
        ctx->isDirectory = true;
        ctx->writable = true;
        ctx->ownerPid = callerPid;
        ctx->grantedAccess = grantedAccess;
        ctx->createOptions = createOptions;

        ctx->handle = ::CreateFileW(config_.upperPath.c_str(),
            grantedAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (ctx->handle == INVALID_HANDLE_VALUE) {
            return NtStatusFromWin32(::GetLastError());
        }

        NTSTATUS status = FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath);
        if (!NT_SUCCESS(status)) {
            ::CloseHandle(ctx->handle);
            return status;
        }

        stats_.activeHandles.fetch_add(1, std::memory_order_relaxed);
        *outCtx = std::move(ctx);
        return STATUS_SUCCESS;
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(hostNorm);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    const bool hostIsDirectory =
        (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!streamSuffix.empty() && hostIsDirectory) {
        // Directory + stream qualifier is rejected at the open surface
        // mirroring Create's stance: streams on directories are out of
        // scope for this overlay.
        return STATUS_FILE_IS_A_DIRECTORY;
    }

    ctx->relativePath = hostNorm;
    ctx->streamSuffix = streamSuffix;
    ctx->isDirectory = hostIsDirectory;
    ctx->ownerPid = callerPid;
    ctx->grantedAccess = grantedAccess;
    ctx->createOptions = createOptions;

    // Lower-layer + write-intent: copy-up first.
    if (resolved.source == LayerSource::Lower && HasWriteAccess(grantedAccess)) {
        NTSTATUS status;
        if (ctx->isDirectory) {
            status = copyUp_->CopyUpDirectory(hostNorm);
        } else if (!streamSuffix.empty()) {
            // Stream opens against a lower-only host MUST force a full
            // data + ADS copy-up. The metacopy optimization defers data
            // replication until a main-stream read/write triggers
            // CompleteLazyCopyUp, which a stream-only handle may never
            // touch -- existing lower-layer ADS would be silently dropped.
            status = copyUp_->CopyUpFile(hostNorm);
        } else {
            // Large-file optimization: stage a metacopy shell (sparse file +
            // :overlay metadata) and defer full data copy until the first
            // read or write actually needs it.
            constexpr LONGLONG kMetacopyThresholdBytes = 1LL * 1024 * 1024;
            LARGE_INTEGER srcSize{};
            if (resolved.attributes != INVALID_FILE_ATTRIBUTES &&
                (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (::GetFileAttributesExW(resolved.absolutePath.c_str(),
                                            GetFileExInfoStandard, &fad)) {
                    srcSize.LowPart = fad.nFileSizeLow;
                    srcSize.HighPart = static_cast<LONG>(fad.nFileSizeHigh);
                }
            }
            // Capability gate: metacopy stages
            // a sparse skeleton in upper. When the host's upper layer
            // doesn't support sparse files, the FSCTL_SET_SPARSE inside
            // CopyUpMetadataOnly fails and the file ends up as a dense
            // zero-filled stub -- worst of both worlds. Force a full data
            // copy when sparse is unavailable.
            if (srcSize.QuadPart > kMetacopyThresholdBytes &&
                capabilities_.HasSparseFiles()) {
                status = copyUp_->CopyUpMetadataOnly(hostNorm);
                if (NT_SUCCESS(status)) {
                    ctx->isMetacopyOnly = true;
                }
            } else {
                status = copyUp_->CopyUpFile(hostNorm);
            }
        }
        if (!NT_SUCCESS(status)) {
            return status;
        }
        ctx->actualPath = pathResolver_->GetUpperPath(hostNorm) + streamSuffix;
        ctx->writable = true;
    } else if (resolved.source == LayerSource::Upper &&
               HasWriteAccess(grantedAccess) &&
               !streamSuffix.empty()) {
        // Writable stream Open against an upper host that's still a
        // metacopy shell: finalize the metacopy now so a later main-stream
        // read/write doesn't trigger CompleteLazyCopyUp and clobber the
        // stream we're about to open (lower's ADS would be copied onto
        // upper, overwriting any same-named stream). The lower-only Open
        // arm above already handles the no-upper-yet case via CopyUpFile.
        const std::wstring upperHostPath = pathResolver_->GetUpperPath(hostNorm);
        const LayerMountMetadata metadata =
            MetadataADS::ReadLayerMountMetadata(upperHostPath, &config_);
        if (metadata.metacopy) {
            NTSTATUS cpStatus = copyUp_->CompleteLazyCopyUp(hostNorm);
            if (!NT_SUCCESS(cpStatus)) {
                return cpStatus;
            }
        }
        ctx->actualPath = upperHostPath + streamSuffix;
        ctx->writable = true;
    } else {
        // Read-only opens (and writable opens on a host that already lives
        // in upper) target the resolved layer's physical path directly;
        // for streams we append the suffix so the kernel sees
        // `<layer>\host:stream`.
        ctx->actualPath = resolved.absolutePath + streamSuffix;
        ctx->writable = (resolved.source == LayerSource::Upper);
    }

    if (resolved.source == LayerSource::Upper && streamSuffix.empty()) {
        // Metacopy bookkeeping reads sidecar metadata off the *host* file;
        // skip it for stream handles -- they don't drive lazy completion
        // and the host's metacopy flag already governs main-stream reads.
        LayerMountMetadata metadata =
            MetadataADS::ReadLayerMountMetadata(ctx->actualPath, &config_);
        ctx->isMetacopyOnly = metadata.metacopy;
    }

    DWORD flags = ctx->isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0;
    if (createOptions & FILE_OPEN_REPARSE_POINT) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    ctx->handle = ::CreateFileW(ctx->actualPath.c_str(),
        grantedAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, flags, nullptr);
    if (ctx->handle == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }

    NTSTATUS status = FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath);
    if (!NT_SUCCESS(status)) {
        ::CloseHandle(ctx->handle);
        return status;
    }

    stats_.activeHandles.fetch_add(1, std::memory_order_relaxed);
    *outCtx = std::move(ctx);
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::Create(const std::wstring& relativePath,
                           UINT32 createOptions,
                           UINT32 grantedAccess,
                           UINT32 fileAttributes,
                           PSECURITY_DESCRIPTOR securityDescriptor,
                           UINT64 allocationSize,
                           DWORD callerPid,
                           std::unique_ptr<FileContext>* outCtx,
                           InternalFileInfo* outInfo) {
    if (outCtx == nullptr || outInfo == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    *outCtx = nullptr;

    std::wstring normalized = NormalizePath(relativePath);

    // Stream-aware validation. Reject traversal / drive-qualified / empty
    // paths and reserved stream names before any upper-path construction.
    // Without this, `..\escape.txt` becomes `upper\..\escape.txt` and
    // Windows canonicalizes the write target outside the overlay root.
    std::wstring hostNorm;
    std::wstring streamSuffix;
    if (!TryParseStreamPath(normalized, hostNorm, streamSuffix)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    // Reject writes into the reserved sidecar subtree. Callers must never be
    // able to create or materialize files inside `<upper>\.overlay\`.
    if (IsReservedRelativePath(hostNorm)) {
        return STATUS_ACCESS_DENIED;
    }

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, hostNorm, OperationType::Create)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    const bool isDirectory = (createOptions & FILE_DIRECTORY_FILE) != 0;
    if (isDirectory && !streamSuffix.empty()) {
        // Directory + stream qualifier is nonsensical in our model. NTFS
        // technically permits ADS on directories; we explicitly do not.
        return STATUS_FILE_IS_A_DIRECTORY;
    }

    // Defer whiteout removal until the create commits — an early remove
    // leaves a resurrection window where a failed Create*W surfaces the
    // lower entry to a caller who saw an error. Whiteouts are host-keyed.
    const bool hadWhiteout =
        whiteoutMgr_->HasWhiteout(hostNorm, config_.upperPath);

    ResolvedPath lowerResolved = pathResolver_->ResolveLowerPath(hostNorm);
    const bool existsInLower = lowerResolved.Found();
    const bool lowerIsDir = existsInLower &&
        (lowerResolved.attributes & FILE_ATTRIBUTE_DIRECTORY);

    std::wstring upperPath = pathResolver_->GetUpperPath(hostNorm);

    if (!streamSuffix.empty()) {
        // ADS-on-directory is rejected: NTFS technically allows it but our
        // overlay does not. Catches both pre-existing upper-layer directories
        // and lower-layer-only directories. The caller-supplied
        // FILE_DIRECTORY_FILE flag is already covered by the earlier
        // `isDirectory && !streamSuffix.empty()` reject; this catches the
        // case where the host *is* a directory but the caller did not pass
        // the flag.
        const DWORD upperAttrs = ::GetFileAttributesW(upperPath.c_str());
        const bool upperIsDir =
            (upperAttrs != INVALID_FILE_ATTRIBUTES) &&
            (upperAttrs & FILE_ATTRIBUTE_DIRECTORY);
        if (upperIsDir || lowerIsDir) {
            return STATUS_FILE_IS_A_DIRECTORY;
        }
    }

    std::filesystem::path parentDir = std::filesystem::path(upperPath).parent_path();
    if (!parentDir.empty()) {
        EnsureDirectoryExists(parentDir.wstring());
    }

    auto ctx = std::make_unique<FileContext>();
    ctx->relativePath = hostNorm;
    ctx->streamSuffix = streamSuffix;
    ctx->actualPath = upperPath + streamSuffix;
    ctx->isDirectory = isDirectory;
    ctx->writable = true;
    ctx->ownerPid = callerPid;
    ctx->grantedAccess = grantedAccess;
    ctx->createOptions = createOptions;

    if (isDirectory) {
        bool dirCreatedByUs = false;
        if (!::CreateDirectoryW(upperPath.c_str(), nullptr)) {
            DWORD err = ::GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                return NtStatusFromWin32(err);
            }
        } else {
            dirCreatedByUs = true;
        }

        // If the opaque marker cannot be persisted, the new directory
        // would leak lower children on lookup. Fail loudly and roll
        // back the create when we were the one who made the directory.
        if (existsInLower && lowerIsDir) {
            if (!whiteoutMgr_->SetOpaque(normalized)) {
                DWORD err = ::GetLastError();
                if (dirCreatedByUs) {
                    ::RemoveDirectoryW(upperPath.c_str());
                }
                return NtStatusFromWin32(err != ERROR_SUCCESS ? err : ERROR_ACCESS_DENIED);
            }
        }

        // Caller-supplied security descriptor must actually take effect;
        // silently falling back to inherited ACLs would violate Windows
        // create semantics and could produce an over-permissive object.
        //
        // Apply via SetKernelObjectSecurity on a backup-semantics handle
        // rather than path-based SetFileSecurityW. SetFileSecurityW does
        // its own DACL check (requires WRITE_DAC/WRITE_OWNER on the
        // object) -- which a freshly-created child under a PROTECTED
        // parent DACL that does not grant those bits will fail with
        // ACCESS_DENIED. A backup-semantics handle honors SE_RESTORE_NAME
        // (enabled in EnsureCopyUpPrivileges via CopyUp construction),
        // which lets the overlay write any ACL on an object it just
        // created even under a restrictive inherited DACL.
        if (securityDescriptor) {
            HANDLE sh = ::CreateFileW(upperPath.c_str(),
                READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (sh == INVALID_HANDLE_VALUE) {
                DWORD err = ::GetLastError();
                if (dirCreatedByUs) {
                    ::RemoveDirectoryW(upperPath.c_str());
                }
                return NtStatusFromWin32(err);
            }
            SECURITY_INFORMATION sinfo = OWNER_SECURITY_INFORMATION |
                                         GROUP_SECURITY_INFORMATION |
                                         DACL_SECURITY_INFORMATION;
            BOOL sdOk = ::SetKernelObjectSecurity(sh, sinfo, securityDescriptor);
            DWORD sdErr = sdOk ? 0 : ::GetLastError();
            ::CloseHandle(sh);
            if (!sdOk) {
                if (dirCreatedByUs) {
                    ::RemoveDirectoryW(upperPath.c_str());
                }
                return NtStatusFromWin32(sdErr);
            }
        }

        ctx->handle = ::CreateFileW(upperPath.c_str(),
            grantedAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    } else {
        if (fileAttributes == 0) {
            fileAttributes = FILE_ATTRIBUTE_NORMAL;
        }

        // Stream creates against a host whose upper-layer copy doesn't yet
        // hold the lower-layer data must finalize copy-up before the new
        // stream attaches -- otherwise a later main-stream read/write
        // triggers CompleteLazyCopyUp, which carries lower ADS onto upper
        // and silently clobbers the just-created stream if its name
        // collides with a lower stream (and re-attaches lower's other
        // streams the user may not expect either).
        //
        // Two shapes need handling: a) host lives only in lower (no upper
        // copy yet) -- full CopyUpFile; b) upper already holds a metacopy
        // shell (sparse skeleton + :overlay metadata) -- CompleteLazyCopyUp
        // promotes the shell into a full file (data + lower ADS) before we
        // attach. After completion the lower ADS sit on upper; a colliding
        // user stream name surfaces as STATUS_OBJECT_NAME_COLLISION from
        // the CREATE_NEW below, which is the right answer.
        if (!streamSuffix.empty()) {
            if (existsInLower && !lowerIsDir &&
                !pathResolver_->ExistsInUpper(hostNorm)) {
                NTSTATUS cpStatus = copyUp_->CopyUpFile(hostNorm);
                if (!NT_SUCCESS(cpStatus)) {
                    return cpStatus;
                }
                upperPath = pathResolver_->GetUpperPath(hostNorm);
                ctx->actualPath = upperPath + streamSuffix;
            } else if (pathResolver_->ExistsInUpper(hostNorm)) {
                const std::wstring upperHostPath =
                    pathResolver_->GetUpperPath(hostNorm);
                const LayerMountMetadata metadata =
                    MetadataADS::ReadLayerMountMetadata(upperHostPath, &config_);
                if (metadata.metacopy) {
                    NTSTATUS cpStatus = copyUp_->CompleteLazyCopyUp(hostNorm);
                    if (!NT_SUCCESS(cpStatus)) {
                        return cpStatus;
                    }
                }
                upperPath = upperHostPath;
                ctx->actualPath = upperPath + streamSuffix;
            }
        }

        ctx->handle = ::CreateFileW(ctx->actualPath.c_str(),
            grantedAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_NEW, fileAttributes, nullptr);

        if (ctx->handle == INVALID_HANDLE_VALUE) {
            return NtStatusFromWin32(::GetLastError());
        }

        // SD and allocationSize apply to the host file, not to an ADS.
        // Streams inherit the host's security descriptor and have their
        // own logical size; skip both branches when attaching a stream.
        if (streamSuffix.empty()) {
            // Same reasoning as the directory branch: use SetKernelObjectSecurity
            // on a backup-semantics handle so SE_RESTORE_NAME lets us write the
            // caller's SD onto a file that inherits a restrictive DACL from its
            // parent (e.g. PROTECTED Everyone-only, no WRITE_DAC).
            if (securityDescriptor) {
                HANDLE sh = ::CreateFileW(upperPath.c_str(),
                    READ_CONTROL | WRITE_DAC | WRITE_OWNER,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                if (sh == INVALID_HANDLE_VALUE) {
                    DWORD err = ::GetLastError();
                    ::CloseHandle(ctx->handle);
                    ctx->handle = INVALID_HANDLE_VALUE;
                    ::DeleteFileW(upperPath.c_str());
                    return NtStatusFromWin32(err);
                }
                SECURITY_INFORMATION sinfo = OWNER_SECURITY_INFORMATION |
                                             GROUP_SECURITY_INFORMATION |
                                             DACL_SECURITY_INFORMATION;
                BOOL sdOk = ::SetKernelObjectSecurity(sh, sinfo, securityDescriptor);
                DWORD sdErr = sdOk ? 0 : ::GetLastError();
                ::CloseHandle(sh);
                if (!sdOk) {
                    ::CloseHandle(ctx->handle);
                    ctx->handle = INVALID_HANDLE_VALUE;
                    ::DeleteFileW(upperPath.c_str());
                    return NtStatusFromWin32(sdErr);
                }
            }

            if (allocationSize > 0) {
                FILE_ALLOCATION_INFO allocInfo;
                allocInfo.AllocationSize.QuadPart = static_cast<LONGLONG>(allocationSize);
                if (!::SetFileInformationByHandle(ctx->handle, FileAllocationInfo,
                                                   &allocInfo, sizeof(allocInfo))) {
                    DWORD err = ::GetLastError();
                    ::CloseHandle(ctx->handle);
                    ctx->handle = INVALID_HANDLE_VALUE;
                    ::DeleteFileW(upperPath.c_str());
                    return NtStatusFromWin32(err);
                }
            }
        }
    }

    if (ctx->handle == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }

    if (hadWhiteout) {
        whiteoutMgr_->RemoveWhiteout(hostNorm);
    }

    cache_->InvalidateWithAncestors(hostNorm);

    NTSTATUS status = FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath);
    if (!NT_SUCCESS(status)) {
        ::CloseHandle(ctx->handle);
        return status;
    }

    stats_.activeHandles.fetch_add(1, std::memory_order_relaxed);
    *outCtx = std::move(ctx);
    return STATUS_SUCCESS;
}

void LayerMount::Close(FileContext* ctx) {
    if (ctx == nullptr) {
        return;
    }

    if (auto tracker = Tracker(); tracker && ctx->ownerPid != 0) {
        tracker->LogAccess(ctx->ownerPid, ctx->relativePath, OperationType::Close);
    }

    if (ctx->handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(ctx->handle);
        ctx->handle = INVALID_HANDLE_VALUE;
    }

    stats_.activeHandles.fetch_sub(1, std::memory_order_relaxed);
}

NTSTATUS LayerMount::EnsureHandleReady(FileContext* ctx) {
    if (ctx == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!ctx->handleNeedsReopen && ctx->handle != INVALID_HANDLE_VALUE) {
        return STATUS_SUCCESS;
    }
    return ReopenContextHandle(ctx);
}

NTSTATUS LayerMount::Read(FileContext* ctx,
                         void* buffer,
                         UINT64 offset,
                         ULONG length,
                         DWORD callerPid,
                         PULONG bytesTransferred) {
    if (ctx == nullptr) return STATUS_INVALID_HANDLE;
    NTSTATUS ready = EnsureHandleReady(ctx);
    if (!NT_SUCCESS(ready)) return ready;
    if (bytesTransferred == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    const DWORD pid = callerPid != 0 ? callerPid : ctx->ownerPid;
    if (auto tracker = Tracker(); tracker && pid != 0) {
        if (!tracker->CheckAccess(pid, ctx->relativePath, OperationType::Read)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    // Complete lazy copy-up before reading: a metacopy shell is a sparse
    // file with no real data blocks, so reading from it returns zeros
    // instead of the lower-layer content the caller expects.
    if (ctx->isMetacopyOnly) {
        NTSTATUS status = copyUp_->CompleteLazyCopyUp(ctx->relativePath);
        if (!NT_SUCCESS(status)) return status;
        ctx->isMetacopyOnly = false;
        NTSTATUS reopenStatus = ReopenContextHandle(ctx);
        if (!NT_SUCCESS(reopenStatus)) return reopenStatus;
    }

    LARGE_INTEGER io;
    io.QuadPart = static_cast<LONGLONG>(offset);
    OVERLAPPED overlapped = {};
    overlapped.Offset = io.LowPart;
    overlapped.OffsetHigh = static_cast<DWORD>(io.HighPart);

    if (!::ReadFile(ctx->handle, buffer, length, bytesTransferred, &overlapped)) {
        DWORD err = ::GetLastError();
        if (err == ERROR_HANDLE_EOF) {
            *bytesTransferred = 0;
            return STATUS_END_OF_FILE;
        }
        return NtStatusFromWin32(err);
    }

    stats_.readCount.fetch_add(1, std::memory_order_relaxed);
    stats_.bytesRead.fetch_add(*bytesTransferred, std::memory_order_relaxed);
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::Write(FileContext* ctx,
                          const void* buffer,
                          UINT64 offset,
                          ULONG length,
                          BOOLEAN writeToEnd,
                          BOOLEAN constrainedIo,
                          DWORD callerPid,
                          PULONG bytesTransferred,
                          InternalFileInfo* outInfo) {
    if (ctx == nullptr) return STATUS_INVALID_HANDLE;
    NTSTATUS ready = EnsureHandleReady(ctx);
    if (!NT_SUCCESS(ready)) return ready;
    if (bytesTransferred == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    const DWORD pid = callerPid != 0 ? callerPid : ctx->ownerPid;
    if (auto tracker = Tracker(); tracker && pid != 0) {
        if (!tracker->CheckAccess(pid, ctx->relativePath, OperationType::Write)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    NTSTATUS status = EnsureInUpperLayer(ctx->relativePath, ctx);
    if (!NT_SUCCESS(status)) return status;

    // If the upper copy is still a metacopy shell (sparse skeleton with no
    // data blocks), finish the copy before we write -- otherwise we'd be
    // writing user content into holes that CompleteLazyCopyUp clobbers.
    if (ctx->isMetacopyOnly) {
        NTSTATUS lazyStatus = copyUp_->CompleteLazyCopyUp(ctx->relativePath);
        if (!NT_SUCCESS(lazyStatus)) return lazyStatus;
        ctx->isMetacopyOnly = false;
        NTSTATUS reopenStatus = ReopenContextHandle(ctx);
        if (!NT_SUCCESS(reopenStatus)) return reopenStatus;
    }

    LARGE_INTEGER fileSize{};
    if (!::GetFileSizeEx(ctx->handle, &fileSize)) {
        // Without a valid size we cannot compute append offsets or constrained
        // I/O truncation. Surfacing the failure prevents fabricating a zero
        // size and silently writing at offset 0 or reporting 0 bytes transferred.
        return NtStatusFromWin32(::GetLastError());
    }

    LARGE_INTEGER writeOffset;
    if (writeToEnd) {
        writeOffset.QuadPart = fileSize.QuadPart;
    } else {
        writeOffset.QuadPart = static_cast<LONGLONG>(offset);
    }

    if (constrainedIo) {
        if (writeOffset.QuadPart >= static_cast<LONGLONG>(fileSize.QuadPart)) {
            *bytesTransferred = 0;
            if (outInfo != nullptr) {
                return FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath);
            }
            return STATUS_SUCCESS;
        }
        if (static_cast<UINT64>(writeOffset.QuadPart) + length >
            static_cast<UINT64>(fileSize.QuadPart)) {
            length = static_cast<ULONG>(fileSize.QuadPart - writeOffset.QuadPart);
        }
    }

    OVERLAPPED overlapped = {};
    overlapped.Offset = writeOffset.LowPart;
    overlapped.OffsetHigh = static_cast<DWORD>(writeOffset.HighPart);

    if (!::WriteFile(ctx->handle, buffer, length, bytesTransferred, &overlapped)) {
        return NtStatusFromWin32(::GetLastError());
    }

    stats_.writeCount.fetch_add(1, std::memory_order_relaxed);
    stats_.bytesWritten.fetch_add(*bytesTransferred, std::memory_order_relaxed);

    if (outInfo != nullptr) {
        return FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath);
    }
    return STATUS_SUCCESS;
}

namespace {

// CREATE_ALWAYS on a file blows away user alternate data streams. Our
// :overlay* bookkeeping is NOT user content and must be preserved; other
// streams are user data and get deleted alongside the primary stream.
bool IsReservedLayerMountStreamName(std::wstring_view streamName) {
    static constexpr std::wstring_view kLayerMount(L":overlay");
    if (streamName.size() < kLayerMount.size()) return false;
    std::wstring lower(streamName.begin(), streamName.begin() + kLayerMount.size());
    ::CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    return lower.compare(0, kLayerMount.size(), kLayerMount) == 0;
}

NTSTATUS DeleteUserAlternateDataStreams(const std::wstring& basePath) {
    WIN32_FIND_STREAM_DATA streamData{};
    HANDLE find = ::FindFirstStreamW(basePath.c_str(), FindStreamInfoStandard,
                                     &streamData, 0);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        return err == ERROR_HANDLE_EOF ? STATUS_SUCCESS
                                       : ::LayerMount::NtStatusFromWin32(err);
    }

    static constexpr std::wstring_view kPrimaryStream(L"::$DATA");
    static constexpr std::wstring_view kDataSuffix(L":$DATA");
    NTSTATUS status = STATUS_SUCCESS;
    for (;;) {
        const std::wstring_view name(streamData.cStreamName);
        if (name != kPrimaryStream && !IsReservedLayerMountStreamName(name)) {
            std::wstring streamPath = basePath;
            if (name.size() >= kDataSuffix.size() &&
                name.compare(name.size() - kDataSuffix.size(),
                             kDataSuffix.size(),
                             kDataSuffix) == 0) {
                streamPath.append(name.data(),
                                  name.size() - kDataSuffix.size());
            } else {
                streamPath += name;
            }

            if (!::DeleteFileW(streamPath.c_str())) {
                const DWORD err = ::GetLastError();
                if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
                    status = ::LayerMount::NtStatusFromWin32(err);
                    break;
                }
            }
        }

        if (!::FindNextStreamW(find, &streamData)) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_HANDLE_EOF) {
                status = ::LayerMount::NtStatusFromWin32(err);
            }
            break;
        }
    }

    ::FindClose(find);
    return status;
}

} // namespace

NTSTATUS LayerMount::Overwrite(FileContext* ctx,
                              UINT32 fileAttributes,
                              BOOLEAN replaceAttributes,
                              UINT64 allocationSize,
                              DWORD callerPid,
                              InternalFileInfo* outInfo) {
    if (ctx == nullptr) return STATUS_INVALID_HANDLE;
    NTSTATUS ready = EnsureHandleReady(ctx);
    if (!NT_SUCCESS(ready)) return ready;

    const DWORD pid = callerPid != 0 ? callerPid : ctx->ownerPid;
    if (auto tracker = Tracker(); tracker && pid != 0) {
        if (!tracker->CheckAccess(pid, ctx->relativePath, OperationType::Overwrite)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    NTSTATUS status = EnsureInUpperLayer(ctx->relativePath, ctx);
    if (!NT_SUCCESS(status)) return status;

    const bool isStreamHandle = !ctx->streamSuffix.empty();

    if (!isStreamHandle) {
        // Same CREATE_ALWAYS contract as legacy SOverwrite: user-visible ADS are
        // gone; :overlay* stays.
        //
        // Skipped for stream handles: `ctx->actualPath` carries the stream
        // suffix, so FindFirstStreamW would enumerate the *host* file's
        // streams and the existing helper would wipe every sibling stream
        // alongside the one the caller meant to truncate. NTFS overwrite
        // semantics target the open stream only -- the kernel-level
        // SetFileInformationByHandle below truncates the stream's data
        // without touching siblings.
        status = DeleteUserAlternateDataStreams(ctx->actualPath);
        if (!NT_SUCCESS(status)) return status;
    }

    // Mirror the SetInfo robustness: the kernel routes IRP_MJ_SET_INFORMATION
    // through whichever handle exists, so an Overwrite arriving via a handle
    // that lacks FILE_WRITE_DATA fails the direct SetFileInformationByHandle
    // call with ERROR_ACCESS_DENIED. Additionally, CopyUp's internal handles
    // (workdir commit, metadata-ADS write, basic-info reapply) can leave the
    // kernel briefly tracking the upper-layer file object after RAII close,
    // and a FileEndOfFileInfo set landing in that window sees
    // ERROR_SHARING_VIOLATION even though ctx->handle has full share modes.
    // Retry through a fresh transient FILE_WRITE_DATA handle in either case.
    auto setSizeInfoRobust = [&](FILE_INFO_BY_HANDLE_CLASS cls,
                                 LPVOID buf, DWORD bufSize) -> NTSTATUS {
        if (::SetFileInformationByHandle(ctx->handle, cls, buf, bufSize)) {
            return STATUS_SUCCESS;
        }
        DWORD firstErr = ::GetLastError();
        if (firstErr != ERROR_ACCESS_DENIED &&
            firstErr != ERROR_SHARING_VIOLATION) {
            return NtStatusFromWin32(firstErr);
        }
        ScopedHandle transient = OpenTransientWritableHandle(
            ctx->actualPath, FILE_WRITE_DATA, ctx->isDirectory);
        if (!transient.IsValid()) {
            return NtStatusFromWin32(firstErr);
        }
        if (!::SetFileInformationByHandle(transient.Get(), cls, buf, bufSize)) {
            return NtStatusFromWin32(::GetLastError());
        }
        return STATUS_SUCCESS;
    };

    FILE_END_OF_FILE_INFO eofInfo{};
    NTSTATUS sizeStatus = setSizeInfoRobust(
        FileEndOfFileInfo, &eofInfo, sizeof(eofInfo));
    if (!NT_SUCCESS(sizeStatus)) return sizeStatus;

    if (allocationSize > 0) {
        FILE_ALLOCATION_INFO allocInfo{};
        allocInfo.AllocationSize.QuadPart = static_cast<LONGLONG>(allocationSize);
        NTSTATUS allocStatus = setSizeInfoRobust(
            FileAllocationInfo, &allocInfo, sizeof(allocInfo));
        if (!NT_SUCCESS(allocStatus)) return allocStatus;
    }

    if (fileAttributes != 0) {
        // File attributes are a property of the host file, not the stream.
        // Target the host's upper-layer path explicitly so a stream
        // overwrite still updates the host's attribute bits correctly.
        const std::wstring attrPath = isStreamHandle
            ? pathResolver_->GetUpperPath(ctx->relativePath)
            : ctx->actualPath;
        DWORD newAttrs;
        if (replaceAttributes) {
            newAttrs = fileAttributes;
        } else {
            DWORD currentAttrs = ::GetFileAttributesW(attrPath.c_str());
            if (currentAttrs == INVALID_FILE_ATTRIBUTES) currentAttrs = 0;
            newAttrs = currentAttrs | fileAttributes;
        }
        if (!::SetFileAttributesW(attrPath.c_str(), newAttrs)) {
            return NtStatusFromWin32(::GetLastError());
        }
    }

    cache_->InvalidateWithAncestors(ctx->relativePath);

    if (outInfo != nullptr) {
        return FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath);
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::Flush(FileContext* ctx,
                          DWORD callerPid,
                          InternalFileInfo* outInfo) {
    if (ctx == nullptr) return STATUS_INVALID_HANDLE;
    NTSTATUS ready = EnsureHandleReady(ctx);
    if (!NT_SUCCESS(ready)) return ready;

    const DWORD pid = callerPid != 0 ? callerPid : ctx->ownerPid;
    if (auto tracker = Tracker(); tracker && pid != 0) {
        if (!tracker->CheckAccess(pid, ctx->relativePath, OperationType::Read)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    if (!::FlushFileBuffers(ctx->handle)) {
        return NtStatusFromWin32(::GetLastError());
    }

    if (outInfo != nullptr) {
        return FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath);
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::CanDelete(const std::wstring& relativePath, DWORD callerPid) {
    std::wstring normalized = NormalizePath(relativePath);

    std::wstring hostNorm;
    std::wstring streamSuffix;
    if (!TryParseStreamPath(normalized, hostNorm, streamSuffix)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, hostNorm, OperationType::Delete)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(hostNorm);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    const bool isDirectory = (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if ((resolved.attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        return STATUS_CANNOT_DELETE;
    }

    if (!streamSuffix.empty()) {
        if (isDirectory) {
            // Directory + stream qualifier is rejected here for symmetry
            // with Create/Open; deleting a stream from a directory makes
            // no sense in our model.
            return STATUS_FILE_IS_A_DIRECTORY;
        }
        // Mirror Delete's lower-only rejection so CanDelete -> Delete is
        // consistent. There is no stream-level whiteout, so a stream that
        // exists only on the lower-layer host cannot be removed through
        // the overlay; tell the caller up front rather than approving the
        // delete and then failing.
        if (!pathResolver_->ExistsInUpper(hostNorm)) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        return STATUS_SUCCESS;
    }

    if (isDirectory) {
        auto entries = MergeDirectoryEntries(hostNorm);
        if (!entries.empty()) {
            return STATUS_DIRECTORY_NOT_EMPTY;
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::CanDelete(FileContext* ctx, DWORD callerPid) {
    if (ctx == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    // `ctx->relativePath` is host-only by construction (Create / Open /
    // UpdateContextPath all strip the stream suffix into ctx->streamSuffix
    // before storing). No re-parsing needed.
    std::wstring normalized = NormalizePath(ctx->relativePath);

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, normalized, OperationType::Delete)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if ((resolved.attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        return STATUS_CANNOT_DELETE;
    }

    const bool isDirectory = (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!ctx->streamSuffix.empty()) {
        if (isDirectory) {
            return STATUS_FILE_IS_A_DIRECTORY;
        }
        // Lower-only stream hosts can't be deleted through the overlay;
        // see the matching guard in Delete().
        if (!pathResolver_->ExistsInUpper(normalized)) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        return STATUS_SUCCESS;
    }

    const bool openedAsReparsePoint =
        (ctx->createOptions & FILE_OPEN_REPARSE_POINT) != 0 &&
        (resolved.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    const bool isEnumerableDirectory =
        isDirectory && !openedAsReparsePoint &&
        (resolved.attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    if (isEnumerableDirectory) {
        auto entries = MergeDirectoryEntries(normalized);
        if (!entries.empty()) {
            return STATUS_DIRECTORY_NOT_EMPTY;
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::Delete(const std::wstring& relativePath, DWORD callerPid) {
    std::wstring normalized = NormalizePath(relativePath);

    std::wstring hostNorm;
    std::wstring streamSuffix;
    if (!TryParseStreamPath(normalized, hostNorm, streamSuffix)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    NTSTATUS canDelete = CanDelete(normalized, callerPid);
    if (!NT_SUCCESS(canDelete)) {
        return canDelete;
    }

    if (!streamSuffix.empty()) {
        // Stream delete: target the host's upper-layer file with the
        // stream suffix appended. Host file and other streams survive.
        // Whiteouts are deliberately NOT touched -- they live at the file
        // level. A stream that exists only on the lower-layer host is
        // not deletable through the overlay (no stream-level whiteout).
        const std::wstring hostUpperPath = pathResolver_->GetUpperPath(hostNorm);
        if (!pathResolver_->ExistsInUpper(hostNorm)) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        const std::wstring streamPath = hostUpperPath + streamSuffix;
        HANDLE h = ::CreateFileW(streamPath.c_str(),
            DELETE | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return NtStatusFromWin32(::GetLastError());
        }
        FILE_DISPOSITION_INFO disp{};
        disp.DeleteFileW = TRUE;
        const BOOL ok = ::SetFileInformationByHandle(
            h, FileDispositionInfo, &disp, sizeof(disp));
        const DWORD setErr = ok ? 0 : ::GetLastError();
        ::CloseHandle(h);
        if (!ok) {
            return NtStatusFromWin32(setErr);
        }
        cache_->InvalidateWithAncestors(hostNorm);
        return STATUS_SUCCESS;
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(hostNorm);
    const bool isDirectory = resolved.Found() &&
        (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ResolvedPath lowerResolved = pathResolver_->ResolveLowerPath(hostNorm);
    const bool lowerHasIt = lowerResolved.Found();

    // If upper has a shadow, sweep it. For directories we rely on
    // remove_all to take any opaque-marker / leftover whiteout files
    // with it (legacy SCleanup did the same), falling back to a single
    // RemoveDirectoryW for the empty-dir case.
    const std::wstring upperPath = pathResolver_->GetUpperPath(hostNorm);
    DWORD upperAttrs = ::GetFileAttributesW(upperPath.c_str());
    if (upperAttrs != INVALID_FILE_ATTRIBUTES) {
        if (isDirectory) {
            if (whiteoutMgr_->IsOpaque(normalized)) {
                whiteoutMgr_->RemoveOpaque(normalized);
            }
            std::error_code ec;
            std::filesystem::remove_all(upperPath, ec);
            if (ec) {
                // remove_all failed: fall back to RemoveDirectoryW, which only
                // succeeds on an empty directory. If both fail we must surface
                // the error and skip whiteout creation; otherwise we would
                // return success while the upper directory still exists and a
                // newly written whiteout hides the lower entry -- leaving the
                // overlay with two conflicting views of the same path.
                if (!::RemoveDirectoryW(upperPath.c_str())) {
                    const DWORD rmErr = ::GetLastError();
                    if (rmErr != ERROR_FILE_NOT_FOUND && rmErr != ERROR_PATH_NOT_FOUND) {
                        cache_->InvalidateWithAncestors(normalized);
                        return NtStatusFromWin32(rmErr);
                    }
                }
            }
        } else {
            if (!::DeleteFileW(upperPath.c_str())) {
                DWORD err = ::GetLastError();
                if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
                    return NtStatusFromWin32(err);
                }
            }
        }
    }

    if (lowerHasIt) {
        // Upper shadow is already gone; if we can't persist the whiteout
        // marker, the lower entry will resurface on the next resolve. Report
        // the failure so the caller sees the delete as failed rather than
        // succeeding and then observing the lower object reappear.
        if (!whiteoutMgr_->CreateWhiteout(normalized,
                isDirectory ? WhiteoutType::Directory : WhiteoutType::File)) {
            const DWORD whErr = ::GetLastError();
            cache_->InvalidateWithAncestors(normalized);
            return whErr ? NtStatusFromWin32(whErr) : STATUS_ACCESS_DENIED;
        }
    }

    cache_->InvalidateWithAncestors(normalized);
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::Delete(FileContext* ctx, DWORD callerPid) {
    if (ctx == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS canDelete = CanDelete(ctx, callerPid);
    if (!NT_SUCCESS(canDelete)) {
        return canDelete;
    }

    if (!ctx->streamSuffix.empty()) {
        // Stream delete on an open handle: flip the delete disposition
        // on the existing stream handle, close it (which commits the
        // delete), and leave the host file + sibling streams intact.
        if (ctx->handle == INVALID_HANDLE_VALUE) {
            return STATUS_INVALID_HANDLE;
        }
        FILE_DISPOSITION_INFO disp{};
        disp.DeleteFileW = TRUE;
        const BOOL ok = ::SetFileInformationByHandle(
            ctx->handle, FileDispositionInfo, &disp, sizeof(disp));
        const DWORD setErr = ok ? 0 : ::GetLastError();
        ::CloseHandle(ctx->handle);
        ctx->handle = INVALID_HANDLE_VALUE;
        ctx->handleNeedsReopen = false;
        if (!ok) {
            return NtStatusFromWin32(setErr);
        }
        cache_->InvalidateWithAncestors(ctx->relativePath);
        return STATUS_SUCCESS;
    }

    std::wstring normalized = NormalizePath(ctx->relativePath);
    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    const bool isDirectory = resolved.Found() &&
        (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool openedAsReparsePoint =
        (ctx->createOptions & FILE_OPEN_REPARSE_POINT) != 0 &&
        resolved.Found() &&
        (resolved.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    const bool deleteAsLink = openedAsReparsePoint &&
        resolved.Found();
    ResolvedPath lowerResolved = pathResolver_->ResolveLowerPath(normalized);
    const bool lowerHasIt = lowerResolved.Found();

    if (ctx->handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(ctx->handle);
        ctx->handle = INVALID_HANDLE_VALUE;
    }
    ctx->handleNeedsReopen = false;

    const std::wstring upperPath = pathResolver_->GetUpperPath(normalized);
    DWORD upperAttrs = ::GetFileAttributesW(upperPath.c_str());
    if (upperAttrs != INVALID_FILE_ATTRIBUTES) {
        if ((upperAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (whiteoutMgr_->IsOpaque(normalized)) {
                whiteoutMgr_->RemoveOpaque(normalized);
            }
            if (deleteAsLink ||
                (upperAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                if (!::RemoveDirectoryW(upperPath.c_str())) {
                    DWORD err = ::GetLastError();
                    if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
                        return NtStatusFromWin32(err);
                    }
                }
            } else {
                std::error_code ec;
                std::filesystem::remove_all(upperPath, ec);
                if (ec) {
                    ::RemoveDirectoryW(upperPath.c_str());
                }
            }
        } else {
            if (!::DeleteFileW(upperPath.c_str())) {
                DWORD err = ::GetLastError();
                if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
                    return NtStatusFromWin32(err);
                }
            }
        }
    }

    if (lowerHasIt) {
        // Same rationale as the path-based Delete above: surface whiteout
        // creation failures so callers don't see success followed by the
        // lower object resurfacing in the merged view.
        if (!whiteoutMgr_->CreateWhiteout(normalized,
                isDirectory ? WhiteoutType::Directory : WhiteoutType::File)) {
            const DWORD whErr = ::GetLastError();
            cache_->InvalidateWithAncestors(normalized);
            return whErr ? NtStatusFromWin32(whErr) : STATUS_ACCESS_DENIED;
        }
    }

    cache_->InvalidateWithAncestors(normalized);
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::Rename(const std::wstring& oldRelativePath,
                           const std::wstring& newRelativePath,
                           BOOLEAN replaceIfExists,
                           DWORD callerPid) {
    std::wstring oldNorm = NormalizePath(oldRelativePath);
    std::wstring newNorm = NormalizePath(newRelativePath);
    const bool isSameLogicalPath = oldNorm == newNorm;

    // Reject unsafe destinations before any copy-up, parent creation, or
    // MoveFileExW. `BuildUpperPathPreserveCase` only strips separators; it
    // does not filter `..` segments or drive qualifiers, so without this
    // guard `newRelativePath = "..\\escape.txt"` moves upper content
    // outside the overlay root (and still creates a source-side whiteout).
    // Source is additionally filtered by ResolvePath below, but validate it
    // here too so the caller gets a clean status rather than an ambiguous
    // not-found.
    //
    // Stream-qualified paths are intentionally rejected on both sides:
    // ADS renames have ugly NTFS semantics (no atomic move; copy+delete
    // only) and aren't worth wiring up at this surface. Returning
    // STATUS_INVALID_PARAMETER (rather than the malformed-path
    // STATUS_OBJECT_NAME_INVALID) signals to callers that the input shape
    // is recognized but unsupported.
    {
        std::wstring oldHostNorm, oldStreamSuffix;
        std::wstring newHostNorm, newStreamSuffix;
        if (!TryParseStreamPath(oldNorm, oldHostNorm, oldStreamSuffix) ||
            !TryParseStreamPath(newNorm, newHostNorm, newStreamSuffix)) {
            return STATUS_OBJECT_NAME_INVALID;
        }
        if (!oldStreamSuffix.empty() || !newStreamSuffix.empty()) {
            return STATUS_INVALID_PARAMETER;
        }
    }

    // Reject renames that touch the reserved sidecar subtree on either end.
    if (IsReservedRelativePath(oldNorm) || IsReservedRelativePath(newNorm)) {
        return STATUS_ACCESS_DENIED;
    }

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, oldNorm, OperationType::Rename)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    ResolvedPath sourceResolved = pathResolver_->ResolvePath(oldNorm);
    if (!sourceResolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    const bool isDirectory =
        (sourceResolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    const bool lowerHasSource = pathResolver_->ResolveLowerPath(oldNorm).Found();
    const bool upperHasSource = pathResolver_->ExistsInUpper(oldNorm);

    // Honor !replaceIfExists by failing fast when the destination already
    // exists. The path-based shim has no open destination handle, so the
    // legacy MoveFileExW(MOVEFILE_REPLACE_EXISTING) check isn't available
    // until the actual move below.
    if (!replaceIfExists && !isSameLogicalPath) {
        ResolvedPath destResolved = pathResolver_->ResolvePath(newNorm);
        if (destResolved.Found()) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }

    const bool destHadWhiteout =
        whiteoutMgr_->HasWhiteout(newNorm, config_.upperPath);

    if (isDirectory) {
        NTSTATUS status = copyUp_->HandleDirectoryRename(
            oldNorm, newNorm, lowerHasSource, replaceIfExists != FALSE);
        if (!NT_SUCCESS(status)) return status;
    } else {
        if (!upperHasSource) {
            NTSTATUS status = copyUp_->CopyUpFile(oldNorm);
            if (!NT_SUCCESS(status)) return status;
        }

        std::wstring oldUpperPath = GetExistingPathDisplayCase(
            BuildUpperPathPreserveCase(config_.upperPath, oldRelativePath));
        std::wstring newUpperPath =
            BuildUpperPathPreserveCase(config_.upperPath, newRelativePath);

        EnsureDirectoryExists(
            std::filesystem::path(newUpperPath).parent_path().wstring());

        DWORD flags = replaceIfExists ? MOVEFILE_REPLACE_EXISTING : 0;
        if (!::MoveFileExW(oldUpperPath.c_str(), newUpperPath.c_str(), flags)) {
            const DWORD moveErr = ::GetLastError();
            if (moveErr != ERROR_ACCESS_DENIED) {
                return NtStatusFromWin32(moveErr);
            }
            // Fallback for restrictive parent ACLs: when CopyUpDirectory
            // propagated an inherited DENY-WRITE from the lower parent up
            // to upper\<parent>, MoveFileExW fails the destination DACL
            // check even though we own the upper file. SE_RESTORE_NAME
            // (enabled in CopyUp::EnsureCopyUpPrivileges) lets a
            // backup-semantics-opened source handle bypass that check via
            // FileRenameInfo. Mirrors CopyUp::CommitFromWorkDir's same-shaped
            // fallback for the work-dir-to-upper commit step.
            HANDLE src = ::CreateFileW(oldUpperPath.c_str(),
                GENERIC_READ | DELETE | SYNCHRONIZE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (src == INVALID_HANDLE_VALUE) {
                return NtStatusFromWin32(::GetLastError());
            }
            const size_t pathBytes = newUpperPath.size() * sizeof(wchar_t);
            std::vector<BYTE> buf(sizeof(FILE_RENAME_INFO) + pathBytes);
            auto* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
            ri->ReplaceIfExists = replaceIfExists ? TRUE : FALSE;
            ri->RootDirectory   = nullptr;
            ri->FileNameLength  = static_cast<DWORD>(pathBytes);
            std::memcpy(ri->FileName, newUpperPath.data(), pathBytes);
            const BOOL renamed = ::SetFileInformationByHandle(
                src, FileRenameInfo, ri, static_cast<DWORD>(buf.size()));
            const DWORD renameErr = renamed ? 0 : ::GetLastError();
            ::CloseHandle(src);
            if (!renamed) {
                return NtStatusFromWin32(renameErr);
            }
        }

        if (lowerHasSource) {
            // Rename has already committed at the filesystem level. If the
            // source-side whiteout can't be persisted the old path will
            // resurface from the lower layer, so surface the failure to the
            // caller rather than claiming success.
            if (!whiteoutMgr_->CreateWhiteout(oldNorm, WhiteoutType::File)) {
                const DWORD whErr = ::GetLastError();
                if (destHadWhiteout) {
                    whiteoutMgr_->RemoveWhiteout(newNorm);
                }
                cache_->InvalidateWithAncestors(oldNorm);
                cache_->InvalidateWithAncestors(newNorm);
                return whErr ? NtStatusFromWin32(whErr) : STATUS_ACCESS_DENIED;
            }
        }
    }

    if (destHadWhiteout) {
        whiteoutMgr_->RemoveWhiteout(newNorm);
    }

    cache_->InvalidateWithAncestors(oldNorm);
    cache_->InvalidateWithAncestors(newNorm);
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::Rename(FileContext* ctx,
                           const std::wstring& newRelativePath,
                           BOOLEAN replaceIfExists,
                           DWORD callerPid) {
    if (ctx == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    const std::wstring oldRelativePath = ctx->relativePath;
    const std::wstring oldNorm = NormalizePath(oldRelativePath);
    const std::wstring newNorm = NormalizePath(newRelativePath);
    const bool sourceWasInUpper = pathResolver_->ExistsInUpper(oldNorm);
    const std::wstring oldActualPath = ctx->actualPath;
    const bool isDirectory = ctx->isDirectory;

    NTSTATUS status = STATUS_SUCCESS;
    if (isDirectory) {
        if (ctx->handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(ctx->handle);
            ctx->handle = INVALID_HANDLE_VALUE;
        }

        status = Rename(oldRelativePath, newRelativePath,
                        replaceIfExists, callerPid);
        if (!NT_SUCCESS(status)) {
            ctx->actualPath = oldActualPath;
            ctx->handleNeedsReopen = true;
            return status;
        }

        ctx->relativePath = newNorm;
        ctx->actualPath = BuildUpperPathPreserveCase(config_.upperPath, newRelativePath);
        ctx->writable = true;
        if (!sourceWasInUpper) {
            ctx->isMetacopyOnly = false;
        }
        ctx->handleNeedsReopen = true;
        return STATUS_SUCCESS;
    }

    if (ctx->handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(ctx->handle);
        ctx->handle = INVALID_HANDLE_VALUE;
    }

    status = Rename(oldRelativePath, newRelativePath,
                    replaceIfExists, callerPid);
    if (!NT_SUCCESS(status)) {
        ctx->actualPath = oldActualPath;
        ctx->handleNeedsReopen = true;
        return status;
    }

    ctx->relativePath = newNorm;
    ctx->actualPath = BuildUpperPathPreserveCase(config_.upperPath, newRelativePath);
    ctx->writable = true;
    if (!sourceWasInUpper) {
        ctx->isMetacopyOnly = false;
    }
    ctx->handleNeedsReopen = true;
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::UpdateContextPath(FileContext* ctx,
                                       const std::wstring& newRelativePath) {
    if (ctx == nullptr) return STATUS_INVALID_HANDLE;
    const std::wstring newNorm = NormalizePath(newRelativePath);

    // Reject the same invalid shapes that Create/Rename/Delete reject at
    // their write-side entry points. Accepting them here would let a buggy
    // host rebind an open handle onto a path outside the overlay root and
    // then issue set-info / delete / read through the rebound context.
    // Stream-qualified inputs are intentionally permitted: hosts that
    // walk their open-handle table after a *file* rename will pass
    // `newhost:stream` for any stream handles that were open on the
    // source; rejecting those would break legitimate host renames.
    std::wstring newHostNorm;
    std::wstring newStreamSuffix;
    if (!TryParseStreamPath(newNorm, newHostNorm, newStreamSuffix)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (IsReservedRelativePath(newHostNorm)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (ctx->relativePath == newHostNorm &&
        ctx->streamSuffix  == newStreamSuffix) {
        return STATUS_SUCCESS;
    }

    if (ctx->handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(ctx->handle);
        ctx->handle = INVALID_HANDLE_VALUE;
    }
    ctx->relativePath      = newHostNorm;
    ctx->streamSuffix      = newStreamSuffix;
    // Feed the case-preserved input (not the lowercased newHostNorm) to
    // BuildUpperPathPreserveCase so display-time consumers reading
    // ctx->actualPath see the caller's original casing. NormalizePath and
    // NormalizePathPreserveCase strip the same leading/trailing slashes,
    // so the stream suffix occupies the same trailing slice of both forms.
    std::wstring newRelativeHostPreserved;
    if (newStreamSuffix.empty()) {
        newRelativeHostPreserved = newRelativePath;
    } else {
        const std::wstring preserved = NormalizePathPreserveCase(newRelativePath);
        newRelativeHostPreserved =
            preserved.substr(0, preserved.length() - newStreamSuffix.length());
    }
    ctx->actualPath        = BuildUpperPathPreserveCase(config_.upperPath,
                                                         newRelativeHostPreserved)
                              + newStreamSuffix;
    ctx->handleNeedsReopen = true;
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::GetSecurity(const std::wstring& relativePath,
                                PUINT32 outAttributes,
                                PSECURITY_DESCRIPTOR sd,
                                SIZE_T sdBytes,
                                SIZE_T* requiredBytes) {
    std::wstring normalized = NormalizePath(relativePath);

    // Resolve target + populate outAttributes -- shared by both the
    // optimized NTFS-ACL path and the !LM_CAP_NTFS_ACLS fallback so a
    // host that lacks ACL semantics still sees real Win32 attributes.
    std::wstring targetPath;
    if (normalized.empty()) {
        targetPath = config_.upperPath;
        if (outAttributes != nullptr) {
            *outAttributes = FILE_ATTRIBUTE_DIRECTORY;
        }
    } else {
        ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
        if (!resolved.Found()) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        targetPath = resolved.absolutePath;
        if (outAttributes != nullptr) {
            DWORD attrs = ::GetFileAttributesW(targetPath.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                return STATUS_OBJECT_NAME_NOT_FOUND;
            }
            *outAttributes = attrs;
        }
    }

    // Capability gate: when the upper layer
    // doesn't carry NTFS ACLs (FAT32, exFAT, network shares with no
    // permissions plumbing), there's no SD to read. Return a synthetic
    // world-readable descriptor (Owner=World, Group=World, DACL grants
    // FILE_GENERIC_READ to Everyone) so callers get a well-formed SD
    // they can hand to other Win32 APIs without surprises.
    if (!capabilities_.HasNtfsAcls()) {
        PSECURITY_DESCRIPTOR worldSd = nullptr;
        ULONG worldSize = 0;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"O:WDG:WDD:(A;;FR;;;WD)", SDDL_REVISION_1,
                &worldSd, &worldSize)) {
            return NtStatusFromWin32(::GetLastError());
        }
        if (requiredBytes != nullptr) {
            *requiredBytes = worldSize;
        }
        if (sd == nullptr || sdBytes < worldSize) {
            ::LocalFree(worldSd);
            return (sd == nullptr) ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
        }
        std::memcpy(sd, worldSd, worldSize);
        ::LocalFree(worldSd);
        return STATUS_SUCCESS;
    }

    // Include SACL iff the FS process holds SE_SECURITY_NAME; otherwise
    // ::GetFileSecurityW fails the whole call with ERROR_PRIVILEGE_NOT_HELD
    // and we lose even OWNER/DACL.
    const SECURITY_INFORMATION secInfo =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION |
        (CopyUp::IsSecurityPrivAvailable() ? SACL_SECURITY_INFORMATION : 0);

    DWORD needed = 0;
    BOOL ok = ::GetFileSecurityW(targetPath.c_str(), secInfo,
                                  sd, static_cast<DWORD>(sdBytes), &needed);
    if (requiredBytes != nullptr) {
        *requiredBytes = needed;
    }
    if (!ok) {
        DWORD err = ::GetLastError();
        if (err == ERROR_INSUFFICIENT_BUFFER) {
            return STATUS_BUFFER_OVERFLOW;
        }
        return NtStatusFromWin32(err);
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::SetSecurity(const std::wstring& relativePath,
                                UINT32 securityInformation,
                                PSECURITY_DESCRIPTOR sd,
                                DWORD callerPid) {
    if (sd == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    std::wstring normalized = NormalizePath(relativePath);

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, normalized, OperationType::SetSecurity)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    // Capability gate: no NTFS ACLs => no
    // descriptor to update. Silently no-op rather than error so callers
    // that always set security after create (the default behavior of
    // CreateFile + InitializeSecurityDescriptor) keep working.
    if (!capabilities_.HasNtfsAcls()) {
        return STATUS_SUCCESS;
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if (resolved.source == LayerSource::Lower) {
        NTSTATUS status;
        if ((resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            status = copyUp_->CopyUpDirectory(normalized);
        } else {
            status = copyUp_->CopyUpFile(normalized);
        }
        if (!NT_SUCCESS(status)) return status;
    }

    std::wstring upperPath = pathResolver_->GetUpperPath(normalized);
    if (!::SetFileSecurityW(upperPath.c_str(),
            static_cast<SECURITY_INFORMATION>(securityInformation), sd)) {
        return NtStatusFromWin32(::GetLastError());
    }

    cache_->InvalidateWithAncestors(normalized);
    return STATUS_SUCCESS;
}

namespace {

inline HANDLE OpenForReparseRead(const std::wstring& path) {
    return ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
}

inline HANDLE OpenForReparseWrite(const std::wstring& path) {
    return ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
}

} // namespace

NTSTATUS LayerMount::GetReparsePoint(const std::wstring& relativePath,
                                    PVOID buffer,
                                    SIZE_T bufferBytes,
                                    SIZE_T* requiredBytes) {
    std::wstring normalized = NormalizePath(relativePath);
    if (normalized.empty()) {
        return STATUS_NOT_A_REPARSE_POINT;
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if ((resolved.attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return STATUS_NOT_A_REPARSE_POINT;
    }

    HANDLE h = OpenForReparseRead(resolved.absolutePath);
    if (h == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }

    // Stage into a max-size buffer first, then fan out by the caller's
    // capacity. FSCTL_GET_REPARSE_POINT does not have a "tell me the
    // size" mode -- short buffers fail with ERROR_MORE_DATA without
    // reporting the required size.
    BYTE staging[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    DWORD returned = 0;
    BOOL ok = ::DeviceIoControl(h, FSCTL_GET_REPARSE_POINT,
                                  nullptr, 0,
                                  staging, sizeof(staging),
                                  &returned, nullptr);
    DWORD lastErr = ok ? 0 : ::GetLastError();
    ::CloseHandle(h);
    if (!ok) {
        if (lastErr == ERROR_NOT_A_REPARSE_POINT) return STATUS_NOT_A_REPARSE_POINT;
        return NtStatusFromWin32(lastErr);
    }

    if (requiredBytes != nullptr) {
        *requiredBytes = returned;
    }
    if (buffer == nullptr || bufferBytes == 0) {
        return STATUS_SUCCESS;
    }
    if (bufferBytes < returned) {
        return STATUS_BUFFER_OVERFLOW;
    }
    std::memcpy(buffer, staging, returned);
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::SetReparsePoint(const std::wstring& relativePath,
                                    const void* buffer,
                                    SIZE_T bufferBytes,
                                    DWORD callerPid) {
    if (buffer == nullptr || bufferBytes == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    // FSCTL_SET_REPARSE_POINT takes a DWORD input length; a SIZE_T larger
    // than DWORD_MAX would silently truncate at the IOCTL boundary and
    // the kernel would see a shorter buffer than we validated. Reject
    // buffers larger than the documented reparse data maximum -- anything
    // above MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 KB) is invalid per
    // Windows reparse point contract.
    if (bufferBytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }
    std::wstring normalized = NormalizePath(relativePath);

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, normalized, OperationType::SetInfo)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (resolved.source == LayerSource::Lower) {
        NTSTATUS status;
        if ((resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            status = copyUp_->CopyUpDirectory(normalized);
        } else {
            status = copyUp_->CopyUpFile(normalized);
        }
        if (!NT_SUCCESS(status)) return status;
    }

    std::wstring upperPath = pathResolver_->GetUpperPath(normalized);
    HANDLE h = OpenForReparseWrite(upperPath);
    if (h == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }
    DWORD returned = 0;
    BOOL ok = ::DeviceIoControl(h, FSCTL_SET_REPARSE_POINT,
                                  const_cast<void*>(buffer),
                                  static_cast<DWORD>(bufferBytes),
                                  nullptr, 0, &returned, nullptr);
    DWORD lastErr = ok ? 0 : ::GetLastError();
    ::CloseHandle(h);
    if (!ok) {
        return NtStatusFromWin32(lastErr);
    }

    cache_->InvalidateWithAncestors(normalized);
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::DeleteReparsePoint(const std::wstring& relativePath,
                                       const void* buffer,
                                       SIZE_T bufferBytes,
                                       DWORD callerPid) {
    if (buffer == nullptr || bufferBytes == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    std::wstring normalized = NormalizePath(relativePath);

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, normalized, OperationType::SetInfo)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (resolved.source == LayerSource::Lower) {
        NTSTATUS status;
        if ((resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            status = copyUp_->CopyUpDirectory(normalized);
        } else {
            status = copyUp_->CopyUpFile(normalized);
        }
        if (!NT_SUCCESS(status)) return status;
    }

    std::wstring upperPath = pathResolver_->GetUpperPath(normalized);
    HANDLE h = OpenForReparseWrite(upperPath);
    if (h == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }
    DWORD returned = 0;
    BOOL ok = ::DeviceIoControl(h, FSCTL_DELETE_REPARSE_POINT,
                                  const_cast<void*>(buffer),
                                  static_cast<DWORD>(bufferBytes),
                                  nullptr, 0, &returned, nullptr);
    DWORD lastErr = ok ? 0 : ::GetLastError();
    ::CloseHandle(h);
    if (!ok) {
        return NtStatusFromWin32(lastErr);
    }

    cache_->InvalidateWithAncestors(normalized);
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::SetInfo(FileContext* ctx,
                            UINT32 fileAttributes,
                            UINT64 creationTime,
                            UINT64 lastAccessTime,
                            UINT64 lastWriteTime,
                            UINT64 changeTime,
                            UINT64 allocationSize,
                            UINT64 fileSize,
                            InternalFileInfo* outInfo) {
    if (ctx == nullptr) {
        return STATUS_INVALID_HANDLE;
    }
    (void)changeTime; // ChangeTime is approximated as LastWriteTime in FillFileInfo.

    if (auto tracker = Tracker(); tracker && ctx->ownerPid != 0) {
        if (!tracker->CheckAccess(ctx->ownerPid, ctx->relativePath, OperationType::SetInfo)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    NTSTATUS status = EnsureInUpperLayer(ctx->relativePath, ctx);
    if (!NT_SUCCESS(status)) return status;

    // Prefer handle-based attribute set so we don't have to re-open the
    // file by path. The path-based ::SetFileAttributesW would otherwise
    // be refused with STATUS_DELETE_PENDING (-> ERROR_ACCESS_DENIED)
    // whenever the upper-layer file has been marked for deletion by a
    // separate handle. SetFileInformationByHandle(FileBasicInfo) goes
    // through ctx->handle, which is already past the kernel's open-by-
    // name gate. Zero values for the timestamp fields signal "no
    // change" to NT, so a basic-info call carrying only FileAttributes
    // is metadata-only.
    if (fileAttributes != INVALID_FILE_ATTRIBUTES) {
        if (ctx->handle != INVALID_HANDLE_VALUE) {
            FILE_BASIC_INFO bi{};
            bi.FileAttributes = fileAttributes;
            if (!::SetFileInformationByHandle(ctx->handle, FileBasicInfo,
                                               &bi, sizeof(bi))) {
                DWORD firstErr = ::GetLastError();
                if (firstErr != ERROR_ACCESS_DENIED) {
                    return NtStatusFromWin32(firstErr);
                }
                ScopedHandle transient = OpenTransientWritableHandle(
                    ctx->actualPath, FILE_WRITE_ATTRIBUTES, ctx->isDirectory);
                if (!transient.IsValid()) {
                    return NtStatusFromWin32(firstErr);
                }
                if (!::SetFileInformationByHandle(transient.Get(),
                        FileBasicInfo, &bi, sizeof(bi))) {
                    return NtStatusFromWin32(::GetLastError());
                }
            }
        } else if (!::SetFileAttributesW(ctx->actualPath.c_str(),
                                          fileAttributes)) {
            return NtStatusFromWin32(::GetLastError());
        }
    }

    // SetFileTime / SetFileInformationByHandle require FILE_WRITE_ATTRIBUTES
    // (timestamps) or FILE_WRITE_DATA / GENERIC_WRITE (sizes) on the handle.
    // The kernel routes SET_INFORMATION calls through whichever open handle
    // exists for the file, regardless of the access mask requested at open
    // time, so a handle opened with e.g. DELETE alone can land here and the
    // direct ::SetFileTime(ctx->handle, ...) would be refused with
    // ERROR_ACCESS_DENIED. OpenTransientWritableHandle returns a transient
    // ScopedHandle with the requested access; it closes automatically on
    // scope exit.
    auto setFileTimeRobust = [&](FILETIME* ct, FILETIME* at, FILETIME* wt) -> NTSTATUS {
        if (::SetFileTime(ctx->handle, ct, at, wt)) {
            return STATUS_SUCCESS;
        }
        DWORD firstErr = ::GetLastError();
        if (firstErr != ERROR_ACCESS_DENIED) {
            return NtStatusFromWin32(firstErr);
        }
        ScopedHandle transient = OpenTransientWritableHandle(
            ctx->actualPath, FILE_WRITE_ATTRIBUTES, ctx->isDirectory);
        if (!transient.IsValid()) {
            return NtStatusFromWin32(firstErr);
        }
        if (!::SetFileTime(transient.Get(), ct, at, wt)) {
            return NtStatusFromWin32(::GetLastError());
        }
        return STATUS_SUCCESS;
    };

    if (ctx->handle != INVALID_HANDLE_VALUE &&
        (creationTime || lastAccessTime || lastWriteTime)) {
        FILETIME ct{}; ct.dwLowDateTime  = static_cast<DWORD>(creationTime);
                       ct.dwHighDateTime = static_cast<DWORD>(creationTime >> 32);
        FILETIME at{}; at.dwLowDateTime  = static_cast<DWORD>(lastAccessTime);
                       at.dwHighDateTime = static_cast<DWORD>(lastAccessTime >> 32);
        FILETIME wt{}; wt.dwLowDateTime  = static_cast<DWORD>(lastWriteTime);
                       wt.dwHighDateTime = static_cast<DWORD>(lastWriteTime >> 32);
        NTSTATUS timeStatus = setFileTimeRobust(
            creationTime    ? &ct : nullptr,
            lastAccessTime  ? &at : nullptr,
            lastWriteTime   ? &wt : nullptr);
        if (!NT_SUCCESS(timeStatus)) return timeStatus;
    }

    constexpr UINT64 kUnchanged = UINT64_MAX;
    // FILE_ALLOCATION_INFO / FILE_END_OF_FILE_INFO take signed LONGLONG;
    // an unsigned size above LLONG_MAX becomes negative at the WinAPI
    // boundary and produces either undefined filesystem behavior or a
    // misleading STATUS_INVALID_PARAMETER far from the real fault. Reject
    // explicit sizes (kUnchanged stays as the unchanged sentinel).
    constexpr UINT64 kMaxSignedSize = static_cast<UINT64>(LLONG_MAX);
    if (allocationSize != kUnchanged && allocationSize > kMaxSignedSize) {
        return STATUS_INVALID_PARAMETER;
    }
    if (fileSize != kUnchanged && fileSize > kMaxSignedSize) {
        return STATUS_INVALID_PARAMETER;
    }

    // Sizes need a handle with write-data semantics. As with timestamps
    // above, fall back to a transient FILE_WRITE_DATA / FILE_WRITE_ATTRIBUTES
    // handle if ctx->handle was opened with insufficient access
    // (ERROR_ACCESS_DENIED). Also retry on ERROR_SHARING_VIOLATION: CopyUp's
    // internal handles (workdir commit, metadata-ADS write, basic-info
    // reapply) can leave the kernel briefly tracking the upper-layer file
    // object after RAII close, and a FileEndOfFileInfo set landing in that
    // window sees a sharing conflict even though ctx->handle has full share
    // modes.
    auto setInfoByHandleRobust = [&](FILE_INFO_BY_HANDLE_CLASS cls,
                                     LPVOID buf, DWORD bufSize,
                                     DWORD transientAccess) -> NTSTATUS {
        if (::SetFileInformationByHandle(ctx->handle, cls, buf, bufSize)) {
            return STATUS_SUCCESS;
        }
        DWORD firstErr = ::GetLastError();
        if (firstErr != ERROR_ACCESS_DENIED &&
            firstErr != ERROR_SHARING_VIOLATION) {
            return NtStatusFromWin32(firstErr);
        }
        ScopedHandle transient = OpenTransientWritableHandle(
            ctx->actualPath, transientAccess, ctx->isDirectory);
        if (!transient.IsValid()) {
            return NtStatusFromWin32(firstErr);
        }
        if (!::SetFileInformationByHandle(transient.Get(), cls, buf, bufSize)) {
            return NtStatusFromWin32(::GetLastError());
        }
        return STATUS_SUCCESS;
    };

    if (allocationSize != kUnchanged && ctx->handle != INVALID_HANDLE_VALUE) {
        FILE_ALLOCATION_INFO allocInfo;
        allocInfo.AllocationSize.QuadPart = static_cast<LONGLONG>(allocationSize);
        NTSTATUS s = setInfoByHandleRobust(
            FileAllocationInfo, &allocInfo, sizeof(allocInfo),
            FILE_WRITE_DATA);
        if (!NT_SUCCESS(s)) return s;
    }
    if (fileSize != kUnchanged && ctx->handle != INVALID_HANDLE_VALUE) {
        FILE_END_OF_FILE_INFO eofInfo;
        eofInfo.EndOfFile.QuadPart = static_cast<LONGLONG>(fileSize);
        NTSTATUS s = setInfoByHandleRobust(
            FileEndOfFileInfo, &eofInfo, sizeof(eofInfo),
            FILE_WRITE_DATA);
        if (!NT_SUCCESS(s)) return s;
    }

    if (outInfo != nullptr) {
        return FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath);
    }
    return STATUS_SUCCESS;
}

namespace {
// Stream names FindFirstStreamW returns are NTFS-native forms such as
// "::$DATA" (the main unnamed stream — file content), ":foo:$DATA" (a
// user-defined ADS named "foo"), and ":overlay:$DATA" /
// ":overlay.opaque:$DATA" (LayerMount's reserved metadata streams).
// EnumerateStreams hides these from callers so the result list is the
// user-facing surface: named data streams only, no implementation
// detail and no main-content alias.
//
// The reserved-name comparison strings are derived from the same
// kLayerMountADSStream / kOpaqueADSStream constants the metadata
// writers use (see MetadataADS.cpp). Adding a new internal stream by
// extending those constants automatically extends this filter.
bool IsReservedFullNtfsStreamName(const wchar_t* name) noexcept {
    if (name == nullptr) return false;
    static const std::wstring kMainData      = L"::$DATA";
    static const std::wstring kOverlayData   =
        std::wstring(kLayerMountADSStream) + L":$DATA";
    static const std::wstring kOpaqueData    =
        std::wstring(kOpaqueADSStream)     + L":$DATA";
    return ::_wcsicmp(name, kMainData.c_str())    == 0
        || ::_wcsicmp(name, kOverlayData.c_str()) == 0
        || ::_wcsicmp(name, kOpaqueData.c_str())  == 0;
}
} // namespace

NTSTATUS LayerMount::EnumerateStreams(const std::wstring& relativePath,
                                      std::vector<InternalStreamInfo>& out) {
    out.clear();

    std::wstring normalized = NormalizePath(relativePath);
    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    WIN32_FIND_STREAM_DATA findData{};
    HANDLE h = ::FindFirstStreamW(
        resolved.absolutePath.c_str(),
        FindStreamInfoStandard,
        &findData,
        0);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        // ERROR_HANDLE_EOF means the file has no streams at all (rare —
        // every NTFS file has at least ::$DATA, but the API documents
        // this as a valid empty-result code).
        if (err == ERROR_HANDLE_EOF) {
            return STATUS_SUCCESS;
        }
        return NtStatusFromWin32(err);
    }

    // info.name assignment and out.push_back below can throw std::bad_alloc;
    // the unique_ptr guarantees FindClose runs on exception unwind.
    std::unique_ptr<void, decltype(&::FindClose)> findGuard(h, &::FindClose);

    do {
        if (IsReservedFullNtfsStreamName(findData.cStreamName)) {
            continue;
        }
        InternalStreamInfo info;
        info.name = findData.cStreamName;
        info.streamSize = static_cast<UINT64>(findData.StreamSize.QuadPart);
        // WIN32_FIND_STREAM_DATA exposes only the logical size; the
        // physical on-disk allocation is not reported by this query
        // form. Treat the two as identical at this layer — callers that
        // need precise allocation accounting should open the stream and
        // query via NtQueryInformationFile(FILE_STANDARD_INFORMATION).
        info.allocationSize = info.streamSize;
        out.push_back(std::move(info));
    } while (::FindNextStreamW(h, &findData));

    DWORD lastErr = ::GetLastError();
    if (lastErr != ERROR_HANDLE_EOF && lastErr != ERROR_SUCCESS) {
        // FindNextStreamW failed mid-iteration with a real error.
        return NtStatusFromWin32(lastErr);
    }
    return STATUS_SUCCESS;
}

} // namespace LayerMount
