#include "LayerMount.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "MetadataStore.h"
#include "Cache.h"
#include "LayerPath.h"
#include "CopyUp.h"
#include "DirectoryMerge.h"
#include "DirectoryRename.h"
#include "FileRename.h"
#include "RenameRollback.h"
#include "RenameLinkBoundary.h"
#include "UpperEntryRemover.h"
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
#include <optional>
#include <string_view>
#include <system_error>

namespace LayerMount {

namespace {
NTSTATUS ReopenContextHandle(FileContext* ctx);

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

namespace {

// wideSerial stays empty when the file system refuses the FileIdInfo query.
struct VolumeSerial {
    DWORD serial = 0;
    std::optional<ULONGLONG> wideSerial;
};

// The open follows a mounted folder, so the serial numbers are those of the
// mounted volume.
bool ReadVolumeSerial(const std::wstring& path, VolumeSerial* volume) {
    ScopedHandle directory(::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                         nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!directory.IsValid() || !::GetFileInformationByHandle(directory.Get(), &info)) {
        return false;
    }
    volume->serial = info.dwVolumeSerialNumber;
    FILE_ID_INFO idInfo{};
    if (::GetFileInformationByHandleEx(directory.Get(), FileIdInfo, &idInfo, sizeof(idInfo))) {
        volume->wideSerial = idInfo.VolumeSerialNumber;
    }
    return true;
}

// Two volumes can share the 32-bit serial, so the 64-bit one decides when
// both file systems report it.
bool OnOneVolume(const VolumeSerial& a, const VolumeSerial& b) {
    if (a.wideSerial && b.wideSerial) {
        return *a.wideSerial == *b.wideSerial;
    }
    return a.serial == b.serial;
}

}

HRESULT LayerConfig::Prepare(std::wstring& error) {
    if (workDirPath.empty()) {
        error = L"Work directory path is empty";
        return E_FAIL;
    }

    if (!EnsureDirectoryExists(workDirPath)) {
        error = L"Failed to create work directory: " + workDirPath;
        return E_FAIL;
    }

    VolumeSerial upperVolume;
    if (!ReadVolumeSerial(upperPath, &upperVolume)) {
        error = L"Failed to read the volume of the upper layer: " + upperPath;
        return E_FAIL;
    }
    VolumeSerial workVolume;
    if (!ReadVolumeSerial(workDirPath, &workVolume)) {
        error = L"Failed to read the volume of the work directory: " + workDirPath;
        return E_FAIL;
    }
    // A copy-up moves an entry from the work directory into the upper with
    // one rename, which only works within one volume.
    if (!OnOneVolume(upperVolume, workVolume)) {
        error = L"Work directory is not on the volume of the upper layer: " + workDirPath;
        return E_INVALIDARG;
    }

    return S_OK;
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

namespace {

bool StreamNamesEqual(std::wstring_view a, std::wstring_view b) noexcept {
    return a.size() == b.size() && a.size() <= INT_MAX &&
           ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                  b.data(), static_cast<int>(b.size()),
                                  TRUE) == CSTR_EQUAL;
}

// `streamName` is the bare name, without the leading colon and without a
// `:$TYPE` suffix.
bool IsReservedStreamName(std::wstring_view streamName) noexcept {
    const std::wstring_view reserved = std::wstring_view(kLayerMountADSStream).substr(1);
    if (streamName.size() == reserved.size()) {
        return StreamNamesEqual(streamName, reserved);
    }
    return streamName.size() > reserved.size() &&
           streamName[reserved.size()] == L'.' &&
           StreamNamesEqual(streamName.substr(0, reserved.size()), reserved);
}

}

bool IsUserAlternateStream(std::wstring_view ntfsStreamName) noexcept {
    if (ntfsStreamName.empty() || ntfsStreamName.front() != L':') {
        return false;
    }
    const std::wstring_view afterColon = ntfsStreamName.substr(1);
    const std::wstring_view name = afterColon.substr(0, afterColon.find(L':'));
    return !name.empty() && !IsReservedStreamName(name);
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

bool IsRootSidecarPath(const std::wstring& normalized) {
    return normalized == kSidecarDirName || IsInsideDirectory(normalized, kSidecarDirName);
}

std::optional<std::wstring_view> ParentOfFirstMarkerSegment(std::wstring_view normalized) {
    size_t start = 0;
    while (true) {
        const size_t separator = normalized.find(L'\\', start);
        if (WhiteoutManager::IsWhiteoutName(
                normalized.substr(start, separator - start))) {
            return start == 0 ? std::wstring_view() : normalized.substr(0, start - 1);
        }
        if (separator == std::wstring_view::npos) {
            return std::nullopt;
        }
        start = separator + 1;
    }
}

bool IsReservedRelativePath(const std::wstring& normalized) {
    return IsRootSidecarPath(normalized) || ParentOfFirstMarkerSegment(normalized).has_value();
}

bool EnsureDirectoryExists(const std::wstring& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        return false;
    }
    return true;
}

namespace {

// The path that GetFinalPathNameByHandleW gives for handle, in extended
// form. Returns an empty path when the path cannot be read.
std::wstring FinalPathNameOf(HANDLE handle) {
    constexpr DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD size = ::GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (size == 0) {
        return {};
    }
    std::wstring path(size, L'\0');
    const DWORD length = ::GetFinalPathNameByHandleW(handle, path.data(), size, flags);
    if (length == 0 || length >= size) {
        return {};
    }
    path.resize(length);
    return path;
}

// The final path of the directory at path. The open follows a junction or a
// directory symbolic link anywhere in path. Returns an empty path when the
// directory cannot be opened or its final path cannot be read.
std::wstring FinalPathOfDirectory(const std::wstring& path) {
    const HANDLE directory = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::wstring finalPath = FinalPathNameOf(directory);
    ::CloseHandle(directory);
    return finalPath;
}

}

LayerMount::LayerMount(LayerConfig config)
    : config_(std::move(config))
    , upperFinalPath_(FinalPathOfDirectory(config_.upperPath))
    , securityPolicy_(config_.Capabilities())
    , events_()
    , cache_(std::make_unique<Cache>(config_.pathCacheCapacity))
    , whiteoutMgr_(std::make_unique<WhiteoutManager>(config_, cache_.get()))
    , pathResolver_(std::make_unique<PathResolver>(config_, *whiteoutMgr_, *cache_))
    , stats_()
    , copyUp_(std::make_unique<CopyUp>(config_, *pathResolver_, *whiteoutMgr_, *cache_, stats_))
    , directoryRename_(std::make_unique<DirectoryRename>(
          config_, *pathResolver_, *whiteoutMgr_, *cache_, *copyUp_, events_))
    , fileRename_(std::make_unique<FileRename>(config_, *pathResolver_, *copyUp_, events_))
    , renameRollback_(std::make_unique<RenameRollback>(
          config_, *pathResolver_, *whiteoutMgr_, *cache_, events_))
    , upperEntryRemover_(std::make_unique<UpperEntryRemover>(
          config_, *pathResolver_, *whiteoutMgr_, *cache_)) {
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

ViewLookup LayerMount::WalkCallerPath(const std::wstring& callerPath,
                                      FinalLink finalLink) const {
    return pathResolver_->ViewPathThroughLinks(callerPath, finalLink);
}

NTSTATUS LayerMount::EnsureInUpperLayer(const std::wstring& relativePath) {
    const ViewLookup view = WalkCallerPath(relativePath, FinalLink::Keep);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    const std::wstring& normalized = view.path.Normalized();
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

namespace {

// Whether path is root or below it. The compare ignores case, and below
// means past a separator, so "C:\ab" is not below "C:\a".
bool IsAtOrBelow(const std::wstring& path, const std::wstring& root) {
    if (root.empty() || path.size() < root.size() ||
        ::CompareStringOrdinal(path.c_str(), static_cast<int>(root.size()),
                               root.c_str(), static_cast<int>(root.size()),
                               TRUE) != CSTR_EQUAL) {
        return false;
    }
    return path.size() == root.size() || root.back() == L'\\' || path[root.size()] == L'\\';
}

// The path of the entry that handle is on, in the form of upperPath, so the
// sidecar lookup finds a record keyed by that path. A final path at or below
// upperFinalPath, the final path of the upper root, goes onto upperPath, so
// an 8.3 name, a substituted drive or a junction in upperPath still matches.
// Any other final path only loses its extended-form prefix when upperPath
// has none. Returns an empty path when the handle's path cannot be read.
std::wstring FinalPathOf(HANDLE handle,
                         const std::wstring& upperPath,
                         const std::wstring& upperFinalPath) {
    std::wstring path = FinalPathNameOf(handle);
    if (path.empty()) {
        return {};
    }

    if (IsAtOrBelow(path, upperFinalPath)) {
        std::wstring_view below(path);
        below.remove_prefix(upperFinalPath.size());
        while (!below.empty() && below.front() == L'\\') {
            below.remove_prefix(1);
        }
        return below.empty() ? upperPath : JoinDirPath(upperPath, std::wstring(below));
    }

    constexpr std::wstring_view kExtendedPrefix = L"\\\\?\\";
    constexpr std::wstring_view kExtendedUncPrefix = L"\\\\?\\UNC\\";
    if (upperPath.rfind(kExtendedPrefix, 0) == 0) {
        return path;
    }
    if (path.rfind(kExtendedUncPrefix, 0) == 0) {
        return L"\\\\" + path.substr(kExtendedUncPrefix.size());
    }
    if (path.rfind(kExtendedPrefix, 0) == 0) {
        return path.substr(kExtendedPrefix.size());
    }
    return path;
}

// The path whose copy-up record gives the stable ID of a handle opened at
// pathHint. A handle on the entry at pathHint, a link opened as a link
// included, takes that entry's record. A handle without the reparse
// attribute, opened at a reparse point, followed the link to its target and
// takes the target's record. Returns an empty path when the target's path
// cannot be read.
std::wstring RecordPathOf(HANDLE handle,
                          DWORD handleAttributes,
                          const std::wstring& pathHint,
                          bool pathHintIsReparsePoint,
                          const std::wstring& upperPath,
                          const std::wstring& upperFinalPath) {
    if ((handleAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || !pathHintIsReparsePoint) {
        return pathHint;
    }
    return FinalPathOf(handle, upperPath, upperFinalPath);
}

}

NTSTATUS LayerMount::FillFileInfoFromHandle(HANDLE handle,
                                            InternalFileInfo* fileInfo,
                                            const std::wstring* pathHint,
                                            bool pathHintIsReparsePoint) const {
    memset(fileInfo, 0, sizeof(*fileInfo));

    BY_HANDLE_FILE_INFORMATION info;
    if (!handleInfoQuery_(handle, &info)) {
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
        const std::wstring recordPath = RecordPathOf(handle, info.dwFileAttributes, *pathHint,
                                                     pathHintIsReparsePoint, config_.upperPath,
                                                     upperFinalPath_);
        if (!recordPath.empty()) {
            const LayerMountMetadata metadata =
                MetadataStore::ReadLayerMountMetadata(recordPath, &config_);
            if (metadata.hasStableIndexNumber) {
                fileInfo->IndexNumber = metadata.stableIndexNumber;
            }
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

MergedDirectory LayerMount::MergeDirectoryEntries(const std::wstring& dirRelativePath) const {
    const ViewLookup view = WalkCallerPath(dirRelativePath, FinalLink::Follow);
    if (!NT_SUCCESS(view.status)) {
        return MergedDirectory{view.status, {}};
    }
    return MergeDirectoryAcrossLayers(config_, *whiteoutMgr_, view.path.Path());
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

// Deletes a file or a stream that a create made. A delete refuses a
// read-only file, and a create can give its new file
// FILE_ATTRIBUTE_READONLY, so a refused delete clears that attribute and
// tries again. On failure, leaves the last error of the failed call.
bool DeleteCreatedFile(const std::wstring& path) {
    if (::DeleteFileW(path.c_str())) {
        return true;
    }
    if (::GetLastError() != ERROR_ACCESS_DENIED) {
        return false;
    }
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_READONLY) == 0) {
        ::SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    return ::SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY) &&
           ::DeleteFileW(path.c_str());
}

// The HRESULT of the last error of the Win32 call that just failed, or
// E_FAIL when that call left the last error at 0.
HRESULT HresultOfFailedCall() {
    const DWORD err = ::GetLastError();
    return err == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(err);
}

// Closes the context handle on scope exit while armed and removes what the
// create made: the new file, or the new stream and the host file passed to
// AlsoDeleteHost. A stream create that made its host file passes it,
// because DeleteFileW on a file:stream path removes only the stream. A
// step of the undo that fails emits an LM_EVT_WARNING event with
// ctx->relativePath.
class FileRollback {
public:
    FileRollback(FileContext* ctx, const abi::EventEmitter& events)
        : ctx_(ctx), events_(events) {}
    ~FileRollback() {
        if (!armed_) {
            return;
        }
        CloseContextHandle(ctx_);
        if (!DeleteCreatedFile(ctx_->actualPath)) {
            events_.Emit(LM_EVT_WARNING, HresultOfFailedCall(), ctx_->relativePath.c_str(),
                         L"The undo of a failed create could not delete the new entry");
        }
        if (createdHostPath_ && !DeleteCreatedFile(*createdHostPath_)) {
            events_.Emit(LM_EVT_WARNING, HresultOfFailedCall(), ctx_->relativePath.c_str(),
                         L"The undo of a failed stream create could not delete its new host file");
        }
    }
    FileRollback(const FileRollback&) = delete;
    FileRollback& operator=(const FileRollback&) = delete;

    void Arm() { armed_ = true; }
    void Disarm() { armed_ = false; }
    void AlsoDeleteHost(const std::wstring& hostPath) { createdHostPath_ = hostPath; }

private:
    FileContext* ctx_;
    const abi::EventEmitter& events_;
    std::optional<std::wstring> createdHostPath_;
    bool armed_ = false;
};

// Closes the context handle on scope exit while armed and removes the new
// directory at ctx->actualPath. A create that marks the directory opaque
// calls AlsoRemoveOpaqueMarkers first. The undo removes the markers before
// the directory, because RemoveDirectoryW fails on a directory that still
// holds the opaque marker file. A step of the undo that fails emits an
// LM_EVT_WARNING event with ctx->relativePath.
class DirectoryRollback {
public:
    DirectoryRollback(FileContext* ctx, WhiteoutManager& markers, const abi::EventEmitter& events)
        : ctx_(ctx), markers_(markers), events_(events) {}
    ~DirectoryRollback() {
        if (!armed_) {
            return;
        }
        CloseContextHandle(ctx_);
        if (opaqueDirectory_ && !markers_.RemoveOpaque(*opaqueDirectory_)) {
            events_.Emit(LM_EVT_WARNING, E_FAIL, ctx_->relativePath.c_str(),
                         L"The undo of a failed create could not remove the new directory's "
                         L"opaque markers");
        }
        if (!::RemoveDirectoryW(ctx_->actualPath.c_str())) {
            events_.Emit(LM_EVT_WARNING, HresultOfFailedCall(), ctx_->relativePath.c_str(),
                         L"The undo of a failed create could not remove the new directory");
        }
    }
    DirectoryRollback(const DirectoryRollback&) = delete;
    DirectoryRollback& operator=(const DirectoryRollback&) = delete;

    void Arm() { armed_ = true; }
    void Disarm() { armed_ = false; }
    void AlsoRemoveOpaqueMarkers(const std::wstring& dirRelativePath) {
        opaqueDirectory_ = dirRelativePath;
    }

private:
    FileContext* ctx_;
    WhiteoutManager& markers_;
    const abi::EventEmitter& events_;
    std::optional<std::wstring> opaqueDirectory_;
    bool armed_ = false;
};

FinalLink FinalLinkFor(UINT32 createOptions) {
    return (createOptions & FILE_OPEN_REPARSE_POINT) != 0 ? FinalLink::Keep : FinalLink::Follow;
}

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

    const ViewLookup view = WalkCallerPath(relativePath, FinalLinkFor(createOptions));
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    const std::wstring& normalized = view.path.Normalized();

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
    ctx->entryIsReparsePoint = (resolved.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    ctx->ownerPid = callerPid;
    ctx->grantedAccess = resolvedAccess;
    ctx->createOptions = createOptions;

    if (resolved.source == LayerSource::Lower && HasWriteAccess(resolvedAccess)) {
        NTSTATUS status = CopyUpForWriteOpen(hostNorm, ctx.get());
        if (!NT_SUCCESS(status)) {
            return status;
        }
        ctx->actualPath = pathResolver_->GetUpperPath(hostNorm) + streamSuffix;
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

    NTSTATUS status = FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath,
                                             ctx->entryIsReparsePoint);
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

    NTSTATUS status = FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath,
                                             ctx->entryIsReparsePoint);
    if (!NT_SUCCESS(status)) {
        ::CloseHandle(ctx->handle);
        return status;
    }

    stats_.activeHandles.fetch_add(1, std::memory_order_relaxed);
    *outCtx = std::move(ctx);
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::CopyUpForWriteOpen(const std::wstring& hostNorm, FileContext* ctx) {
    if (ctx->isDirectory) {
        return copyUp_->CopyUpDirectory(hostNorm);
    }
    if (!ctx->streamSuffix.empty()) {
        return copyUp_->CopyUpFile(hostNorm);
    }

    constexpr LONGLONG kShellOnlyAboveBytes = 1LL * 1024 * 1024;
    const FileCopyUpResult copied = copyUp_->CopyUpFileOrShell(hostNorm, kShellOnlyAboveBytes);
    if (copied.stagedShell) {
        ctx->isMetacopyOnly = true;
    }
    return copied.status;
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

NTSTATUS LayerMount::Create(const CreateRequest& callerRequest,
                           std::unique_ptr<FileContext>* outCtx,
                           InternalFileInfo* outInfo) {
    if (outCtx == nullptr || outInfo == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    *outCtx = nullptr;

    const ViewLookup view =
        WalkCallerPath(callerRequest.relativePath, FinalLinkFor(callerRequest.createOptions));
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    CreateRequest request = callerRequest;
    request.relativePath = view.path.Path();

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

    create.resolution = std::move(resolution);

    create.upperPath =
        pathResolver_->GetUpperPathForNewEntry(CallerPath(create.path.callerHost));

    const NTSTATUS parentStatus = copyUp_->EnsureUpperParent(NormalizePath(request.relativePath));
    if (!NT_SUCCESS(parentStatus)) {
        return parentStatus;
    }

    std::unique_ptr<FileContext> ctx = BuildCreate(request, &create);

    const NTSTATUS createStatus = IsDirectoryCreate(request)
        ? CreateDirectoryInUpper(create, ctx.get(), outInfo)
        : CreateFileInUpper(create, ctx.get(), outInfo);
    if (!NT_SUCCESS(createStatus)) {
        return createStatus;
    }

    cache_->InvalidateWithAncestors(create.path.hostNorm);

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
    if (pathResolver_->IsReservedPath(create.path.hostNorm)) {
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
        StreamExists(resolution->overlayHit.absolutePath, create.path.streamSuffix)) {
        return STATUS_OBJECT_NAME_COLLISION;
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::CreateDirectoryInUpper(const UpperCreate& create,
                                            FileContext* ctx,
                                            InternalFileInfo* outInfo) {
    DirectoryRollback rollback(ctx, *whiteoutMgr_, events_);
    if (!::CreateDirectoryW(create.upperPath.c_str(), nullptr)) {
        return NtStatusFromWin32(::GetLastError());
    }
    rollback.Arm();

    if (IsDirectoryHit(create.resolution.lower)) {
        rollback.AlsoRemoveOpaqueMarkers(create.path.hostNorm);
        const NTSTATUS opaqueStatus = whiteoutMgr_->SetOpaque(create.path.hostNorm);
        if (!NT_SUCCESS(opaqueStatus)) {
            return opaqueStatus;
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
    const NTSTATUS finishStatus = FinishCreatedEntry(create, ctx, outInfo);
    if (!NT_SUCCESS(finishStatus)) {
        return finishStatus;
    }
    rollback.Disarm();
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::CreateFileInUpper(const UpperCreate& create,
                                       FileContext* ctx,
                                       InternalFileInfo* outInfo) {
    if (!create.path.streamSuffix.empty()) {
        NTSTATUS hostStatus = PrepareStreamHost(create);
        if (!NT_SUCCESS(hostStatus)) {
            return hostStatus;
        }
    }

    bool streamMakesHost = false;
    if (!create.path.streamSuffix.empty() &&
        ::GetFileAttributesW(create.upperPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD hostError = ::GetLastError();
        streamMakesHost = hostError == ERROR_FILE_NOT_FOUND || hostError == ERROR_PATH_NOT_FOUND;
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
    FileRollback rollback(ctx, events_);
    if (streamMakesHost) {
        rollback.AlsoDeleteHost(create.upperPath);
    }
    rollback.Arm();

    NTSTATUS resolveStatus = ResolveContextMaximumAllowed(ctx, create.grantedAccess);
    if (!NT_SUCCESS(resolveStatus)) {
        return resolveStatus;
    }

    if (create.path.streamSuffix.empty()) {
        const NTSTATUS hostStatus = ApplyNewHostFileSettings(create, ctx->handle);
        if (!NT_SUCCESS(hostStatus)) {
            return hostStatus;
        }
    }
    const NTSTATUS finishStatus = FinishCreatedEntry(create, ctx, outInfo);
    if (!NT_SUCCESS(finishStatus)) {
        return finishStatus;
    }
    rollback.Disarm();
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::ApplyNewHostFileSettings(const UpperCreate& create, HANDLE handle) {
    const NTSTATUS sdStatus =
        WriteSecurityToNewObject(create.upperPath, create.securityDescriptor);
    if (!NT_SUCCESS(sdStatus)) {
        return sdStatus;
    }
    if (create.allocationSize > 0) {
        FILE_ALLOCATION_INFO allocInfo;
        allocInfo.AllocationSize.QuadPart = static_cast<LONGLONG>(create.allocationSize);
        if (!::SetFileInformationByHandle(handle, FileAllocationInfo,
                                           &allocInfo, sizeof(allocInfo))) {
            return NtStatusFromWin32(::GetLastError());
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::FinishCreatedEntry(const UpperCreate& create,
                                        FileContext* ctx,
                                        InternalFileInfo* outInfo) {
    const NTSTATUS infoStatus =
        FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath, ctx->entryIsReparsePoint);
    if (!NT_SUCCESS(infoStatus)) {
        return infoStatus;
    }
    if (!create.resolution.whiteoutAtPath) {
        return STATUS_SUCCESS;
    }
    return whiteoutMgr_->RemoveWhiteout(create.path.hostNorm);
}

NTSTATUS LayerMount::PrepareStreamHost(const UpperCreate& create) {
    const ResolvedPath& overlayHit = create.resolution.overlayHit;
    const bool lowerIsVisible = overlayHit.Found() && overlayHit.source == LayerSource::Lower;
    if (lowerIsVisible && !pathResolver_->ExistsInUpper(create.path.hostNorm)) {
        return copyUp_->CopyUpFile(create.path.hostNorm);
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
                return FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath,
                                              ctx->entryIsReparsePoint);
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
        return FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath,
                                      ctx->entryIsReparsePoint);
    }
    return STATUS_SUCCESS;
}

namespace {

NTSTATUS DeleteUserAlternateDataStreams(const std::wstring& basePath) {
    WIN32_FIND_STREAM_DATA streamData{};
    HANDLE find = ::FindFirstStreamW(basePath.c_str(), FindStreamInfoStandard,
                                     &streamData, 0);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        return err == ERROR_HANDLE_EOF ? STATUS_SUCCESS
                                       : ::LayerMount::NtStatusFromWin32(err);
    }

    static constexpr std::wstring_view kDataSuffix(L":$DATA");
    NTSTATUS status = STATUS_SUCCESS;
    for (;;) {
        const std::wstring_view name(streamData.cStreamName);
        if (IsUserAlternateStream(name)) {
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
        return FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath,
                                      ctx->entryIsReparsePoint);
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
        return FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath,
                                      ctx->entryIsReparsePoint);
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::DirectoryEmptinessStatus(const std::wstring& dirNorm) const {
    const MergedDirectory merged = MergeDirectoryAcrossLayers(config_, *whiteoutMgr_, dirNorm);
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
    if (kinds->source != EntryKind::File &&
        IsInsideDirectory(paths.newNorm, paths.oldNorm)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!destResolved.Found()) {
        return STATUS_SUCCESS;
    }
    if (IsInsideDirectory(paths.oldNorm, paths.newNorm)) {
        return STATUS_DIRECTORY_NOT_EMPTY;
    }
    EntryKind destinationKind = EntryKind::File;
    const NTSTATUS kindStatus =
        EntryKindOf(destResolved.absolutePath, destResolved.attributes, &destinationKind);
    if (!NT_SUCCESS(kindStatus)) {
        return kindStatus;
    }
    kinds->destination = destinationKind;
    const bool sourceIsDirectory = kinds->source == EntryKind::Directory;
    const bool destinationIsDirectory = destinationKind == EntryKind::Directory;
    if (sourceIsDirectory != destinationIsDirectory) {
        return sourceIsDirectory ? STATUS_NOT_A_DIRECTORY : STATUS_FILE_IS_A_DIRECTORY;
    }
    if (sourceIsDirectory) {
        return DirectoryEmptinessStatus(paths.newNorm);
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::CanDelete(const std::wstring& relativePath, DWORD callerPid) {
    const ViewLookup view = WalkCallerPath(relativePath, FinalLink::Keep);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    const std::wstring& normalized = view.path.Normalized();

    std::wstring hostNorm;
    std::wstring streamSuffix;
    if (!TryParseStreamPath(normalized, hostNorm, streamSuffix)) {
        return STATUS_OBJECT_NAME_INVALID;
    }
    return CanDeleteEntry(hostNorm, streamSuffix, callerPid);
}

NTSTATUS LayerMount::CanDelete(FileContext* ctx) {
    if (ctx == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    return CanDeleteEntry(NormalizePath(ctx->relativePath), ctx->streamSuffix, ctx->ownerPid);
}

NTSTATUS LayerMount::CanDeleteEntry(const std::wstring& hostNorm,
                                    const std::wstring& streamSuffix,
                                    DWORD callerPid) {
    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, hostNorm, OperationType::Delete)) {
            return STATUS_ACCESS_DENIED;
        }
    }

    const ResolvedPath resolved = pathResolver_->ResolvePath(hostNorm);
    if (!resolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if ((resolved.attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        return STATUS_CANNOT_DELETE;
    }

    if (!streamSuffix.empty()) {
        if ((resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return STATUS_FILE_IS_A_DIRECTORY;
        }
        if (!pathResolver_->ExistsInUpper(hostNorm)) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        return STATUS_SUCCESS;
    }

    EntryKind kind = EntryKind::File;
    const NTSTATUS kindStatus = EntryKindOf(resolved.absolutePath, resolved.attributes, &kind);
    if (!NT_SUCCESS(kindStatus)) {
        return kindStatus;
    }
    if (kind == EntryKind::Directory) {
        return DirectoryEmptinessStatus(hostNorm);
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::Delete(const std::wstring& relativePath, DWORD callerPid) {
    const ViewLookup view = WalkCallerPath(relativePath, FinalLink::Keep);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    const std::wstring& normalized = view.path.Normalized();

    std::wstring hostNorm;
    std::wstring streamSuffix;
    if (!TryParseStreamPath(normalized, hostNorm, streamSuffix)) {
        return STATUS_OBJECT_NAME_INVALID;
    }

    const NTSTATUS canDelete = CanDeleteEntry(hostNorm, streamSuffix, callerPid);
    if (!NT_SUCCESS(canDelete)) {
        return canDelete;
    }

    if (!streamSuffix.empty()) {
        return DeleteStreamByPath(hostNorm, streamSuffix);
    }

    return RemoveEntry(hostNorm);
}

NTSTATUS LayerMount::RemoveEntry(const std::wstring& hostNorm) {
    const NTSTATUS linkStatus = copyUp_->CopyUpLowerLinkAbove(hostNorm);
    if (!NT_SUCCESS(linkStatus)) {
        return linkStatus;
    }
    return upperEntryRemover_->Remove(hostNorm);
}

NTSTATUS LayerMount::DeleteStreamByPath(const std::wstring& hostNorm,
                                        const std::wstring& streamSuffix) {
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

    CloseContextHandle(ctx);
    ctx->handleNeedsReopen = false;

    return RemoveEntry(NormalizePath(ctx->relativePath));
}

MovedSource LayerMount::RenameFileEntry(const RenameRequest& request, RenameCopyUp copyUpMode) {
    std::wstring oldNorm = NormalizePath(request.oldRelativePath);
    std::wstring newNorm = NormalizePath(request.newRelativePath);
    const bool lowerHoldsSource = pathResolver_->ResolveLowerPath(oldNorm).Found();

    MovedFile moved = fileRename_->MoveToUpper(
        request.oldRelativePath, request.newRelativePath,
        request.replaceIfExists ? ReplaceExisting::Yes : ReplaceExisting::No, copyUpMode);
    MovedSource result{moved.status, moved.stagedShell, false,
                       UpperRename{std::move(oldNorm), std::move(newNorm),
                                   std::move(moved.oldUpperPath), UndoOpaqueMarker::Keep}};
    if (!NT_SUCCESS(result.status) || !lowerHoldsSource) {
        return result;
    }
    const RenameStepResult whiteout =
        renameRollback_->WhiteOutSource(result.rename, WhiteoutType::File);
    result.status = whiteout.status;
    result.newNameOccupied = whiteout.newNameOccupied;
    return result;
}

NTSTATUS LayerMount::DirectoryRenameRouteOf(const std::wstring& oldNorm,
                                            EntryKind sourceKind,
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
    if (sourceKind != EntryKind::Directory || whiteoutMgr_->IsOpaque(oldNorm)) {
        *route = DirectoryRenameRoute::MoveUpperAndWhiteout;
        return STATUS_SUCCESS;
    }
    EntryKind lowerKind = EntryKind::File;
    const NTSTATUS status = EntryKindOf(lower.absolutePath, lower.attributes, &lowerKind);
    if (!NT_SUCCESS(status)) return status;
    *route = lowerKind == EntryKind::Directory
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

    if (pathResolver_->IsReservedPath(paths.oldNorm) ||
        pathResolver_->IsReservedPath(paths.newNorm)) {
        return STATUS_ACCESS_DENIED;
    }

    if (auto tracker = Tracker(); tracker && callerPid != 0) {
        if (!tracker->CheckAccess(callerPid, paths.oldNorm, OperationType::Rename)) {
            return STATUS_ACCESS_DENIED;
        }
    }
    return STATUS_SUCCESS;
}

struct LayerMount::CheckedRename {
    RenameKinds kinds;
    RenameLinks links;
};

NTSTATUS LayerMount::CheckRename(const RenamePaths& paths,
                                 BOOLEAN replaceIfExists,
                                 DWORD callerPid,
                                 CheckedRename* checked) {
    NTSTATUS status = CheckRenameRequest(paths, callerPid);
    if (!NT_SUCCESS(status)) return status;

    const ResolvedPath sourceResolved = pathResolver_->ResolvePath(paths.oldNorm);
    if (!sourceResolved.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    status = CheckRenameLinkBoundary(config_, *whiteoutMgr_, *pathResolver_, paths.oldNorm,
                                     paths.newNorm, &checked->links);
    if (!NT_SUCCESS(status)) return status;
    status = EntryKindOf(sourceResolved.absolutePath, sourceResolved.attributes,
                         &checked->kinds.source);
    if (!NT_SUCCESS(status)) return status;
    if (paths.oldNorm != paths.newNorm) {
        return CheckRenameDestination(paths, replaceIfExists, &checked->kinds);
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::Rename(const std::wstring& callerOldPath,
                           const std::wstring& callerNewPath,
                           BOOLEAN replaceIfExists,
                           DWORD callerPid) {
    const ViewLookup oldView = WalkCallerPath(callerOldPath, FinalLink::Keep);
    if (!NT_SUCCESS(oldView.status)) {
        return oldView.status;
    }
    const ViewLookup newView = WalkCallerPath(callerNewPath, FinalLink::Keep);
    if (!NT_SUCCESS(newView.status)) {
        return newView.status;
    }
    return RenameViewPaths(oldView.path.Path(), newView.path.Path(), replaceIfExists, callerPid);
}

NTSTATUS LayerMount::RenameViewPaths(const std::wstring& oldRelativePath,
                                     const std::wstring& newRelativePath,
                                     BOOLEAN replaceIfExists,
                                     DWORD callerPid) {
    const std::wstring oldNorm = NormalizePath(oldRelativePath);
    const std::wstring newNorm = NormalizePath(newRelativePath);
    CheckedRename checked{RenameKinds{EntryKind::File, std::nullopt},
                          RenameLinks{false, std::nullopt}};
    const NTSTATUS status =
        CheckRename(RenamePaths{oldNorm, newNorm}, replaceIfExists, callerPid, &checked);
    if (!NT_SUCCESS(status)) return status;
    if (NormalizePathPreserveCase(oldRelativePath) ==
        NormalizePathPreserveCase(newRelativePath)) {
        return STATUS_SUCCESS;
    }
    return RenameCheckedEntry(oldRelativePath, newRelativePath, replaceIfExists, checked,
                              RenameCopyUp::ShellWhenPossible).status;
}

LayerMount::RenameResult LayerMount::RenameCheckedEntry(const std::wstring& oldRelativePath,
                                                        const std::wstring& newRelativePath,
                                                        BOOLEAN replaceIfExists,
                                                        const CheckedRename& checked,
                                                        RenameCopyUp copyUpMode) {
    const std::wstring oldNorm = NormalizePath(oldRelativePath);
    const std::wstring newNorm = NormalizePath(newRelativePath);
    const bool isSameLogicalPath = oldNorm == newNorm;
    const RenameKinds& kinds = checked.kinds;
    const bool isDirectory = kinds.source != EntryKind::File;
    const auto failure = [&checked](NTSTATUS failedStatus) {
        return RenameResult{failedStatus, false, checked.links.sourceInLinkTarget};
    };

    NTSTATUS status = CopyUpRenameLink(*copyUp_, checked.links);
    if (!NT_SUCCESS(status)) return failure(status);

    DirectoryRenameRoute route = DirectoryRenameRoute::MoveUpper;
    if (isDirectory && !isSameLogicalPath) {
        status = DirectoryRenameRouteOf(oldNorm, kinds.source, &route);
        if (!NT_SUCCESS(status)) return failure(status);
    }

    RenameDestinationAside destinationAside(config_, *cache_, events_);
    if (replaceIfExists && kinds.destination.has_value()) {
        status = copyUp_->SetRenameDestinationAside(newNorm, *kinds.destination,
                                                    &destinationAside);
        if (!NT_SUCCESS(status)) return failure(status);
    }

    const bool destHadWhiteout =
        whiteoutMgr_->HasWhiteout(newNorm, config_.upperPath);

    const MovedSource moved =
        MoveRenameSource(RenameRequest{oldRelativePath, newRelativePath, replaceIfExists},
                         kinds.source, route, copyUpMode);
    if (!NT_SUCCESS(moved.status)) {
        if (moved.newNameOccupied) destinationAside.Release();
        return failure(moved.status);
    }

    if (destHadWhiteout) {
        const RenameStepResult removed =
            renameRollback_->RemoveDestinationWhiteoutOrUndo(moved.rename);
        if (!NT_SUCCESS(removed.status)) {
            if (removed.newNameOccupied) destinationAside.Release();
            return failure(removed.status);
        }
    }
    // Commit runs after the whiteout step: once it runs, an undone rename
    // can no longer bring back the replaced destination.
    destinationAside.Commit();

    cache_->InvalidateWithAncestors(oldNorm);
    cache_->InvalidateWithAncestors(newNorm);
    return {STATUS_SUCCESS, moved.stagedShell, checked.links.sourceInLinkTarget};
}

MovedSource LayerMount::MoveRenameSource(const RenameRequest& request,
                                         EntryKind sourceKind,
                                         DirectoryRenameRoute route,
                                         RenameCopyUp copyUpMode) {
    if (sourceKind == EntryKind::File) {
        return RenameFileEntry(request, copyUpMode);
    }

    std::wstring oldNorm = NormalizePath(request.oldRelativePath);
    std::wstring newNorm = NormalizePath(request.newRelativePath);
    const bool isCaseRename = oldNorm == newNorm;
    const bool mayMarkSource = !isCaseRename && sourceKind == EntryKind::Directory &&
                               route != DirectoryRenameRoute::MergeLower &&
                               !whiteoutMgr_->IsOpaque(oldNorm);
    UpperRename rename{std::move(oldNorm), std::move(newNorm),
                       pathResolver_->GetUpperPathForRenameSource(request.oldRelativePath),
                       mayMarkSource ? UndoOpaqueMarker::Remove : UndoOpaqueMarker::Keep};
    if (isCaseRename) {
        const NTSTATUS status = copyUp_->RenameDirectoryCase(
            CallerPath(request.oldRelativePath), CallerPath(request.newRelativePath), sourceKind);
        return {status, false, false, std::move(rename)};
    }
    const RenameStepResult moved = RenameDirectoryEntry(request, sourceKind, route, rename);
    return {moved.status, false, moved.newNameOccupied, std::move(rename)};
}

RenameStepResult LayerMount::RenameDirectoryEntry(const RenameRequest& request,
                                                  EntryKind sourceKind,
                                                  DirectoryRenameRoute route,
                                                  const UpperRename& rename) {
    const CallerPath oldCallerPath(request.oldRelativePath);
    const CallerPath newCallerPath(request.newRelativePath);
    const DirectoryRename::RenameCallerPaths callerPaths{oldCallerPath, newCallerPath};
    const ReplaceExisting replace =
        request.replaceIfExists ? ReplaceExisting::Yes : ReplaceExisting::No;
    const RenameStepResult moved = route == DirectoryRenameRoute::MergeLower
        ? directoryRename_->RenameLowerDirectory(callerPaths, sourceKind, replace)
        : RenameStepResult{
              directoryRename_->RenameUpperDirectory(callerPaths, sourceKind, replace), false};
    if (route == DirectoryRenameRoute::MoveUpper || !NT_SUCCESS(moved.status)) return moved;
    return renameRollback_->WhiteOutSource(rename, WhiteoutType::Directory);
}

NTSTATUS LayerMount::Rename(FileContext* ctx,
                           const std::wstring& callerNewPath,
                           BOOLEAN replaceIfExists,
                           DWORD callerPid) {
    if (ctx == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    const ViewLookup newView = WalkCallerPath(callerNewPath, FinalLink::Keep);
    if (!NT_SUCCESS(newView.status)) {
        return newView.status;
    }
    const std::wstring& newRelativePath = newView.path.Path();

    const std::wstring oldRelativePath = ctx->relativePath;
    if (NormalizePathPreserveCase(oldRelativePath) ==
        NormalizePathPreserveCase(newRelativePath)) {
        return RenameViewPaths(oldRelativePath, newRelativePath, replaceIfExists, callerPid);
    }
    const std::wstring oldNorm = NormalizePath(oldRelativePath);
    const std::wstring newNorm = NormalizePath(newRelativePath);
    CheckedRename checked{RenameKinds{EntryKind::File, std::nullopt},
                          RenameLinks{false, std::nullopt}};
    const NTSTATUS status =
        CheckRename(RenamePaths{oldNorm, newNorm}, replaceIfExists, callerPid, &checked);
    if (!NT_SUCCESS(status)) return status;
    const bool sourceWasInUpper = pathResolver_->ExistsInUpper(oldNorm);
    const std::wstring oldActualPath = ctx->actualPath;

    if (ctx->handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(ctx->handle);
        ctx->handle = INVALID_HANDLE_VALUE;
    }

    // A read never fills a shell, so a handle that can read needs the data in the upper.
    const RenameCopyUp copyUpMode = HasFileDataAccess(ctx->grantedAccess)
        ? RenameCopyUp::FullCopy
        : RenameCopyUp::ShellWhenPossible;
    const RenameResult renamed = RenameCheckedEntry(oldRelativePath, newRelativePath,
                                                    replaceIfExists, checked, copyUpMode);
    if (!NT_SUCCESS(renamed.status)) {
        ctx->actualPath = oldActualPath;
        ctx->handleNeedsReopen = true;
        return renamed.status;
    }

    ctx->relativePath = newNorm;
    ctx->actualPath = pathResolver_->GetUpperPathForNewEntry(CallerPath(newRelativePath));
    ctx->writable = true;
    if (!sourceWasInUpper && !renamed.sourceInLinkTarget) {
        ctx->isMetacopyOnly = renamed.stagedShell;
    }
    ctx->handleNeedsReopen = true;
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::UpdateContextPath(FileContext* ctx,
                                       const std::wstring& callerNewPath) {
    if (ctx == nullptr) return STATUS_INVALID_HANDLE;
    const ViewLookup newView = WalkCallerPath(callerNewPath, FinalLink::Keep);
    if (!NT_SUCCESS(newView.status)) {
        return newView.status;
    }
    const std::wstring& newRelativePath = newView.path.Path();
    const std::wstring& newNorm = newView.path.Normalized();

    std::wstring newHostNorm;
    std::wstring newStreamSuffix;
    if (!TryParseStreamPath(newNorm, newHostNorm, newStreamSuffix)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (pathResolver_->IsReservedPath(newHostNorm)) {
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

    const ViewLookup view = WalkCallerPath(relativePath, FinalLink::Follow);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    const std::wstring& normalized = view.path.Normalized();

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
    const ViewLookup view = WalkCallerPath(relativePath, FinalLink::Follow);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    const std::wstring& normalized = view.path.Normalized();

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
    const ViewLookup view = WalkCallerPath(relativePath, FinalLink::Keep);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    const std::wstring& normalized = view.path.Normalized();
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
    const ViewLookup view = WalkCallerPath(relativePath, FinalLink::Keep);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    const std::wstring& normalized = view.path.Normalized();

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
    const ViewLookup view = WalkCallerPath(relativePath, FinalLink::Keep);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    const std::wstring& normalized = view.path.Normalized();

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
        return FillFileInfoFromHandle(ctx->handle, outInfo, &ctx->actualPath,
                                      ctx->entryIsReparsePoint);
    }
    return STATUS_SUCCESS;
}

NTSTATUS LayerMount::EnumerateStreams(const std::wstring& relativePath,
                                      std::vector<InternalStreamInfo>& out) {
    out.clear();

    const ViewLookup view = WalkCallerPath(relativePath, FinalLink::Follow);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    const std::wstring& normalized = view.path.Normalized();
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
        if (!IsUserAlternateStream(findData.cStreamName)) {
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

namespace {

// Whether CreateWhiteout and SetOpaque may write a marker for the path.
bool AcceptsMarker(const ViewPath& path) {
    return IsSafeRelativePath(path.Normalized()) && !IsReservedRelativePath(path.Normalized());
}

}

NTSTATUS LayerMount::CreateWhiteout(const std::wstring& relativePath,
                                    WhiteoutType type,
                                    MarkerPath* markerPath) {
    *markerPath = MarkerPath::Accepted;
    const ViewLookup view = WalkCallerPath(relativePath, FinalLink::Keep);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    if (!AcceptsMarker(view.path)) {
        *markerPath = MarkerPath::Refused;
        return STATUS_OBJECT_NAME_INVALID;
    }
    return whiteoutMgr_->CreateWhiteout(view.path.Path(), type);
}

NTSTATUS LayerMount::SetOpaque(const std::wstring& dirRelativePath, MarkerPath* markerPath) {
    *markerPath = MarkerPath::Accepted;
    const ViewLookup view = WalkCallerPath(dirRelativePath, FinalLink::Keep);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    if (!AcceptsMarker(view.path)) {
        *markerPath = MarkerPath::Refused;
        return STATUS_OBJECT_NAME_INVALID;
    }
    return whiteoutMgr_->SetOpaque(view.path.Path());
}

NTSTATUS LayerMount::ResolvePath(const std::wstring& relativePath,
                                 ResolvedPath* resolved) const {
    const ViewLookup view = WalkCallerPath(relativePath, FinalLink::Keep);
    if (!NT_SUCCESS(view.status)) {
        return view.status;
    }
    *resolved = pathResolver_->ResolvePath(view.path.Path());
    return STATUS_SUCCESS;
}

}
