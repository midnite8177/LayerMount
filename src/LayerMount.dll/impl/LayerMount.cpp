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

    // Already in upper layer — nothing to do
    std::wstring normalized = NormalizePath(relativePath);
    if (pathResolver_->ExistsInUpper(normalized)) {
        // Update context to point to upper path if it wasn't already
        if (ctx->actualPath != pathResolver_->GetUpperPath(normalized)) {
            ctx->actualPath = pathResolver_->GetUpperPath(normalized);
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

    ctx->actualPath = pathResolver_->GetUpperPath(normalized);
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

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        OperationType openOp = HasWriteAccess(grantedAccess)
            ? OperationType::Write : OperationType::Open;
        if (!tracker->CheckAccess(callerPid, normalized, openOp)) {
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

    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    ctx->relativePath = normalized;
    ctx->isDirectory = (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ctx->ownerPid = callerPid;
    ctx->grantedAccess = grantedAccess;
    ctx->createOptions = createOptions;

    // Lower-layer + write-intent: copy-up first.
    if (resolved.source == LayerSource::Lower && HasWriteAccess(grantedAccess)) {
        NTSTATUS status;
        if (ctx->isDirectory) {
            status = copyUp_->CopyUpDirectory(normalized);
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
            // Capability gate (FR-17): metacopy stages
            // a sparse skeleton in upper. When the host's upper layer
            // doesn't support sparse files, the FSCTL_SET_SPARSE inside
            // CopyUpMetadataOnly fails and the file ends up as a dense
            // zero-filled stub -- worst of both worlds. Force a full data
            // copy when sparse is unavailable.
            if (srcSize.QuadPart > kMetacopyThresholdBytes &&
                capabilities_.HasSparseFiles()) {
                status = copyUp_->CopyUpMetadataOnly(normalized);
                if (NT_SUCCESS(status)) {
                    ctx->isMetacopyOnly = true;
                }
            } else {
                status = copyUp_->CopyUpFile(normalized);
            }
        }
        if (!NT_SUCCESS(status)) {
            return status;
        }
        ctx->actualPath = pathResolver_->GetUpperPath(normalized);
        ctx->writable = true;
    } else {
        ctx->actualPath = resolved.absolutePath;
        ctx->writable = (resolved.source == LayerSource::Upper);
    }

    if (resolved.source == LayerSource::Upper) {
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

    // Reject traversal / drive-qualified / empty paths before any upper-path
    // construction. Without this, `..\escape.txt` becomes `upper\..\escape.txt`
    // and Windows canonicalizes the write target outside the overlay root.
    if (!IsSafeRelativePath(normalized)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    // Reject writes into the reserved sidecar subtree. Callers must never be
    // able to create or materialize files inside `<upper>\.overlay\`.
    if (IsReservedRelativePath(normalized)) {
        return STATUS_ACCESS_DENIED;
    }

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, normalized, OperationType::Create)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    const bool isDirectory = (createOptions & FILE_DIRECTORY_FILE) != 0;

    // Defer whiteout removal until the create commits — an early remove
    // leaves a resurrection window where a failed Create*W surfaces the
    // lower entry to a caller who saw an error.
    const bool hadWhiteout =
        whiteoutMgr_->HasWhiteout(normalized, config_.upperPath);

    ResolvedPath lowerResolved = pathResolver_->ResolveLowerPath(normalized);
    const bool existsInLower = lowerResolved.Found();
    const bool lowerIsDir = existsInLower &&
        (lowerResolved.attributes & FILE_ATTRIBUTE_DIRECTORY);

    std::wstring upperPath = pathResolver_->GetUpperPath(normalized);

    std::filesystem::path parentDir = std::filesystem::path(upperPath).parent_path();
    if (!parentDir.empty()) {
        EnsureDirectoryExists(parentDir.wstring());
    }

    auto ctx = std::make_unique<FileContext>();
    ctx->relativePath = normalized;
    ctx->actualPath = upperPath;
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

        ctx->handle = ::CreateFileW(upperPath.c_str(),
            grantedAccess,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, CREATE_NEW, fileAttributes, nullptr);

        if (ctx->handle == INVALID_HANDLE_VALUE) {
            return NtStatusFromWin32(::GetLastError());
        }

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

    if (ctx->handle == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }

    if (hadWhiteout) {
        whiteoutMgr_->RemoveWhiteout(normalized);
    }

    cache_->InvalidateWithAncestors(normalized);

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

    // Same CREATE_ALWAYS contract as legacy SOverwrite: user-visible ADS are
    // gone; :overlay* stays.
    status = DeleteUserAlternateDataStreams(ctx->actualPath);
    if (!NT_SUCCESS(status)) return status;

    FILE_END_OF_FILE_INFO eofInfo{};
    if (!::SetFileInformationByHandle(ctx->handle, FileEndOfFileInfo,
                                      &eofInfo, sizeof(eofInfo))) {
        return NtStatusFromWin32(::GetLastError());
    }

    if (allocationSize > 0) {
        FILE_ALLOCATION_INFO allocInfo{};
        allocInfo.AllocationSize.QuadPart = static_cast<LONGLONG>(allocationSize);
        if (!::SetFileInformationByHandle(ctx->handle, FileAllocationInfo,
                                          &allocInfo, sizeof(allocInfo))) {
            return NtStatusFromWin32(::GetLastError());
        }
    }

    if (fileAttributes != 0) {
        DWORD newAttrs;
        if (replaceAttributes) {
            newAttrs = fileAttributes;
        } else {
            DWORD currentAttrs = ::GetFileAttributesW(ctx->actualPath.c_str());
            if (currentAttrs == INVALID_FILE_ATTRIBUTES) currentAttrs = 0;
            newAttrs = currentAttrs | fileAttributes;
        }
        if (!::SetFileAttributesW(ctx->actualPath.c_str(), newAttrs)) {
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

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, normalized, OperationType::Delete)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    const bool isDirectory = (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if ((resolved.attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        return STATUS_CANNOT_DELETE;
    }

    if (isDirectory) {
        auto entries = MergeDirectoryEntries(normalized);
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

    NTSTATUS canDelete = CanDelete(normalized, callerPid);
    if (!NT_SUCCESS(canDelete)) {
        return canDelete;
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    const bool isDirectory = resolved.Found() &&
        (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ResolvedPath lowerResolved = pathResolver_->ResolveLowerPath(normalized);
    const bool lowerHasIt = lowerResolved.Found();

    // If upper has a shadow, sweep it. For directories we rely on
    // remove_all to take any opaque-marker / leftover whiteout files
    // with it (legacy SCleanup did the same), falling back to a single
    // RemoveDirectoryW for the empty-dir case.
    const std::wstring upperPath = pathResolver_->GetUpperPath(normalized);
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
    // does not filter `..` segments or drive/stream qualifiers, so without
    // this guard `newRelativePath = "..\\escape.txt"` moves upper content
    // outside the overlay root (and still creates a source-side whiteout).
    // Source is additionally filtered by ResolvePath below, but validate it
    // here too so the caller gets a clean STATUS_OBJECT_NAME_INVALID rather
    // than an ambiguous not-found.
    if (!IsSafeRelativePath(oldNorm) || !IsSafeRelativePath(newNorm)) {
        return STATUS_OBJECT_NAME_INVALID;
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
    if (!IsSafeRelativePath(newNorm) || IsReservedRelativePath(newNorm)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (ctx->relativePath == newNorm) return STATUS_SUCCESS;

    if (ctx->handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(ctx->handle);
        ctx->handle = INVALID_HANDLE_VALUE;
    }
    ctx->relativePath      = newNorm;
    ctx->actualPath        = BuildUpperPathPreserveCase(config_.upperPath,
                                                         newRelativePath);
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

    // Capability gate (FR-17): when the upper layer
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

    // Capability gate (FR-17): no NTFS ACLs => no
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

    if (fileAttributes != INVALID_FILE_ATTRIBUTES) {
        if (!::SetFileAttributesW(ctx->actualPath.c_str(), fileAttributes)) {
            return NtStatusFromWin32(::GetLastError());
        }
    }

    if (ctx->handle != INVALID_HANDLE_VALUE &&
        (creationTime || lastAccessTime || lastWriteTime)) {
        FILETIME ct{}; ct.dwLowDateTime  = static_cast<DWORD>(creationTime);
                       ct.dwHighDateTime = static_cast<DWORD>(creationTime >> 32);
        FILETIME at{}; at.dwLowDateTime  = static_cast<DWORD>(lastAccessTime);
                       at.dwHighDateTime = static_cast<DWORD>(lastAccessTime >> 32);
        FILETIME wt{}; wt.dwLowDateTime  = static_cast<DWORD>(lastWriteTime);
                       wt.dwHighDateTime = static_cast<DWORD>(lastWriteTime >> 32);
        if (!::SetFileTime(ctx->handle,
                creationTime    ? &ct : nullptr,
                lastAccessTime  ? &at : nullptr,
                lastWriteTime   ? &wt : nullptr)) {
            return NtStatusFromWin32(::GetLastError());
        }
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

    if (allocationSize != kUnchanged && ctx->handle != INVALID_HANDLE_VALUE) {
        FILE_ALLOCATION_INFO allocInfo;
        allocInfo.AllocationSize.QuadPart = static_cast<LONGLONG>(allocationSize);
        if (!::SetFileInformationByHandle(ctx->handle, FileAllocationInfo,
                                           &allocInfo, sizeof(allocInfo))) {
            return NtStatusFromWin32(::GetLastError());
        }
    }
    if (fileSize != kUnchanged && ctx->handle != INVALID_HANDLE_VALUE) {
        FILE_END_OF_FILE_INFO eofInfo;
        eofInfo.EndOfFile.QuadPart = static_cast<LONGLONG>(fileSize);
        if (!::SetFileInformationByHandle(ctx->handle, FileEndOfFileInfo,
                                           &eofInfo, sizeof(eofInfo))) {
            return NtStatusFromWin32(::GetLastError());
        }
    }

    if (outInfo != nullptr) {
        return FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath);
    }
    return STATUS_SUCCESS;
}

} // namespace LayerMount
