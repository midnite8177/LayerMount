#include "LayerMount.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "MetadataStore.h"
#include "Cache.h"
#include "LayerPath.h"
#include "CopyUp.h"
#include "ElevationUtil.h"
#include "ProcessTracker.h"
#include "NtStatusUtil.h"
#include "NtdllExport.h"
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
#include <system_error>
#include <unordered_set>

namespace LayerMount {

namespace {
NTSTATUS ReopenContextHandle(FileContext* ctx);

// Removes the directory at `path` on scope exit while armed. A create
// arms it after CreateDirectoryW makes the directory and disarms it once
// the handle is open, so a failure in between removes only what the
// create made.
class DirectoryRollback {
public:
    explicit DirectoryRollback(const std::wstring& path) : path_(path) {}
    ~DirectoryRollback() {
        if (armed_) {
            ::RemoveDirectoryW(path_.c_str());
        }
    }
    DirectoryRollback(const DirectoryRollback&) = delete;
    DirectoryRollback& operator=(const DirectoryRollback&) = delete;

    void Arm() { armed_ = true; }
    void Disarm() { armed_ = false; }

private:
    const std::wstring& path_;
    bool armed_ = false;
};

// Opens a short-lived handle to a physical path. Returns an invalid
// ScopedHandle on failure and leaves GetLastError set. The caller passes
// the directory status, because a path-based query fails for a
// delete-pending file and would drop FILE_FLAG_BACKUP_SEMANTICS.
ScopedHandle OpenTransientHandle(const std::wstring& path,
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

// Runs `setInfo` on the context handle. When that fails with an access
// or sharing error, runs it again on a transient handle opened with
// `transientAccess`. The kernel routes SET_INFORMATION calls through
// whichever open handle exists for the file, regardless of the access
// mask requested at open time, so a handle opened with DELETE alone can
// land here. A sharing conflict comes from the copy-up's internal
// handles, which the kernel tracks briefly after their close. Any other
// error, or a failed transient open, returns the first error.
template <typename SetInfoFn>
NTSTATUS SetInfoWithTransientRetry(const FileContext& ctx,
                                   DWORD transientAccess,
                                   SetInfoFn setInfo) {
    if (setInfo(ctx.handle)) {
        return STATUS_SUCCESS;
    }
    const DWORD firstErr = ::GetLastError();
    if (firstErr != ERROR_ACCESS_DENIED &&
        firstErr != ERROR_SHARING_VIOLATION) {
        return NtStatusFromWin32(firstErr);
    }
    ScopedHandle transient = OpenTransientHandle(
        ctx.actualPath, transientAccess, ctx.isDirectory);
    if (!transient.IsValid()) {
        return NtStatusFromWin32(firstErr);
    }
    if (!setInfo(transient.Get())) {
        return NtStatusFromWin32(::GetLastError());
    }
    return STATUS_SUCCESS;
}
}

bool LayerConfig::Validate(std::wstring& error) const {
    DWORD upperAttrs = GetFileAttributesW(upperPath.c_str());
    if (upperAttrs == INVALID_FILE_ATTRIBUTES) {
        error = L"Upper layer path does not exist: " + upperPath;
        return false;
    }
    if (!(upperAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        error = L"Upper layer path is not a directory: " + upperPath;
        return false;
    }

    std::wstring testFile = JoinDirPath(upperPath, L".layermount_write_test");
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

std::wstring NormalizePath(const std::wstring& path) {
    std::wstring result = NormalizePathPreserveCase(path);
    if (!result.empty()) {
        CharLowerBuffW(result.data(), static_cast<DWORD>(result.size()));
    }

    return result;
}

// Guard for every entry point that joins an untrusted ABI path onto a
// layer root. Windows canonicalizes the joined string, so a `..` segment
// escapes the root. On the write side, `CreateFileW`, `MoveFileExW` and
// `CreateDirectoryW` would then act outside the overlay. In directory
// enumeration, `FindFirstFileW` resolves the `..` and would list a
// directory outside the layer root. A colon anywhere in the path lets the
// caller inject a drive letter (`c:\escape`) or an alternate-data-stream
// suffix that names the wrong target.

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
        if (!IsSafeRelativePath(normalized)) {
            return false;
        }
        outHostNorm = normalized;
        return true;
    }

    std::wstring host = normalized.substr(0, firstColon);
    if (!IsSafeRelativePath(host)) {
        return false;
    }

    const size_t streamStart = firstColon + 1;
    const size_t secondColon = normalized.find(L':', streamStart);
    const size_t streamEnd =
        (secondColon == std::wstring::npos) ? normalized.size() : secondColon;
    const std::wstring streamName =
        normalized.substr(streamStart, streamEnd - streamStart);

    if (streamName.empty()) {
        return false;
    }
    if (streamName.find(L'\\') != std::wstring::npos) {
        return false;
    }
    if (IsReservedStreamName(streamName)) {
        return false;
    }

    std::wstring streamType;
    if (secondColon != std::wstring::npos) {
        streamType = normalized.substr(secondColon + 1);
        if (streamType.empty()) {
            return false;
        }
        if (streamType.find(L':') != std::wstring::npos) {
            return false;
        }
        if (::_wcsicmp(streamType.c_str(), L"$DATA") != 0) {
            return false;
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

namespace {

bool IsRootSidecarPath(const std::wstring& normalized) {
    return normalized == kSidecarDirName || IsInsideDirectory(normalized, kSidecarDirName);
}

bool HasMarkerSegment(std::wstring_view normalized) {
    size_t start = 0;
    while (true) {
        const size_t separator = normalized.find(L'\\', start);
        if (WhiteoutManager::IsWhiteoutName(
                normalized.substr(start, separator - start))) {
            return true;
        }
        if (separator == std::wstring_view::npos) {
            return false;
        }
        start = separator + 1;
    }
}

}

bool IsReservedRelativePath(const std::wstring& normalized) {
    return IsRootSidecarPath(normalized) || HasMarkerSegment(normalized);
}

bool EnsureDirectoryExists(const std::wstring& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return false;
    }
    return true;
}

LayerMount::LayerMount(LayerConfig config)
    : config_(std::move(config))
    , capabilities_(config_.hostCapabilities)
    , securityPolicy_(capabilities_)
    , events_()
    , cache_(std::make_unique<Cache>(config_.pathCacheCapacity))
    , whiteoutMgr_(std::make_unique<WhiteoutManager>(config_, cache_.get()))
    , pathResolver_(std::make_unique<PathResolver>(config_, *whiteoutMgr_, *cache_))
    , stats_()
    , copyUp_(std::make_unique<CopyUp>(config_, *pathResolver_, *whiteoutMgr_, *cache_, stats_)) {
    copyUp_->SetCapabilityGate(capabilities_);
    copyUp_->SetEventEmitter(&events_);
    whiteoutMgr_->SetEventEmitter(&events_);
    if (config_.enableProcessTracking) {
        processTracker_ = TryMakeProcessTracker();
        if (processTracker_ == nullptr) {
            throw std::system_error(static_cast<int>(ERROR_INVALID_DATA),
                                    std::system_category(),
                                    "The process rules file did not load");
        }
    }
}

std::shared_ptr<ProcessTracker> LayerMount::TryMakeProcessTracker() {
    auto tracker = std::make_shared<ProcessTracker>(config_.accessLogCapacity);
    tracker->SetEventEmitter(&events_);
    if (!config_.processRulesPath.empty() &&
        !tracker->LoadRules(config_.processRulesPath)) {
        return nullptr;
    }
    return tracker;
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
        vss_ = std::make_unique<VSS::VSSManager>();
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
        {
            std::shared_lock readLock(processTrackerMutex_);
            if (processTracker_ != nullptr) return S_OK;
        }
        auto candidate = TryMakeProcessTracker();
        if (candidate == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        }
        std::unique_lock writeLock(processTrackerMutex_);
        if (processTracker_ == nullptr) {
            processTracker_ = std::move(candidate);
        }
    } else {
        std::unique_lock writeLock(processTrackerMutex_);
        processTracker_.reset();
    }
    return S_OK;
}

NTSTATUS LayerMount::EnsureInUpperLayer(const std::wstring& relativePath,
                                        FileContext* ctx) {
    if (!ctx) {
        return STATUS_INVALID_PARAMETER;
    }

    // A valid, current handle proves the file exists in the upper. A
    // path-based ExistsInUpper check would report the file as missing
    // once another handle marks it delete-pending, although this handle
    // stays valid until it closes.
    if (ctx->writable &&
        ctx->handle != INVALID_HANDLE_VALUE &&
        !ctx->handleNeedsReopen) {
        return STATUS_SUCCESS;
    }

    std::wstring normalized = NormalizePath(relativePath);
    if (pathResolver_->ExistsInUpper(normalized)) {
        const std::wstring upperHostPath = pathResolver_->GetUpperPath(normalized);
        const std::wstring upperFullPath = upperHostPath + ctx->streamSuffix;
        // The upper entry keeps the lower's or the caller's case while
        // GetUpperPath is lowercase, so a case-sensitive compare would
        // reopen the handle on every call.
        if (::CompareStringOrdinal(ctx->actualPath.c_str(), static_cast<int>(ctx->actualPath.size()),
                                   upperFullPath.c_str(), static_cast<int>(upperFullPath.size()),
                                   TRUE) != CSTR_EQUAL) {
            ctx->actualPath = upperFullPath;
            ctx->writable = true;
            NTSTATUS reopenStatus = ReopenContextHandle(ctx);
            if (!NT_SUCCESS(reopenStatus)) {
                return reopenStatus;
            }
        }
        return STATUS_SUCCESS;
    }

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
    // WIN32_FILE_ATTRIBUTE_DATA has no change time.
    fileInfo->ChangeTime     = fileInfo->LastWriteTime;
    fileInfo->FileSize       = ComposeUInt64(attrData.nFileSizeHigh, attrData.nFileSizeLow);
    fileInfo->AllocationSize = AllocationSizeFor(fileInfo->FileSize);
    fileInfo->HardLinks      = 0;
    fileInfo->EaSize         = 0;
    fileInfo->IndexNumber    = 0;

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

ResolvedSizes LayerMount::SizesOf(const ResolvedPath& resolved) const {
    ResolvedSizes sizes;
    if (!resolved.Found()) {
        return sizes;
    }
    InternalFileInfo info{};
    if (!NT_SUCCESS(FillFileInfo(resolved.absolutePath, &info))) {
        return sizes;
    }
    sizes.fileSize       = info.FileSize;
    sizes.allocationSize = info.AllocationSize;
    return sizes;
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
    // BY_HANDLE_FILE_INFORMATION has no change time.
    fileInfo->ChangeTime     = fileInfo->LastWriteTime;
    fileInfo->FileSize       = ComposeUInt64(info.nFileSizeHigh, info.nFileSizeLow);
    UINT64 realAllocation = 0;
    FILE_STANDARD_INFO si{};
    if (::GetFileInformationByHandleEx(handle, FileStandardInfo, &si, sizeof(si))) {
        realAllocation = static_cast<UINT64>(si.AllocationSize.QuadPart);
    }
    fileInfo->AllocationSize = AllocationSizeFor(fileInfo->FileSize, realAllocation);
    fileInfo->HardLinks      = 0;
    fileInfo->EaSize         = 0;
    fileInfo->IndexNumber    = MakeIndexNumber(info);

    if (pathHint && !pathHint->empty()) {
        const LayerMountMetadata metadata = MetadataStore::ReadLayerMountMetadata(*pathHint, nullptr);
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

namespace {

std::wstring CaseFoldedName(const std::wstring& name) {
    std::wstring folded = name;
    CharLowerBuffW(folded.data(), static_cast<DWORD>(folded.size()));
    return folded;
}

std::optional<std::wstring> VisibleEntryKey(const std::wstring& dirNorm,
                                            const std::wstring& name) {
    if (name == L"." || name == L"..") return std::nullopt;
    if (WhiteoutManager::IsWhiteoutName(name)) return std::nullopt;
    std::wstring key = CaseFoldedName(name);
    if (dirNorm.empty() && IsReservedRelativePath(key)) return std::nullopt;
    return key;
}

struct DirectoryMerge {
    std::map<std::wstring, MergedEntry> entries;
    std::unordered_set<std::wstring> whitedOutNames;
};

// FindFirstFileW gives ERROR_DIRECTORY when the layer holds a file at the
// directory's path. The layer then has no directory there to list.
bool IsDirectoryAbsentError(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_DIRECTORY;
}

// Maps a failed scan's Win32 error to its NTSTATUS. An error that maps to a
// success status, such as ERROR_IO_PENDING, gives STATUS_UNSUCCESSFUL, so an
// incomplete scan never counts as complete.
NTSTATUS ScanFailureStatus(DWORD error) {
    const NTSTATUS status = NtStatusFromWin32(error);
    return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
}

struct LayerDirectoryEntry {
    std::wstring key;
    WIN32_FIND_DATAW findData;
};

struct LayerDirectoryScan {
    NTSTATUS status;
    std::vector<std::wstring> whitedOutNames;
    std::vector<LayerDirectoryEntry> entries;
};

// Reads one layer's directory in one enumeration. whitedOutNames holds the
// case-folded names that the layer's whiteouts hide, and entries holds the
// layer's visible entries. A directory absent from the layer gives
// STATUS_SUCCESS and no names. A failed scan gives the status of its Win32
// error and holds what it read before the failure.
LayerDirectoryScan ScanLayerDirectory(const std::wstring& layerPath,
                                      const std::wstring& dirNorm) {
    LayerDirectoryScan scan{STATUS_SUCCESS, {}, {}};
    WIN32_FIND_DATAW findData;
    const std::wstring searchPath = JoinLayerScanPath(layerPath, dirNorm);
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        const DWORD openError = ::GetLastError();
        if (!IsDirectoryAbsentError(openError)) {
            scan.status = ScanFailureStatus(openError);
        }
        return scan;
    }

    do {
        const std::wstring name = findData.cFileName;
        if (const std::optional<std::wstring> hidden =
                WhiteoutManager::WhitedOutNameOfEntry(name)) {
            scan.whitedOutNames.push_back(CaseFoldedName(*hidden));
            continue;
        }
        const std::optional<std::wstring> key = VisibleEntryKey(dirNorm, name);
        if (!key) continue;
        scan.entries.push_back(LayerDirectoryEntry{*key, findData});
    } while (FindNextFileW(hFind, &findData));

    // FindNextFileW returns false both at the end of the directory and on a
    // failure. FindClose can overwrite the error, so read it first.
    const DWORD scanEndError = ::GetLastError();
    FindClose(hFind);
    if (scanEndError != ERROR_NO_MORE_FILES) scan.status = ScanFailureStatus(scanEndError);
    return scan;
}

void AddLayerWhiteouts(const LayerDirectoryScan& scan, DirectoryMerge& merge) {
    merge.whitedOutNames.insert(scan.whitedOutNames.begin(), scan.whitedOutNames.end());
}

// Adds the layer's entries in the directory that no higher layer lists and
// that no whiteout hides, and adds the names that the layer's whiteouts hide
// to merge.whitedOutNames. A lower's whiteouts hide its own entries too; the
// upper's whiteouts hide only the lowers' entries. On success, sets
// lowersBelow to whether the lowers below the layer can add entries to the
// directory. A failed scan adds nothing, leaves lowersBelow unset, and
// returns the scan's status.
NTSTATUS MergeLayerEntries(const LayerDirectory& dir,
                           LayerSource source,
                           DirectoryMerge& merge,
                           LowerVisibility& lowersBelow) {
    const LayerDirectoryScan scan = ScanLayerDirectory(dir.layerPath, dir.dirNorm);
    if (!NT_SUCCESS(scan.status)) return scan.status;

    // An upper entry wins over an upper whiteout of the same name.
    if (source == LayerSource::Lower) AddLayerWhiteouts(scan, merge);
    for (const LayerDirectoryEntry& entry : scan.entries) {
        if (merge.entries.count(entry.key) || merge.whitedOutNames.count(entry.key)) continue;
        merge.entries[entry.key] = MergedEntry{entry.findData, source};
    }
    if (source == LayerSource::Upper) AddLayerWhiteouts(scan, merge);

    lowersBelow = LowersBelow(dir);
    return STATUS_SUCCESS;
}

}

MergedDirectory LayerMount::MergeDirectoryEntries(const std::wstring& dirRelativePath) const {
    const std::wstring dirNorm = NormalizePath(dirRelativePath);

    if (!dirNorm.empty() && !IsSafeRelativePath(dirNorm)) {
        return MergedDirectory{STATUS_SUCCESS, {}};
    }
    if (IsReservedRelativePath(dirNorm)) {
        return MergedDirectory{STATUS_SUCCESS, {}};
    }

    DirectoryMerge merge;
    LowerVisibility lowersBelow;
    const NTSTATUS upperStatus = MergeLayerEntries(
        LayerDirectory{*whiteoutMgr_, config_.upperPath, dirNorm}, LayerSource::Upper,
        merge, lowersBelow);
    if (!NT_SUCCESS(upperStatus)) {
        return MergedDirectory{upperStatus, {}};
    }

    for (size_t lower = 0; lower < config_.lowerPaths.size(); ++lower) {
        if (lowersBelow != LowerVisibility::Visible) break;
        if (HasLinkUnderHigherLayerEntry(config_, lower, dirNorm)) break;
        const NTSTATUS lowerStatus = MergeLayerEntries(
            LayerDirectory{*whiteoutMgr_, config_.lowerPaths[lower], dirNorm}, LayerSource::Lower,
            merge, lowersBelow);
        if (!NT_SUCCESS(lowerStatus)) {
            return MergedDirectory{lowerStatus, {}};
        }
    }

    return MergedDirectory{STATUS_SUCCESS, std::move(merge.entries)};
}

namespace {

inline UINT32 MapFileGenericRights(UINT32 access) {
    UINT32 mapped = access & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
    if (access & GENERIC_READ)    mapped |= FILE_GENERIC_READ;
    if (access & GENERIC_WRITE)   mapped |= FILE_GENERIC_WRITE;
    if (access & GENERIC_EXECUTE) mapped |= FILE_GENERIC_EXECUTE;
    if (access & GENERIC_ALL)     mapped |= FILE_ALL_ACCESS;
    return mapped;
}

inline bool HasWriteAccess(UINT32 access) {
    return (MapFileGenericRights(access) &
            (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES |
             FILE_WRITE_EA | WRITE_DAC | WRITE_OWNER)) != 0;
}

inline bool HasFileDataAccess(UINT32 access) {
    return (MapFileGenericRights(access) &
            (FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_EXECUTE)) != 0;
}

// A file system serves paging reads on a write-only file object: the cache
// manager reads the page at the new end of file after a truncation, and
// faults a page in before a partial cached write. The access check for the
// caller happened at open. So a handle with a write-data or append-data
// right also carries the read-data right that a file system has for its
// own reads.
UINT32 ComputePhysicalHandleAccess(UINT32 grantedAccess) {
    UINT32 access = MapFileGenericRights(grantedAccess);
    if ((access & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0) {
        access |= FILE_READ_DATA;
    }
    return access;
}

typedef NTSTATUS(NTAPI* NtQueryObjectFn)(
    HANDLE Handle,
    OBJECT_INFORMATION_CLASS ObjectInformationClass,
    PVOID ObjectInformation,
    ULONG ObjectInformationLength,
    PULONG ReturnLength);

NtQueryObjectFn GetNtQueryObject() {
    static NtQueryObjectFn fn = LoadNtdllExport<NtQueryObjectFn>("NtQueryObject");
    return fn;
}

// The kernel resolves MAXIMUM_ALLOWED with an access check before a
// filesystem sees the request, so only a direct caller of the engine can
// pass the bit. A handle opened with MAXIMUM_ALLOWED carries the kernel's
// answer as its granted access.
NTSTATUS ResolveMaximumAllowedFromHandle(HANDLE handle,
                                         UINT32 requestedAccess,
                                         UINT32* resolvedAccess) {
    if ((requestedAccess & MAXIMUM_ALLOWED) == 0) {
        *resolvedAccess = requestedAccess;
        return STATUS_SUCCESS;
    }
    NtQueryObjectFn queryObject = GetNtQueryObject();
    if (queryObject == nullptr) {
        return STATUS_NOT_SUPPORTED;
    }
    PUBLIC_OBJECT_BASIC_INFORMATION info{};
    ULONG returnLength = 0;
    NTSTATUS status = queryObject(handle, ObjectBasicInformation,
                                  &info, sizeof(info), &returnLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *resolvedAccess =
        (requestedAccess & ~static_cast<UINT32>(MAXIMUM_ALLOWED)) | info.GrantedAccess;
    return STATUS_SUCCESS;
}

// A probe open with MAXIMUM_ALLOWED gets the same answer from the same
// kernel check, under the same token as the real open. An AccessCheck
// call against the file's DACL would need an impersonation token and
// would repeat the kernel's work.
NTSTATUS ResolveMaximumAllowed(const std::wstring& physicalPath,
                               bool isDirectory,
                               UINT32 requestedAccess,
                               UINT32* resolvedAccess) {
    if ((requestedAccess & MAXIMUM_ALLOWED) == 0) {
        *resolvedAccess = requestedAccess;
        return STATUS_SUCCESS;
    }
    ScopedHandle probe =
        OpenTransientHandle(physicalPath, MAXIMUM_ALLOWED, isDirectory);
    if (!probe.IsValid()) {
        return NtStatusFromWin32(::GetLastError());
    }
    return ResolveMaximumAllowedFromHandle(probe.Get(), requestedAccess, resolvedAccess);
}

UINT32 ComputeHandleReopenAccess(const FileContext& ctx) {
    UINT32 reopenAccess =
        ComputePhysicalHandleAccess(ctx.grantedAccess) & ~static_cast<UINT32>(DELETE);
    if (reopenAccess == 0) {
        reopenAccess = FILE_READ_ATTRIBUTES;
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

void CloseContextHandle(FileContext* ctx) {
    if (ctx->handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(ctx->handle);
        ctx->handle = INVALID_HANDLE_VALUE;
    }
}

// Replaces MAXIMUM_ALLOWED in the context's granted access with the
// rights the kernel granted to its handle. On failure the handle closes;
// the caller removes what it made.
NTSTATUS ResolveContextMaximumAllowed(FileContext* ctx, UINT32 requestedAccess) {
    NTSTATUS status = ResolveMaximumAllowedFromHandle(
        ctx->handle, requestedAccess, &ctx->grantedAccess);
    if (!NT_SUCCESS(status)) {
        CloseContextHandle(ctx);
    }
    return status;
}

NTSTATUS CleanupContextHandle(FileContext* ctx) {
    if (ctx == nullptr) return STATUS_INVALID_HANDLE;
    CloseContextHandle(ctx);
    ctx->handleNeedsReopen = true;
    return STATUS_SUCCESS;
}

NTSTATUS OpenContextHandleWithAccess(FileContext* ctx, UINT32 accessMask) {
    if (ctx == nullptr) return STATUS_INVALID_PARAMETER;

    CloseContextHandle(ctx);

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

// The delete takes effect when the last handle on the file or stream
// closes, so the close here is part of the delete, not a cleanup.
NTSTATUS SetDeleteDispositionAndClose(HANDLE handle) {
    FILE_DISPOSITION_INFO disp{};
    disp.DeleteFileW = TRUE;
    const BOOL ok = ::SetFileInformationByHandle(
        handle, FileDispositionInfo, &disp, sizeof(disp));
    const DWORD setErr = ok ? 0 : ::GetLastError();
    ::CloseHandle(handle);
    if (!ok) {
        return NtStatusFromWin32(setErr);
    }
    return STATUS_SUCCESS;
}

// SetKernelObjectSecurity fails if its SECURITY_INFORMATION names an owner
// or group that sd does not carry. A DACL or SACL bit for an ACL that sd
// does not carry replaces the ACL that the new object inherited.
SECURITY_INFORMATION SecurityInformationCarriedBy(PSECURITY_DESCRIPTOR sd) {
    SECURITY_INFORMATION carried = 0;
    if (sd == nullptr) {
        return carried;
    }
    PSID owner = nullptr;
    BOOL ownerDefaulted = FALSE;
    if (::GetSecurityDescriptorOwner(sd, &owner, &ownerDefaulted) && owner != nullptr) {
        carried |= OWNER_SECURITY_INFORMATION;
    }
    PSID group = nullptr;
    BOOL groupDefaulted = FALSE;
    if (::GetSecurityDescriptorGroup(sd, &group, &groupDefaulted) && group != nullptr) {
        carried |= GROUP_SECURITY_INFORMATION;
    }
    BOOL daclPresent = FALSE;
    PACL dacl = nullptr;
    BOOL daclDefaulted = FALSE;
    if (::GetSecurityDescriptorDacl(sd, &daclPresent, &dacl, &daclDefaulted) && daclPresent) {
        carried |= DACL_SECURITY_INFORMATION;
    }
    BOOL saclPresent = FALSE;
    PACL sacl = nullptr;
    BOOL saclDefaulted = FALSE;
    if (::GetSecurityDescriptorSacl(sd, &saclPresent, &sacl, &saclDefaulted) && saclPresent) {
        carried |= SACL_SECURITY_INFORMATION;
    }
    return carried;
}

// Not SetFileSecurityW: it checks the object's DACL for WRITE_DAC and
// WRITE_OWNER, so it fails with ACCESS_DENIED on a new child under a
// protected parent DACL that does not grant them. When the process holds
// SE_RESTORE_NAME, a backup-semantics handle can write any owner, group or
// DACL on the new object. A SACL also needs SE_SECURITY_NAME and
// ACCESS_SYSTEM_SECURITY on the handle.
NTSTATUS WriteSecurityToNewObject(const std::wstring& path, PSECURITY_DESCRIPTOR sd) {
    const SECURITY_INFORMATION carried = SecurityInformationCarriedBy(sd);
    // Without SE_SECURITY_NAME the create still succeeds and writes the
    // owner, group and DACL, as GetSecurity still returns them.
    const SECURITY_INFORMATION toWrite = DropSaclWithoutPrivilege(carried);
    if (toWrite == 0) {
        return STATUS_SUCCESS;
    }
    // An overlay that has not built a CopyUp yet has no SE_RESTORE_NAME, and
    // the open below then fails with ACCESS_DENIED under such a parent.
    EnableFileSystemPrivileges();
    DWORD access = READ_CONTROL | WRITE_DAC | WRITE_OWNER;
    if ((toWrite & SACL_SECURITY_INFORMATION) != 0) {
        access |= ACCESS_SYSTEM_SECURITY;
    }
    ScopedHandle securityHandle{::CreateFileW(path.c_str(),
        access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (!securityHandle.IsValid()) {
        return NtStatusFromWin32(::GetLastError());
    }
    if (!::SetKernelObjectSecurity(securityHandle.Get(), toWrite, sd)) {
        return NtStatusFromWin32(::GetLastError());
    }
    return STATUS_SUCCESS;
}

// Closes the context handle and deletes ctx->actualPath on scope exit while
// armed. A file create arms it after CreateFileW makes the entry and
// disarms it on success, so a failure in between removes only the new file
// or stream. DeleteFileW on a file:stream path removes only the stream.
class FileRollback {
public:
    explicit FileRollback(FileContext* ctx) : ctx_(ctx) {}
    ~FileRollback() {
        if (armed_) {
            CloseContextHandle(ctx_);
            ::DeleteFileW(ctx_->actualPath.c_str());
        }
    }
    FileRollback(const FileRollback&) = delete;
    FileRollback& operator=(const FileRollback&) = delete;

    void Arm() { armed_ = true; }
    void Disarm() { armed_ = false; }

private:
    FileContext* ctx_;
    bool armed_ = false;
};

}

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
        if (!tracker->CheckAccess(callerPid, hostNorm, openOp)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    if (normalized.empty()) {
        return OpenRoot(grantedAccess, createOptions, callerPid, outCtx, outInfo);
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(hostNorm);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    const bool hostIsDirectory =
        (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!streamSuffix.empty() && hostIsDirectory) {
        return STATUS_FILE_IS_A_DIRECTORY;
    }

    UINT32 resolvedAccess = 0;
    NTSTATUS resolveStatus = ResolveMaximumAllowed(
        resolved.absolutePath, hostIsDirectory, grantedAccess, &resolvedAccess);
    if (!NT_SUCCESS(resolveStatus)) {
        return resolveStatus;
    }

    auto ctx = std::make_unique<FileContext>();
    ctx->relativePath = hostNorm;
    ctx->streamSuffix = streamSuffix;
    ctx->isDirectory = hostIsDirectory;
    ctx->ownerPid = callerPid;
    ctx->grantedAccess = resolvedAccess;
    ctx->createOptions = createOptions;

    if (resolved.source == LayerSource::Lower && HasWriteAccess(resolvedAccess)) {
        NTSTATUS status = CopyUpForWriteOpen(hostNorm, resolved, ctx.get());
        if (!NT_SUCCESS(status)) {
            return status;
        }
        ctx->actualPath = pathResolver_->GetUpperPath(hostNorm) + streamSuffix;
        ctx->writable = true;
    } else if (resolved.source == LayerSource::Upper &&
               HasWriteAccess(resolvedAccess) &&
               !streamSuffix.empty()) {
        const std::wstring upperHostPath = pathResolver_->GetUpperPath(hostNorm);
        const LayerMountMetadata metadata =
            MetadataStore::ReadLayerMountMetadata(upperHostPath, &config_);
        // A later fill copies the lower's streams over the stream this open writes.
        if (metadata.metacopy) {
            NTSTATUS cpStatus = FillShell(hostNorm, ctx.get());
            if (!NT_SUCCESS(cpStatus)) {
                return cpStatus;
            }
        }
        ctx->actualPath = upperHostPath + streamSuffix;
        ctx->writable = true;
    } else {
        ctx->actualPath = resolved.absolutePath + streamSuffix;
        ctx->writable = (resolved.source == LayerSource::Upper);
    }

    if (resolved.source == LayerSource::Upper && streamSuffix.empty()) {
        LayerMountMetadata metadata =
            MetadataStore::ReadLayerMountMetadata(ctx->actualPath, &config_);
        ctx->isMetacopyOnly = metadata.metacopy;
    }

    if (ctx->isMetacopyOnly && !ctx->isDirectory && HasFileDataAccess(resolvedAccess)) {
        NTSTATUS fillStatus = FillShell(hostNorm, ctx.get());
        if (!NT_SUCCESS(fillStatus)) {
            return fillStatus;
        }
    }

    DWORD flags = ctx->isDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0;
    if (createOptions & FILE_OPEN_REPARSE_POINT) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }
    ctx->handle = ::CreateFileW(ctx->actualPath.c_str(),
        ComputePhysicalHandleAccess(resolvedAccess),
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

NTSTATUS LayerMount::OpenRoot(UINT32 grantedAccess,
                             UINT32 createOptions,
                             DWORD callerPid,
                             std::unique_ptr<FileContext>* outCtx,
                             InternalFileInfo* outInfo) {
    auto ctx = std::make_unique<FileContext>();
    ctx->actualPath = config_.upperPath;
    ctx->isDirectory = true;
    ctx->writable = true;
    ctx->ownerPid = callerPid;
    ctx->createOptions = createOptions;

    ctx->handle = ::CreateFileW(config_.upperPath.c_str(),
        ComputePhysicalHandleAccess(grantedAccess),
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (ctx->handle == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }

    NTSTATUS resolveStatus = ResolveContextMaximumAllowed(ctx.get(), grantedAccess);
    if (!NT_SUCCESS(resolveStatus)) {
        return resolveStatus;
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

NTSTATUS LayerMount::CopyUpForWriteOpen(const std::wstring& hostNorm,
                                       const ResolvedPath& resolved,
                                       FileContext* ctx) {
    if (ctx->isDirectory) {
        return copyUp_->CopyUpDirectory(hostNorm);
    }
    if (!ctx->streamSuffix.empty()) {
        return copyUp_->CopyUpFile(hostNorm);
    }

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
    // A metacopy shell needs a sparse upper file. When the upper layer has
    // no sparse support, the shell becomes a dense file of zeros, so the
    // copy-up copies the data instead.
    if (srcSize.QuadPart > kMetacopyThresholdBytes && capabilities_.HasSparseFiles()) {
        NTSTATUS status = copyUp_->CopyUpMetadataOnly(hostNorm);
        if (NT_SUCCESS(status)) {
            ctx->isMetacopyOnly = true;
        }
        return status;
    }
    return copyUp_->CopyUpFile(hostNorm);
}

namespace {

bool IsDirectoryHit(const ResolvedPath& hit) {
    return hit.Found() && (hit.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsDirectoryCreate(const LayerMount::CreateRequest& request) {
    return (request.createOptions & FILE_DIRECTORY_FILE) != 0;
}

bool StreamExists(const std::wstring& hostPath, const std::wstring& streamSuffix) {
    const std::wstring streamPath = hostPath + streamSuffix;
    return ::GetFileAttributesW(streamPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool MetacopyOriginHasStream(const ResolvedPath& host,
                             const std::wstring& streamSuffix,
                             const LayerConfig& config) {
    if (host.source != LayerSource::Upper) {
        return false;
    }
    const LayerMountMetadata metadata =
        MetadataStore::ReadLayerMountMetadata(host.absolutePath, &config);
    return metadata.metacopy && !metadata.originLayer.empty() &&
        StreamExists(metadata.originLayer, streamSuffix);
}

bool HostFileHasStream(const ResolvedPath& host,
                       const std::wstring& streamSuffix,
                       const LayerConfig& config) {
    return StreamExists(host.absolutePath, streamSuffix) ||
        MetacopyOriginHasStream(host, streamSuffix, config);
}

}

std::optional<StreamPath> ParseStreamPath(const std::wstring& relativePath) {
    StreamPath path;
    if (!TryParseStreamPath(NormalizePathPreserveCase(relativePath),
                            path.callerHost, path.callerStreamSuffix)) {
        return std::nullopt;
    }
    path.hostNorm = CaseFoldedName(path.callerHost);
    path.streamSuffix = CaseFoldedName(path.callerStreamSuffix);
    return path;
}

NTSTATUS LayerMount::Create(const CreateRequest& request,
                           std::unique_ptr<FileContext>* outCtx,
                           InternalFileInfo* outInfo) {
    if (outCtx == nullptr || outInfo == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    *outCtx = nullptr;

    std::optional<StreamPath> path = ParseStreamPath(request.relativePath);
    if (!path) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    UpperCreate create;
    create.path = std::move(*path);

    CreateResolution resolution;
    const NTSTATUS precondition = CheckCreatePreconditions(request, create, &resolution);
    if (!NT_SUCCESS(precondition)) {
        return precondition;
    }

    create.lowerIsDirectory = IsDirectoryHit(resolution.lower);
    create.lowerIsVisible = resolution.overlayHit.Found() &&
        resolution.overlayHit.source == LayerSource::Lower;

    create.upperPath =
        pathResolver_->GetUpperPathForNewEntry(CallerPath(create.path.callerHost));

    const NTSTATUS parentStatus = copyUp_->EnsureUpperParent(CallerPath(request.relativePath));
    if (!NT_SUCCESS(parentStatus)) {
        return parentStatus;
    }

    std::unique_ptr<FileContext> ctx = BuildCreate(request, &create);

    const NTSTATUS createStatus = IsDirectoryCreate(request)
        ? CreateDirectoryInUpper(create, ctx.get())
        : CreateFileInUpper(create, ctx.get());
    if (!NT_SUCCESS(createStatus)) {
        return createStatus;
    }

    // The whiteout goes only after the create succeeds. Removing it
    // earlier would show the lower entry again if the create failed.
    if (resolution.whiteoutAtPath) {
        whiteoutMgr_->RemoveWhiteout(create.path.hostNorm);
    }

    cache_->InvalidateWithAncestors(create.path.hostNorm);

    NTSTATUS status = FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath);
    if (!NT_SUCCESS(status)) {
        ::CloseHandle(ctx->handle);
        return status;
    }

    stats_.activeHandles.fetch_add(1, std::memory_order_relaxed);
    *outCtx = std::move(ctx);
    return STATUS_SUCCESS;
}

std::unique_ptr<FileContext> LayerMount::BuildCreate(const CreateRequest& request,
                                                     UpperCreate* create) const {
    create->grantedAccess = request.grantedAccess;
    create->fileAttributes = request.fileAttributes;
    create->securityDescriptor = securityPolicy_.DescriptorForCreate(request.securityDescriptor);
    create->allocationSize = request.allocationSize;

    auto ctx = std::make_unique<FileContext>();
    ctx->relativePath = create->path.hostNorm;
    ctx->streamSuffix = create->path.streamSuffix;
    ctx->actualPath = create->upperPath + create->path.callerStreamSuffix;
    ctx->isDirectory = IsDirectoryCreate(request);
    ctx->writable = true;
    ctx->ownerPid = request.callerPid;
    ctx->createOptions = request.createOptions;
    return ctx;
}

NTSTATUS LayerMount::CheckCreatePreconditions(const CreateRequest& request,
                                              const UpperCreate& create,
                                              CreateResolution* resolution) const {
    if (IsReservedRelativePath(create.path.hostNorm)) {
        return STATUS_ACCESS_DENIED;
    }

    if (auto tracker = Tracker(); tracker && request.callerPid != 0) {
        if (!tracker->CheckAccess(request.callerPid, create.path.hostNorm, OperationType::Create)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    if (IsDirectoryCreate(request) && !create.path.streamSuffix.empty()) {
        return STATUS_FILE_IS_A_DIRECTORY;
    }

    *resolution = pathResolver_->ResolveForCreate(create.path.hostNorm);
    if (create.path.streamSuffix.empty()) {
        return resolution->overlayHit.Found() ? STATUS_OBJECT_NAME_COLLISION : STATUS_SUCCESS;
    }

    const std::wstring upperHostPath = pathResolver_->GetUpperPath(create.path.hostNorm);
    const DWORD upperAttrs = ::GetFileAttributesW(upperHostPath.c_str());
    const bool upperIsDirectory = upperAttrs != INVALID_FILE_ATTRIBUTES &&
        (upperAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (upperIsDirectory || IsDirectoryHit(resolution->overlayHit)) {
        return STATUS_FILE_IS_A_DIRECTORY;
    }
    if (resolution->overlayHit.Found() &&
        HostFileHasStream(resolution->overlayHit, create.path.streamSuffix, config_)) {
        return STATUS_OBJECT_NAME_COLLISION;
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::CreateDirectoryInUpper(const UpperCreate& create,
                                            FileContext* ctx) {
    DirectoryRollback rollback(create.upperPath);
    if (!::CreateDirectoryW(create.upperPath.c_str(), nullptr)) {
        return NtStatusFromWin32(::GetLastError());
    }
    rollback.Arm();

    if (create.lowerIsDirectory) {
        if (!whiteoutMgr_->SetOpaque(create.path.hostNorm)) {
            DWORD err = ::GetLastError();
            return NtStatusFromWin32(err != ERROR_SUCCESS ? err : ERROR_ACCESS_DENIED);
        }
    }

    NTSTATUS sdStatus = WriteSecurityToNewObject(create.upperPath, create.securityDescriptor);
    if (!NT_SUCCESS(sdStatus)) {
        return sdStatus;
    }

    ctx->handle = ::CreateFileW(create.upperPath.c_str(),
        ComputePhysicalHandleAccess(create.grantedAccess),
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (ctx->handle == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        return NtStatusFromWin32(err);
    }
    NTSTATUS resolveStatus = ResolveContextMaximumAllowed(ctx, create.grantedAccess);
    if (!NT_SUCCESS(resolveStatus)) {
        return resolveStatus;
    }
    rollback.Disarm();
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::CreateFileInUpper(const UpperCreate& create,
                                       FileContext* ctx) {
    if (!create.path.streamSuffix.empty()) {
        NTSTATUS hostStatus = PrepareStreamHost(create, ctx);
        if (!NT_SUCCESS(hostStatus)) {
            return hostStatus;
        }
    }

    const UINT32 fileAttributes =
        create.fileAttributes != 0 ? create.fileAttributes : FILE_ATTRIBUTE_NORMAL;
    ctx->handle = ::CreateFileW(ctx->actualPath.c_str(),
        ComputePhysicalHandleAccess(create.grantedAccess),
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_NEW, fileAttributes, nullptr);
    if (ctx->handle == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }
    FileRollback rollback(ctx);
    rollback.Arm();

    NTSTATUS resolveStatus = ResolveContextMaximumAllowed(ctx, create.grantedAccess);
    if (!NT_SUCCESS(resolveStatus)) {
        return resolveStatus;
    }

    if (create.path.streamSuffix.empty()) {
        NTSTATUS sdStatus = WriteSecurityToNewObject(create.upperPath, create.securityDescriptor);
        if (!NT_SUCCESS(sdStatus)) {
            return sdStatus;
        }

        if (create.allocationSize > 0) {
            FILE_ALLOCATION_INFO allocInfo;
            allocInfo.AllocationSize.QuadPart = static_cast<LONGLONG>(create.allocationSize);
            if (!::SetFileInformationByHandle(ctx->handle, FileAllocationInfo,
                                               &allocInfo, sizeof(allocInfo))) {
                return NtStatusFromWin32(::GetLastError());
            }
        }
    }
    rollback.Disarm();
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::PrepareStreamHost(const UpperCreate& create, FileContext* ctx) {
    if (!pathResolver_->ExistsInUpper(create.path.hostNorm)) {
        if (create.lowerIsVisible) {
            return copyUp_->CopyUpFile(create.path.hostNorm);
        }
        return STATUS_SUCCESS;
    }
    const LayerMountMetadata metadata =
        MetadataStore::ReadLayerMountMetadata(create.upperPath, &config_);
    if (metadata.metacopy) {
        return FillShell(create.path.hostNorm, ctx);
    }
    return STATUS_SUCCESS;
}

void LayerMount::Close(FileContext* ctx) {
    if (ctx == nullptr) {
        return;
    }

    if (auto tracker = Tracker(); tracker && ctx->ownerPid != 0) {
        tracker->LogAccess(ctx->ownerPid, ctx->relativePath, OperationType::Close);
    }

    CloseContextHandle(ctx);

    stats_.activeHandles.fetch_sub(1, std::memory_order_relaxed);
}

NTSTATUS LayerMount::Cleanup(FileContext* ctx) {
    return CleanupContextHandle(ctx);
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

NTSTATUS LayerMount::FillShell(const std::wstring& hostNorm, FileContext* ctx) {
    NTSTATUS status = copyUp_->CompleteLazyCopyUp(hostNorm);
    if (!NT_SUCCESS(status)) return status;
    ctx->isMetacopyOnly = false;
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::EnsureMetacopyMaterialized(FileContext* ctx) {
    if (!ctx->isMetacopyOnly) {
        return STATUS_SUCCESS;
    }
    NTSTATUS status = FillShell(ctx->relativePath, ctx);
    if (!NT_SUCCESS(status)) return status;
    return ReopenContextHandle(ctx);
}

namespace {

// A host hands the buffer of a paging read back to the memory manager as
// a page. Without the fill, the part past the end of the file keeps stale
// memory and a mapped file shows it.
void ZeroFillTail(void* buffer, ULONG filled, ULONG length) {
    if (filled >= length) return;
    memset(static_cast<BYTE*>(buffer) + filled, 0, length - filled);
}

}

NTSTATUS LayerMount::Read(FileContext* ctx,
                         void* buffer,
                         UINT64 offset,
                         ULONG length,
                         PULONG bytesTransferred) {
    // A paging read can arrive here. A copy-up or a handle reopen for a
    // fill inside this call breaks the mapping the memory manager holds.
    if (ctx == nullptr) return STATUS_INVALID_HANDLE;
    NTSTATUS ready = EnsureHandleReady(ctx);
    if (!NT_SUCCESS(ready)) return ready;
    if (bytesTransferred == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    if (auto tracker = Tracker(); tracker && ctx->ownerPid != 0) {
        if (!tracker->CheckAccess(ctx->ownerPid, ctx->relativePath, OperationType::Read)) {
            return STATUS_ACCESS_DENIED;
        }
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

    if (*bytesTransferred == 0 && length != 0) {
        return STATUS_END_OF_FILE;
    }
    ZeroFillTail(buffer, *bytesTransferred, length);

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
                          PULONG bytesTransferred,
                          InternalFileInfo* outInfo) {
    if (ctx == nullptr) return STATUS_INVALID_HANDLE;
    NTSTATUS ready = EnsureHandleReady(ctx);
    if (!NT_SUCCESS(ready)) return ready;
    if (bytesTransferred == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    if (auto tracker = Tracker(); tracker && ctx->ownerPid != 0) {
        if (!tracker->CheckAccess(ctx->ownerPid, ctx->relativePath, OperationType::Write)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    NTSTATUS status = EnsureInUpperLayer(ctx->relativePath, ctx);
    if (!NT_SUCCESS(status)) return status;

    NTSTATUS metacopyStatus = EnsureMetacopyMaterialized(ctx);
    if (!NT_SUCCESS(metacopyStatus)) return metacopyStatus;

    LARGE_INTEGER fileSize{};
    if (!::GetFileSizeEx(ctx->handle, &fileSize)) {
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

}

NTSTATUS LayerMount::Overwrite(FileContext* ctx,
                              UINT32 fileAttributes,
                              BOOLEAN replaceAttributes,
                              UINT64 allocationSize,
                              InternalFileInfo* outInfo) {
    if (ctx == nullptr) return STATUS_INVALID_HANDLE;
    NTSTATUS ready = EnsureHandleReady(ctx);
    if (!NT_SUCCESS(ready)) return ready;

    if (auto tracker = Tracker(); tracker && ctx->ownerPid != 0) {
        if (!tracker->CheckAccess(ctx->ownerPid, ctx->relativePath, OperationType::Overwrite)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    NTSTATUS status = EnsureInUpperLayer(ctx->relativePath, ctx);
    if (!NT_SUCCESS(status)) return status;

    const bool isStreamHandle = !ctx->streamSuffix.empty();

    if (!isStreamHandle) {
        // Skipped for stream handles: `ctx->actualPath` carries the stream
        // suffix, so FindFirstStreamW would enumerate the *host* file's
        // streams and DeleteUserAlternateDataStreams would wipe every sibling stream
        // alongside the one the caller meant to truncate. NTFS overwrite
        // semantics target the open stream only -- the kernel-level
        // SetFileInformationByHandle below truncates the stream's data
        // without touching siblings.
        status = DeleteUserAlternateDataStreams(ctx->actualPath);
        if (!NT_SUCCESS(status)) return status;
    }

    FILE_END_OF_FILE_INFO eofInfo{};
    NTSTATUS sizeStatus = SetInfoWithTransientRetry(
        *ctx, FILE_WRITE_DATA, [&](HANDLE h) {
            return ::SetFileInformationByHandle(
                h, FileEndOfFileInfo, &eofInfo, sizeof(eofInfo)) != FALSE;
        });
    if (!NT_SUCCESS(sizeStatus)) return sizeStatus;

    if (allocationSize > 0) {
        FILE_ALLOCATION_INFO allocInfo{};
        allocInfo.AllocationSize.QuadPart = static_cast<LONGLONG>(allocationSize);
        NTSTATUS allocStatus = SetInfoWithTransientRetry(
            *ctx, FILE_WRITE_DATA, [&](HANDLE h) {
                return ::SetFileInformationByHandle(
                    h, FileAllocationInfo, &allocInfo, sizeof(allocInfo)) != FALSE;
            });
        if (!NT_SUCCESS(allocStatus)) return allocStatus;
    }

    if (fileAttributes != 0) {
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
                          InternalFileInfo* outInfo) {
    if (ctx == nullptr) return STATUS_INVALID_HANDLE;
    NTSTATUS ready = EnsureHandleReady(ctx);
    if (!NT_SUCCESS(ready)) return ready;

    if (auto tracker = Tracker(); tracker && ctx->ownerPid != 0) {
        if (!tracker->CheckAccess(ctx->ownerPid, ctx->relativePath, OperationType::Read)) {
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

NTSTATUS LayerMount::DirectoryEmptinessStatus(const std::wstring& dirNorm) const {
    const MergedDirectory merged = MergeDirectoryEntries(dirNorm);
    if (!NT_SUCCESS(merged.status)) {
        return merged.status;
    }
    if (!merged.entries.empty()) {
        return STATUS_DIRECTORY_NOT_EMPTY;
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::CheckRenameDestination(const RenamePaths& paths,
                                            BOOLEAN replaceIfExists,
                                            RenameKinds* kinds) const {
    const ResolvedPath destResolved = pathResolver_->ResolvePath(paths.newNorm);
    if (destResolved.Found() && !replaceIfExists) {
        return STATUS_OBJECT_NAME_COLLISION;
    }
    if (kinds->source != RenameEntryKind::File &&
        IsInsideDirectory(paths.newNorm, paths.oldNorm)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!destResolved.Found()) {
        return STATUS_SUCCESS;
    }
    if (IsInsideDirectory(paths.oldNorm, paths.newNorm)) {
        return STATUS_DIRECTORY_NOT_EMPTY;
    }
    RenameEntryKind destinationKind = RenameEntryKind::File;
    const NTSTATUS kindStatus =
        EntryKindOf(destResolved.absolutePath, destResolved.attributes, &destinationKind);
    if (!NT_SUCCESS(kindStatus)) {
        return kindStatus;
    }
    kinds->destination = destinationKind;
    const bool sourceIsDirectory = kinds->source == RenameEntryKind::Directory;
    const bool destinationIsDirectory = destinationKind == RenameEntryKind::Directory;
    if (sourceIsDirectory != destinationIsDirectory) {
        return sourceIsDirectory ? STATUS_NOT_A_DIRECTORY : STATUS_FILE_IS_A_DIRECTORY;
    }
    if (sourceIsDirectory) {
        return DirectoryEmptinessStatus(paths.newNorm);
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
            return STATUS_FILE_IS_A_DIRECTORY;
        }
        if (!pathResolver_->ExistsInUpper(hostNorm)) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        return STATUS_SUCCESS;
    }

    if (isDirectory) {
        return DirectoryEmptinessStatus(hostNorm);
    }

    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::CanDelete(FileContext* ctx) {
    if (ctx == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    std::wstring normalized = NormalizePath(ctx->relativePath);

    if (auto tracker = Tracker(); tracker && ctx->ownerPid != 0) {
        if (!tracker->CheckAccess(ctx->ownerPid, normalized, OperationType::Delete)) {
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
        return DirectoryEmptinessStatus(normalized);
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
        NTSTATUS status = SetDeleteDispositionAndClose(h);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        cache_->InvalidateWithAncestors(hostNorm);
        return STATUS_SUCCESS;
    }

    ResolvedPath resolved = pathResolver_->ResolvePath(hostNorm);
    const bool isDirectory = resolved.Found() &&
        (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ResolvedPath lowerResolved = pathResolver_->ResolveLowerPath(hostNorm);
    const bool lowerHasIt = lowerResolved.Found();

    const std::wstring upperPath = pathResolver_->GetUpperPath(hostNorm);
    if (::GetFileAttributesW(upperPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (isDirectory && whiteoutMgr_->IsOpaque(normalized)) {
            whiteoutMgr_->RemoveOpaque(normalized);
        }
        const NTSTATUS removal = RemoveUpperEntry(upperPath);
        if (!NT_SUCCESS(removal)) {
            cache_->InvalidateWithAncestors(normalized);
            return removal;
        }
    }

    if (lowerHasIt) {
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

NTSTATUS LayerMount::DeleteStreamOnContext(FileContext* ctx) {
    if (ctx->handle == INVALID_HANDLE_VALUE) {
        // The context's reopen mask drops DELETE, which the delete disposition needs.
        NTSTATUS reopenStatus = OpenContextHandleWithAccess(ctx, DELETE | SYNCHRONIZE);
        if (!NT_SUCCESS(reopenStatus)) {
            return reopenStatus;
        }
    }
    const HANDLE streamHandle = ctx->handle;
    ctx->handle = INVALID_HANDLE_VALUE;
    ctx->handleNeedsReopen = false;
    NTSTATUS status = SetDeleteDispositionAndClose(streamHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    cache_->InvalidateWithAncestors(ctx->relativePath);
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::Delete(FileContext* ctx) {
    if (ctx == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS canDelete = CanDelete(ctx);
    if (!NT_SUCCESS(canDelete)) {
        return canDelete;
    }

    if (!ctx->streamSuffix.empty()) {
        return DeleteStreamOnContext(ctx);
    }

    std::wstring normalized = NormalizePath(ctx->relativePath);
    ResolvedPath resolved = pathResolver_->ResolvePath(normalized);
    const bool isDirectory = resolved.Found() &&
        (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ResolvedPath lowerResolved = pathResolver_->ResolveLowerPath(normalized);
    const bool lowerHasIt = lowerResolved.Found();

    CloseContextHandle(ctx);
    ctx->handleNeedsReopen = false;

    const std::wstring upperPath = pathResolver_->GetUpperPath(normalized);
    const DWORD upperAttrs = ::GetFileAttributesW(upperPath.c_str());
    if (upperAttrs != INVALID_FILE_ATTRIBUTES) {
        if ((upperAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            whiteoutMgr_->IsOpaque(normalized)) {
            whiteoutMgr_->RemoveOpaque(normalized);
        }
        const NTSTATUS removal = RemoveUpperEntry(upperPath);
        if (!NT_SUCCESS(removal)) {
            cache_->InvalidateWithAncestors(normalized);
            return removal;
        }
    }

    if (lowerHasIt) {
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

NTSTATUS LayerMount::RenameFileInUpper(const std::wstring& oldRelativePath,
                                       const std::wstring& newRelativePath,
                                       BOOLEAN replaceIfExists,
                                       bool destHadWhiteout) {
    const std::wstring oldNorm = NormalizePath(oldRelativePath);
    const std::wstring newNorm = NormalizePath(newRelativePath);
    const bool lowerHasSource = pathResolver_->ResolveLowerPath(oldNorm).Found();

    // Before CopyUpFile, so a rename into a hidden parent fails without copying the source up.
    NTSTATUS status = copyUp_->EnsureUpperParent(CallerPath(newRelativePath));
    if (!NT_SUCCESS(status)) return status;

    if (!pathResolver_->ExistsInUpper(oldNorm)) {
        status = copyUp_->CopyUpFile(oldNorm);
        if (!NT_SUCCESS(status)) return status;
    }

    const std::wstring oldUpperPath = pathResolver_->GetStoredUpperPath(oldRelativePath);
    const std::wstring newUpperPath =
        pathResolver_->GetUpperPathForNewEntry(CallerPath(newRelativePath));

    status = MoveUpperEntry(
        oldUpperPath, newUpperPath,
        replaceIfExists ? ReplaceExisting::Yes : ReplaceExisting::No,
        CopyAcrossVolumes::No);
    if (!NT_SUCCESS(status)) return status;

    if (lowerHasSource) {
        return WhiteOutRenameSource(RenamePaths{oldNorm, newNorm}, WhiteoutType::File,
                                    oldUpperPath, destHadWhiteout);
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::WhiteOutRenameSource(const RenamePaths& paths,
                                          WhiteoutType type,
                                          const std::wstring& oldUpperPath,
                                          bool destHadWhiteout) {
    if (whiteoutMgr_->CreateWhiteout(paths.oldNorm, type)) {
        return STATUS_SUCCESS;
    }
    const DWORD whErr = ::GetLastError();
    const NTSTATUS moveBack = MoveUpperEntry(pathResolver_->GetStoredUpperPath(paths.newNorm),
                                             oldUpperPath, ReplaceExisting::No,
                                             CopyAcrossVolumes::No);
    if (!NT_SUCCESS(moveBack) && destHadWhiteout) {
        whiteoutMgr_->RemoveWhiteout(paths.newNorm);
    }
    cache_->InvalidateWithAncestors(paths.oldNorm);
    cache_->InvalidateWithAncestors(paths.newNorm);
    return whErr ? NtStatusFromWin32(whErr) : STATUS_ACCESS_DENIED;
}

NTSTATUS LayerMount::DirectoryRenameRouteOf(const std::wstring& oldNorm,
                                            RenameEntryKind sourceKind,
                                            DirectoryRenameRoute* route) const {
    const ResolvedPath lower = pathResolver_->ResolveLowerPath(oldNorm);
    if (!lower.Found()) {
        *route = DirectoryRenameRoute::MoveUpper;
        return STATUS_SUCCESS;
    }
    if (!pathResolver_->ExistsInUpper(oldNorm)) {
        *route = DirectoryRenameRoute::MergeLower;
        return STATUS_SUCCESS;
    }
    if (sourceKind != RenameEntryKind::Directory || whiteoutMgr_->IsOpaque(oldNorm)) {
        *route = DirectoryRenameRoute::MoveUpperAndWhiteout;
        return STATUS_SUCCESS;
    }
    RenameEntryKind lowerKind = RenameEntryKind::File;
    const NTSTATUS status = EntryKindOf(lower.absolutePath, lower.attributes, &lowerKind);
    if (!NT_SUCCESS(status)) return status;
    *route = lowerKind == RenameEntryKind::Directory
        ? DirectoryRenameRoute::MergeLower
        : DirectoryRenameRoute::MoveUpperAndWhiteout;
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::CheckRenameRequest(const RenamePaths& paths, DWORD callerPid) {
    std::wstring oldHostNorm, oldStreamSuffix;
    std::wstring newHostNorm, newStreamSuffix;
    if (!TryParseStreamPath(paths.oldNorm, oldHostNorm, oldStreamSuffix) ||
        !TryParseStreamPath(paths.newNorm, newHostNorm, newStreamSuffix)) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    if (!oldStreamSuffix.empty() || !newStreamSuffix.empty()) {
        return STATUS_INVALID_PARAMETER;
    }

    if (IsReservedRelativePath(paths.oldNorm) || IsReservedRelativePath(paths.newNorm)) {
        return STATUS_ACCESS_DENIED;
    }

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, paths.oldNorm, OperationType::Rename)) {
            return STATUS_ACCESS_DENIED;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::Rename(const std::wstring& oldRelativePath,
                           const std::wstring& newRelativePath,
                           BOOLEAN replaceIfExists,
                           DWORD callerPid) {
    const std::wstring oldNorm = NormalizePath(oldRelativePath);
    const std::wstring newNorm = NormalizePath(newRelativePath);
    const RenamePaths paths{oldNorm, newNorm};

    const NTSTATUS requestStatus = CheckRenameRequest(paths, callerPid);
    if (!NT_SUCCESS(requestStatus)) return requestStatus;

    const ResolvedPath sourceResolved = pathResolver_->ResolvePath(oldNorm);
    if (!sourceResolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (NormalizePathPreserveCase(oldRelativePath) ==
        NormalizePathPreserveCase(newRelativePath)) {
        return STATUS_SUCCESS;
    }
    RenameKinds kinds{RenameEntryKind::File, std::nullopt};
    NTSTATUS status =
        EntryKindOf(sourceResolved.absolutePath, sourceResolved.attributes, &kinds.source);
    if (!NT_SUCCESS(status)) return status;
    if (oldNorm != newNorm) {
        status = CheckRenameDestination(paths, replaceIfExists, &kinds);
        if (!NT_SUCCESS(status)) return status;
    }
    return RenameCheckedEntry(oldRelativePath, newRelativePath, replaceIfExists, kinds);
}

NTSTATUS LayerMount::RenameCheckedEntry(const std::wstring& oldRelativePath,
                                        const std::wstring& newRelativePath,
                                        BOOLEAN replaceIfExists,
                                        const RenameKinds& kinds) {
    const std::wstring oldNorm = NormalizePath(oldRelativePath);
    const std::wstring newNorm = NormalizePath(newRelativePath);
    const bool isSameLogicalPath = oldNorm == newNorm;
    const bool isDirectory = kinds.source != RenameEntryKind::File;

    NTSTATUS status = STATUS_SUCCESS;
    DirectoryRenameRoute route = DirectoryRenameRoute::MoveUpper;
    if (isDirectory && !isSameLogicalPath) {
        status = DirectoryRenameRouteOf(oldNorm, kinds.source, &route);
        if (!NT_SUCCESS(status)) return status;
    }

    RenameDestinationAside destinationAside;
    if (replaceIfExists && kinds.destination.has_value()) {
        status = copyUp_->SetRenameDestinationAside(newNorm, kinds.source,
                                                    *kinds.destination,
                                                    &destinationAside);
        if (!NT_SUCCESS(status)) return status;
    }

    const bool destHadWhiteout =
        whiteoutMgr_->HasWhiteout(newNorm, config_.upperPath);

    if (isDirectory && isSameLogicalPath) {
        status = copyUp_->RenameDirectoryCase(CallerPath(oldRelativePath),
                                              CallerPath(newRelativePath));
    } else if (isDirectory) {
        status = RenameDirectoryEntry(oldRelativePath, newRelativePath, kinds.source, route,
                                      replaceIfExists, destHadWhiteout);
    } else {
        status = RenameFileInUpper(oldRelativePath, newRelativePath, replaceIfExists,
                                   destHadWhiteout);
    }
    if (!NT_SUCCESS(status)) return status;
    destinationAside.Commit();

    if (destHadWhiteout) {
        whiteoutMgr_->RemoveWhiteout(newNorm);
    }

    cache_->InvalidateWithAncestors(oldNorm);
    cache_->InvalidateWithAncestors(newNorm);
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::RenameDirectoryEntry(const std::wstring& oldRelativePath,
                                          const std::wstring& newRelativePath,
                                          RenameEntryKind sourceKind,
                                          DirectoryRenameRoute route,
                                          BOOLEAN replaceIfExists,
                                          bool destHadWhiteout) {
    const std::wstring oldNorm = NormalizePath(oldRelativePath);
    const std::wstring newNorm = NormalizePath(newRelativePath);
    const CallerPath oldCallerPath(oldRelativePath);
    const CallerPath newCallerPath(newRelativePath);
    const ReplaceExisting replace =
        replaceIfExists ? ReplaceExisting::Yes : ReplaceExisting::No;
    if (route == DirectoryRenameRoute::MoveUpper) {
        return copyUp_->RenameUpperDirectory(oldCallerPath, newCallerPath, sourceKind, replace);
    }

    // The name that the move back in WhiteOutRenameSource gives the entry.
    const std::wstring oldUpperPath = pathResolver_->ExistsInUpper(oldNorm)
        ? pathResolver_->GetStoredUpperPath(oldRelativePath)
        : pathResolver_->GetUpperPathForCopyUp(oldNorm, pathResolver_->ResolveLowerPath(oldNorm));
    const NTSTATUS status = route == DirectoryRenameRoute::MergeLower
        ? copyUp_->RenameLowerDirectory(oldCallerPath, newCallerPath, sourceKind, replace)
        : copyUp_->RenameUpperDirectory(oldCallerPath, newCallerPath, sourceKind, replace);
    if (!NT_SUCCESS(status)) return status;
    return WhiteOutRenameSource(RenamePaths{oldNorm, newNorm}, WhiteoutType::Directory,
                                oldUpperPath, destHadWhiteout);
}

NTSTATUS LayerMount::Rename(FileContext* ctx,
                           const std::wstring& newRelativePath,
                           BOOLEAN replaceIfExists,
                           DWORD callerPid) {
    if (ctx == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    const std::wstring oldRelativePath = ctx->relativePath;
    if (NormalizePathPreserveCase(oldRelativePath) ==
        NormalizePathPreserveCase(newRelativePath)) {
        return Rename(oldRelativePath, newRelativePath, replaceIfExists, callerPid);
    }
    const std::wstring oldNorm = NormalizePath(oldRelativePath);
    const std::wstring newNorm = NormalizePath(newRelativePath);
    const RenamePaths paths{oldNorm, newNorm};

    const NTSTATUS requestStatus = CheckRenameRequest(paths, callerPid);
    if (!NT_SUCCESS(requestStatus)) return requestStatus;

    const ResolvedPath sourceResolved = pathResolver_->ResolvePath(oldNorm);
    if (!sourceResolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    RenameKinds kinds{RenameEntryKind::File, std::nullopt};
    NTSTATUS status =
        EntryKindOf(sourceResolved.absolutePath, sourceResolved.attributes, &kinds.source);
    if (!NT_SUCCESS(status)) return status;
    if (oldNorm != newNorm) {
        status = CheckRenameDestination(paths, replaceIfExists, &kinds);
        if (!NT_SUCCESS(status)) return status;
    }
    const bool sourceWasInUpper = pathResolver_->ExistsInUpper(oldNorm);
    const std::wstring oldActualPath = ctx->actualPath;

    if (ctx->handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(ctx->handle);
        ctx->handle = INVALID_HANDLE_VALUE;
    }

    status = RenameCheckedEntry(oldRelativePath, newRelativePath, replaceIfExists, kinds);
    if (!NT_SUCCESS(status)) {
        ctx->actualPath = oldActualPath;
        ctx->handleNeedsReopen = true;
        return status;
    }

    ctx->relativePath = newNorm;
    ctx->actualPath = pathResolver_->GetUpperPathForNewEntry(CallerPath(newRelativePath));
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
    std::wstring newRelativeHostPreserved;
    if (newStreamSuffix.empty()) {
        newRelativeHostPreserved = newRelativePath;
    } else {
        // newHostNorm is lowercase, so the host is cut from the caller's case.
        const std::wstring preserved = NormalizePathPreserveCase(newRelativePath);
        newRelativeHostPreserved =
            preserved.substr(0, preserved.length() - newStreamSuffix.length());
    }
    ctx->actualPath        = pathResolver_->GetUpperPathForNewEntry(
                                 CallerPath(newRelativeHostPreserved))
                              + newStreamSuffix;
    ctx->handleNeedsReopen = true;
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::GetSecurity(const std::wstring& relativePath,
                                UINT32 securityInformation,
                                PUINT32 outAttributes,
                                PSECURITY_DESCRIPTOR sd,
                                SIZE_T sdBytes,
                                SIZE_T* requiredBytes) {
    const SECURITY_INFORMATION effective = DropSaclWithoutPrivilege(securityInformation);

    std::wstring normalized = NormalizePath(relativePath);

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

    const bool isProbe = (sd == nullptr || sdBytes == 0);

    if (effective == 0) {
        if (requiredBytes != nullptr) {
            *requiredBytes = 0;
        }
        return STATUS_SUCCESS;
    }

    if (!securityPolicy_.ReadsUpperSecurity()) {
        return SecurityPolicy::SyntheticSecurity(effective, isProbe, sd, sdBytes, requiredBytes);
    }

    // Bits outside owner, group, DACL, and SACL pass through to
    // ::GetFileSecurityW unexamined.
    DWORD needed = 0;
    BOOL ok = ::GetFileSecurityW(targetPath.c_str(), effective,
                                  sd, static_cast<DWORD>(sdBytes), &needed);
    if (requiredBytes != nullptr) {
        *requiredBytes = needed;
    }
    if (!ok) {
        DWORD err = ::GetLastError();
        if (err == ERROR_INSUFFICIENT_BUFFER) {
            return isProbe ? STATUS_SUCCESS : STATUS_BUFFER_OVERFLOW;
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

    if (!securityPolicy_.AppliesSecurityWrites()) {
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

}

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

namespace {

// The handle-based set survives a delete-pending mark on the upper file,
// which a path-based ::SetFileAttributesW does not. Zero timestamps in
// FILE_BASIC_INFO mean no change, so the call sets only the attributes.
NTSTATUS SetContextAttributes(const FileContext& ctx, UINT32 fileAttributes) {
    FILE_BASIC_INFO bi{};
    bi.FileAttributes = fileAttributes;
    return SetInfoWithTransientRetry(
        ctx, FILE_WRITE_ATTRIBUTES, [&](HANDLE h) {
            return ::SetFileInformationByHandle(
                h, FileBasicInfo, &bi, sizeof(bi)) != FALSE;
        });
}

FILETIME ToFileTime(UINT64 value) {
    FILETIME ft{};
    ft.dwLowDateTime  = static_cast<DWORD>(value);
    ft.dwHighDateTime = static_cast<DWORD>(value >> 32);
    return ft;
}

// A zero time leaves that timestamp unchanged.
NTSTATUS SetContextTimes(const FileContext& ctx,
                         UINT64 creationTime,
                         UINT64 lastAccessTime,
                         UINT64 lastWriteTime) {
    FILETIME ct = ToFileTime(creationTime);
    FILETIME at = ToFileTime(lastAccessTime);
    FILETIME wt = ToFileTime(lastWriteTime);
    return SetInfoWithTransientRetry(
        ctx, FILE_WRITE_ATTRIBUTES, [&](HANDLE h) {
            return ::SetFileTime(h,
                                 creationTime   ? &ct : nullptr,
                                 lastAccessTime ? &at : nullptr,
                                 lastWriteTime  ? &wt : nullptr) != FALSE;
        });
}

NTSTATUS SetContextAllocationSize(const FileContext& ctx, UINT64 allocationSize) {
    FILE_ALLOCATION_INFO info{};
    info.AllocationSize.QuadPart = static_cast<LONGLONG>(allocationSize);
    return SetInfoWithTransientRetry(
        ctx, FILE_WRITE_DATA, [&](HANDLE h) {
            return ::SetFileInformationByHandle(
                h, FileAllocationInfo, &info, sizeof(info)) != FALSE;
        });
}

NTSTATUS SetContextEndOfFile(const FileContext& ctx, UINT64 fileSize) {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(fileSize);
    return SetInfoWithTransientRetry(
        ctx, FILE_WRITE_DATA, [&](HANDLE h) {
            return ::SetFileInformationByHandle(
                h, FileEndOfFileInfo, &info, sizeof(info)) != FALSE;
        });
}

}

NTSTATUS LayerMount::SetInfo(FileContext* ctx,
                            const SetInfoRequest& request,
                            InternalFileInfo* outInfo) {
    if (ctx == nullptr) {
        return STATUS_INVALID_HANDLE;
    }
    if (auto tracker = Tracker(); tracker && ctx->ownerPid != 0) {
        if (!tracker->CheckAccess(ctx->ownerPid, ctx->relativePath, OperationType::SetInfo)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    NTSTATUS status = EnsureInUpperLayer(ctx->relativePath, ctx);
    if (!NT_SUCCESS(status)) return status;

    // After the copy-up, or a stale handle reopens on the lower and then again on the upper.
    NTSTATUS ready = EnsureHandleReady(ctx);
    if (!NT_SUCCESS(ready)) return ready;

    constexpr UINT64 kUnchanged = UINT64_MAX;

    // FILE_ALLOCATION_INFO / FILE_END_OF_FILE_INFO take signed LONGLONG;
    // an unsigned size above LLONG_MAX becomes negative at the WinAPI
    // boundary and produces either undefined filesystem behavior or a
    // misleading STATUS_INVALID_PARAMETER far from the real fault. Reject
    // explicit sizes (kUnchanged stays as the unchanged sentinel).
    constexpr UINT64 kMaxSignedSize = static_cast<UINT64>(LLONG_MAX);
    if (request.allocationSize != kUnchanged && request.allocationSize > kMaxSignedSize) {
        return STATUS_INVALID_PARAMETER;
    }
    if (request.fileSize != kUnchanged && request.fileSize > kMaxSignedSize) {
        return STATUS_INVALID_PARAMETER;
    }

    // After the size checks and before the size change, which the fill resets to the origin's size.
    if (request.allocationSize != kUnchanged || request.fileSize != kUnchanged) {
        NTSTATUS metacopyStatus = EnsureMetacopyMaterialized(ctx);
        if (!NT_SUCCESS(metacopyStatus)) return metacopyStatus;
    }

    if (request.fileAttributes != INVALID_FILE_ATTRIBUTES) {
        NTSTATUS s = SetContextAttributes(*ctx, request.fileAttributes);
        if (!NT_SUCCESS(s)) return s;
    }

    if (request.creationTime || request.lastAccessTime || request.lastWriteTime) {
        NTSTATUS s = SetContextTimes(*ctx, request.creationTime, request.lastAccessTime,
                                     request.lastWriteTime);
        if (!NT_SUCCESS(s)) return s;
    }

    if (request.allocationSize != kUnchanged) {
        NTSTATUS s = SetContextAllocationSize(*ctx, request.allocationSize);
        if (!NT_SUCCESS(s)) return s;
    }
    if (request.fileSize != kUnchanged) {
        NTSTATUS s = SetContextEndOfFile(*ctx, request.fileSize);
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
}

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

    std::unique_ptr<void, decltype(&::FindClose)> findGuard(h, &::FindClose);

    do {
        if (IsReservedFullNtfsStreamName(findData.cStreamName)) {
            continue;
        }
        InternalStreamInfo info;
        info.name = findData.cStreamName;
        info.streamSize = static_cast<UINT64>(findData.StreamSize.QuadPart);
        // The find-stream data carries no allocation size, so the rounded
        // stream size is the only value this producer can report.
        info.allocationSize = AllocationSizeFor(info.streamSize);
        out.push_back(std::move(info));
    } while (::FindNextStreamW(h, &findData));

    DWORD lastErr = ::GetLastError();
    if (lastErr != ERROR_HANDLE_EOF && lastErr != ERROR_SUCCESS) {
        return NtStatusFromWin32(lastErr);
    }
    return STATUS_SUCCESS;
}

}
