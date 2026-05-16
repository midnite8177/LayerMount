#include "VHDLayerManager.h"
#include "Manifest.h"
#include "VolumeGuid.h"

#include <limits>

namespace LayerMount::VHD {

// ===========================================================================
// String conversion helpers
// ===========================================================================

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                     static_cast<int>(wide.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string utf8(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                          static_cast<int>(wide.size()),
                          utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int size = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                     static_cast<int>(utf8.size()),
                                     nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                          static_cast<int>(utf8.size()),
                          wide.data(), size);
    return wide;
}

// ===========================================================================
// Trailing backslash helpers
// ===========================================================================

std::wstring EnsureTrailingBackslash(const std::wstring& path) {
    if (path.empty() || path.back() == L'\\') return path;
    return path + L'\\';
}

std::wstring StripTrailingBackslash(const std::wstring& path) {
    if (!path.empty() && path.back() == L'\\')
        return path.substr(0, path.size() - 1);
    return path;
}

// ===========================================================================
// VHDLayerManager — construction / destruction
// ===========================================================================

VHDLayerManager::VHDLayerManager(const std::wstring& workingDir)
    : workingDir_(workingDir)
    , manifest_(std::make_unique<Manifest>())
{
    std::filesystem::create_directories(workingDir_);

    auto manifestPath = Manifest::DefaultPath(workingDir_);
    manifestLock_ = ManifestLock(manifestPath);
    // Failing to acquire the cross-process lock leaves the manager
    // usable for strictly in-process operations, but any disk-mutating
    // path must check Held() before load/save. We don't throw here so
    // tests that construct a manager purely to exercise in-memory
    // state still work; real writes verify the lock at their own
    // boundary.
    if (std::filesystem::exists(manifestPath)) {
        manifest_->Load(manifestPath);
    }
}

VHDLayerManager::~VHDLayerManager() {
    // Only save when the cross-process manifest mutex was actually
    // acquired. Without the lock, an unrelated process holding the
    // mutex could be mid-Load/Save against the same file, and our
    // unconditional save here would clobber its in-flight changes.
    // Real Save failures on the non-destructor path still propagate.
    // Manifest::Save no longer throws (Batch L7 -- F045 gave it the
    // error_code overload of create_directories); the try/catch is
    // kept as a defensive belt for any future filesystem call that
    // does throw, since a destructor must not unwind.
    if (manifestLock_.Held()) {
        try {
            manifest_->Save(Manifest::DefaultPath(workingDir_));
        }
        catch (...) {
            // Best-effort; teardown proceeds regardless of IO / filesystem
            // trouble here.
        }
    }
}

// ===========================================================================
// CheckElevation
// ===========================================================================

DWORD VHDLayerManager::CheckElevation() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return ::GetLastError();
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    BOOL ok = ::GetTokenInformation(token, TokenElevation,
                                    &elevation, sizeof(elevation), &size);
    DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(token);

    if (err != ERROR_SUCCESS) return err;

    return elevation.TokenIsElevated ? ERROR_SUCCESS : ERROR_PRIVILEGE_NOT_HELD;
}

// ===========================================================================
// GenerateId — UUID via CoCreateGuid
// ===========================================================================

std::wstring VHDLayerManager::GenerateId() {
    GUID guid{};
    HRESULT hr = ::CoCreateGuid(&guid);
    if (FAILED(hr)) return L"";

    // StringFromGUID2 produces {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
    // We strip the braces for a cleaner ID.
    wchar_t buf[40]{};
    int len = ::StringFromGUID2(guid, buf, 40);
    if (len == 0) return L"";

    std::wstring id(buf);
    // Strip leading '{' and trailing '}'
    if (id.size() >= 2 && id.front() == L'{' && id.back() == L'}') {
        id = id.substr(1, id.size() - 2);
    }
    return id;
}

// ===========================================================================
// OpenVHD — open an existing VHD/VHDX file
// ===========================================================================

DWORD VHDLayerManager::OpenVHD(const std::wstring& path, VhdHandle& outHandle) {
    VIRTUAL_STORAGE_TYPE storageType{};
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    // Detect format by extension
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));

    if (ext == L".vhdx") {
        storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    } else if (ext == L".vhd") {
        storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
    } else {
        // Default to VHDX
        storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    }

    OPEN_VIRTUAL_DISK_PARAMETERS openParams{};
    openParams.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    openParams.Version1.RWDepth = OPEN_VIRTUAL_DISK_RW_DEPTH_DEFAULT;

    DWORD result = ::OpenVirtualDisk(
        &storageType,
        path.c_str(),
        VIRTUAL_DISK_ACCESS_ALL,
        OPEN_VIRTUAL_DISK_FLAG_NONE,
        &openParams,
        outHandle.Put());

    return result;
}

// ===========================================================================
// Manifest accessors
// ===========================================================================

Manifest& VHDLayerManager::GetManifest() { return *manifest_; }
const Manifest& VHDLayerManager::GetManifest() const { return *manifest_; }

// ===========================================================================
// 4.1 — CreateVHD
// ===========================================================================

DWORD VHDLayerManager::CreateVHD(const std::wstring& path, ULONGLONG sizeBytes,
                                  bool dynamic, VhdHandle& outHandle) {
    // Always create as VHDX
    VIRTUAL_STORAGE_TYPE storageType{};
    storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    CREATE_VIRTUAL_DISK_PARAMETERS createParams{};
    createParams.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    createParams.Version2.UniqueId = GUID_NULL;
    createParams.Version2.MaximumSize = sizeBytes;
    createParams.Version2.BlockSizeInBytes = 0;   // System default
    createParams.Version2.SectorSizeInBytes = 0;  // System default
    createParams.Version2.ParentPath = nullptr;
    createParams.Version2.SourcePath = nullptr;
    createParams.Version2.PhysicalSectorSizeInBytes = 0;

    // Dynamic = sparse (no preallocation); Fixed = full physical allocation
    CREATE_VIRTUAL_DISK_FLAG flags = dynamic
        ? CREATE_VIRTUAL_DISK_FLAG_NONE
        : CREATE_VIRTUAL_DISK_FLAG_FULL_PHYSICAL_ALLOCATION;

    // Version 2 create params require VIRTUAL_DISK_ACCESS_NONE
    DWORD result = ::CreateVirtualDisk(
        &storageType,
        path.c_str(),
        VIRTUAL_DISK_ACCESS_NONE,
        nullptr,    // security descriptor
        flags,
        0,          // provider-specific flags
        &createParams,
        nullptr,    // overlapped
        outHandle.Put());

    return result;
}

// ===========================================================================
// 4.2 — AttachVHD / DetachVHD
// ===========================================================================

DWORD VHDLayerManager::AttachVHD(const std::wstring& path, bool readOnly,
                                  VhdHandle& outHandle,
                                  std::wstring& outPhysicalPath,
                                  AttachLifetime lifetime,
                                  bool suppressDriveLetter) {
    outPhysicalPath.clear();

    VhdHandle handle;
    DWORD result = OpenVHD(path, handle);
    if (result != ERROR_SUCCESS) return result;

    ATTACH_VIRTUAL_DISK_PARAMETERS attachParams{};
    attachParams.Version = ATTACH_VIRTUAL_DISK_VERSION_1;

    // ProcessScoped omits PERMANENT_LIFETIME so the OS releases the attach
    // automatically when the process handle closes — essential for hard-kill
    // cleanup of mount-child VHDs.
    ATTACH_VIRTUAL_DISK_FLAG flags =
        (lifetime == AttachLifetime::Permanent)
            ? ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME
            : ATTACH_VIRTUAL_DISK_FLAG_NONE;
    if (readOnly) {
        flags = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(
            flags | ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY);
    }
    if (suppressDriveLetter) {
        // Ask the Mount Manager to skip auto-assignment of a drive letter
        // so users don't see a disruptive AutoPlay flash on transient attach.
        flags = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(
            flags | ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER);
    }

    result = ::AttachVirtualDisk(
        handle.Get(),
        nullptr,    // security descriptor
        flags,
        0,          // provider-specific flags
        &attachParams,
        nullptr);   // overlapped

    if (result != ERROR_SUCCESS) return result;

    // Retrieve the physical disk path (e.g., \\.\PhysicalDrive2)
    WCHAR diskPath[MAX_PATH]{};
    ULONG diskPathSize = sizeof(diskPath);
    result = ::GetVirtualDiskPhysicalPath(handle.Get(), &diskPathSize, diskPath);
    if (result != ERROR_SUCCESS) {
        // Attached but cannot get path — detach and fail
        ::DetachVirtualDisk(handle.Get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        return result;
    }

    outPhysicalPath = diskPath;
    outHandle = std::move(handle);
    return ERROR_SUCCESS;
}

DWORD VHDLayerManager::DetachVHD(const std::wstring& path) {
    VhdHandle handle;
    DWORD result = OpenVHD(path, handle);
    if (result != ERROR_SUCCESS) return result;

    result = ::DetachVirtualDisk(
        handle.Get(),
        DETACH_VIRTUAL_DISK_FLAG_NONE,
        0);

    return result;
}

// ===========================================================================
// 4.3 — CreateDifferencingVHD
// ===========================================================================

DWORD VHDLayerManager::CreateDifferencingVHD(const std::wstring& childPath,
                                              const std::wstring& parentPath,
                                              VhdHandle& outHandle) {
    VIRTUAL_STORAGE_TYPE storageType{};
    storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    CREATE_VIRTUAL_DISK_PARAMETERS createParams{};
    createParams.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    createParams.Version2.UniqueId = GUID_NULL;
    createParams.Version2.MaximumSize = 0;            // Inherited from parent
    createParams.Version2.BlockSizeInBytes = 0;
    createParams.Version2.SectorSizeInBytes = 0;
    createParams.Version2.ParentPath = parentPath.c_str();
    createParams.Version2.SourcePath = nullptr;
    createParams.Version2.PhysicalSectorSizeInBytes = 0;

    // Version 2 create params require VIRTUAL_DISK_ACCESS_NONE
    DWORD result = ::CreateVirtualDisk(
        &storageType,
        childPath.c_str(),
        VIRTUAL_DISK_ACCESS_NONE,
        nullptr,
        CREATE_VIRTUAL_DISK_FLAG_NONE,
        0,
        &createParams,
        nullptr,
        outHandle.Put());

    return result;
}

// ===========================================================================
// 4.4 — MergeVHD
// ===========================================================================

DWORD VHDLayerManager::MergeVHD(const std::wstring& childPath) {
    VhdHandle handle;
    DWORD result = OpenVHD(childPath, handle);
    if (result != ERROR_SUCCESS) return result;

    MERGE_VIRTUAL_DISK_PARAMETERS mergeParams{};
    mergeParams.Version = MERGE_VIRTUAL_DISK_VERSION_2;
    mergeParams.Version2.MergeSourceDepth = 1;
    mergeParams.Version2.MergeTargetDepth = 2;

    result = ::MergeVirtualDisk(
        handle.Get(),
        MERGE_VIRTUAL_DISK_FLAG_NONE,
        &mergeParams,
        nullptr);   // overlapped

    return result;
}

// ===========================================================================
// 4.5 — InitializeVHD
// ===========================================================================

// Well-known GPT partition type GUIDs
// {EBD0A0A2-B9E5-4433-87C0-68B6B72699C7} — Basic Data Partition
static const GUID PARTITION_BASIC_DATA_ID =
    { 0xebd0a0a2, 0xb9e5, 0x4433, { 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };

DWORD VHDLayerManager::InitializeVHD(const std::wstring& physicalDiskPath,
                                      const std::wstring& vhdPath) {
    // Open the physical disk
    HANDLE hDisk = ::CreateFileW(
        physicalDiskPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hDisk == INVALID_HANDLE_VALUE) {
        return InitializeVHDDiskpart(vhdPath);
    }

    DWORD bytesReturned = 0;
    BOOL ok = FALSE;

    // Step 1: Bring disk online — clear OFFLINE and READ_ONLY attributes
    SET_DISK_ATTRIBUTES diskAttrs{};
    diskAttrs.Version = sizeof(SET_DISK_ATTRIBUTES);
    diskAttrs.Persist = TRUE;
    diskAttrs.Attributes = 0;  // Desired state: online + writable
    diskAttrs.AttributesMask = DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY;
    ::DeviceIoControl(hDisk, IOCTL_DISK_SET_DISK_ATTRIBUTES,
                      &diskAttrs, sizeof(diskAttrs),
                      nullptr, 0, &bytesReturned, nullptr);
    // Ignore failure — may not be offline

    // Step 2: Get disk geometry to know total size
    DISK_GEOMETRY_EX geom{};
    ok = ::DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                           nullptr, 0,
                           &geom, sizeof(geom), &bytesReturned, nullptr);
    if (!ok) {
        ::CloseHandle(hDisk);
        return InitializeVHDDiskpart(vhdPath);
    }
    LONGLONG totalDiskSize = geom.DiskSize.QuadPart;

    // Step 3: Create GPT disk
    GUID diskId{};
    ::CoCreateGuid(&diskId);

    CREATE_DISK createDisk{};
    createDisk.PartitionStyle = PARTITION_STYLE_GPT;
    createDisk.Gpt.DiskId = diskId;
    createDisk.Gpt.MaxPartitionCount = 128;

    ok = ::DeviceIoControl(hDisk, IOCTL_DISK_CREATE_DISK,
                           &createDisk, sizeof(createDisk),
                           nullptr, 0, &bytesReturned, nullptr);
    if (!ok) {
        ::CloseHandle(hDisk);
        return InitializeVHDDiskpart(vhdPath);
    }

    // Step 4: Update properties
    ::DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES,
                      nullptr, 0, nullptr, 0, &bytesReturned, nullptr);

    // Step 5: Set drive layout — single Basic Data partition (per spec)
    // DRIVE_LAYOUT_INFORMATION_EX already contains 1 PARTITION_INFORMATION_EX entry
    // so sizeof(DRIVE_LAYOUT_INFORMATION_EX) is sufficient for 1 partition.
    constexpr LONGLONG kPartitionOffset = 1048576;  // 1 MB
    constexpr LONGLONG kTailPadding = 1048576;       // 1 MB

    LONGLONG partitionLength = totalDiskSize - kPartitionOffset - kTailPadding;
    if (partitionLength <= 0) {
        ::CloseHandle(hDisk);
        return ERROR_DISK_TOO_FRAGMENTED;  // Disk too small
    }

    // Allocate buffer for DRIVE_LAYOUT_INFORMATION_EX with 1 partition entry
    // The struct already includes one PartitionEntry, so no extra allocation needed.
    alignas(8) BYTE layoutBuf[sizeof(DRIVE_LAYOUT_INFORMATION_EX)]{};
    auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuf);

    layout->PartitionStyle = PARTITION_STYLE_GPT;
    layout->PartitionCount = 1;
    layout->Gpt.DiskId = diskId;
    layout->Gpt.StartingUsableOffset.QuadPart = kPartitionOffset;
    layout->Gpt.UsableLength.QuadPart = totalDiskSize - kPartitionOffset - kTailPadding;
    layout->Gpt.MaxPartitionCount = 128;

    GUID partId{};
    ::CoCreateGuid(&partId);

    layout->PartitionEntry[0].PartitionStyle = PARTITION_STYLE_GPT;
    layout->PartitionEntry[0].StartingOffset.QuadPart = kPartitionOffset;
    layout->PartitionEntry[0].PartitionLength.QuadPart = partitionLength;
    layout->PartitionEntry[0].PartitionNumber = 1;
    layout->PartitionEntry[0].RewritePartition = TRUE;
    layout->PartitionEntry[0].Gpt.PartitionType = PARTITION_BASIC_DATA_ID;
    layout->PartitionEntry[0].Gpt.PartitionId = partId;
    layout->PartitionEntry[0].Gpt.Attributes = 0;

    ok = ::DeviceIoControl(hDisk, IOCTL_DISK_SET_DRIVE_LAYOUT_EX,
                           layoutBuf, sizeof(layoutBuf),
                           nullptr, 0, &bytesReturned, nullptr);
    if (!ok) {
        ::CloseHandle(hDisk);
        return InitializeVHDDiskpart(vhdPath);
    }

    // Step 6: Update properties again
    ::DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES,
                      nullptr, 0, nullptr, 0, &bytesReturned, nullptr);

    ::CloseHandle(hDisk);

    // Step 7: Volume discovery — retry with backoff until OS recognizes the volume
    std::wstring volumeGuid;
    DWORD discoverResult = ERROR_NOT_FOUND;
    const DWORD delays[] = { 100, 200, 400, 800, 1600 };

    for (DWORD delay : delays) {
        discoverResult = GetVolumeGuidForPhysicalDisk(physicalDiskPath, volumeGuid);
        if (discoverResult == ERROR_SUCCESS) break;
        if (discoverResult != ERROR_NOT_FOUND &&
            discoverResult != ERROR_DEV_NOT_EXIST &&
            discoverResult != ERROR_NOT_READY) {
            return discoverResult;  // Fatal error
        }
        ::Sleep(delay);
    }

    if (discoverResult != ERROR_SUCCESS) {
        // Could not find volume — try diskpart as last resort
        return InitializeVHDDiskpart(vhdPath);
    }

    // Step 8: Format as NTFS
    return FormatVolume(volumeGuid, L"LayerMount");
}

// ===========================================================================
// FormatVolume — format via format.com
// ===========================================================================

DWORD VHDLayerManager::FormatVolume(const std::wstring& volumeGuid,
                                     const std::wstring& label) {
    // format.com accepts volume GUID paths on Windows 10+
    // /FS:NTFS /Q = quick format, /Y = no confirmation, /V:label
    std::wstring cmdLine = L"format.com " + StripTrailingBackslash(volumeGuid)
        + L" /FS:NTFS /Q /Y /V:" + label;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};

    // CreateProcessW needs a writable command line buffer
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    BOOL created = ::CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr, nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    if (!created) return ::GetLastError();

    // Handle the three WaitForSingleObject outcomes distinctly. Previously
    // the return value was ignored, so a hung format.com would keep running
    // after we "timed out" -- the child could then race our detach/delete
    // path or corrupt a subsequent VHD init. Terminate on timeout, return
    // ERROR_TIMEOUT so callers don't misattribute the failure as "bad
    // volume format"; on WAIT_FAILED propagate GetLastError().
    const DWORD waitResult = ::WaitForSingleObject(pi.hProcess, 30000);
    if (waitResult == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, INFINITE);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        return ERROR_TIMEOUT;
    }
    if (waitResult == WAIT_FAILED) {
        const DWORD waitErr = ::GetLastError();
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        return waitErr;
    }

    DWORD exitCode = 1;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);

    return (exitCode == 0) ? ERROR_SUCCESS : ERROR_UNRECOGNIZED_VOLUME;
}

// ===========================================================================
// InitializeVHDDiskpart — diskpart fallback
// ===========================================================================

DWORD VHDLayerManager::InitializeVHDDiskpart(const std::wstring& vhdPath) {
    // Build diskpart script
    std::wstring script =
        L"SELECT VDISK FILE=\"" + vhdPath + L"\"\r\n"
        L"ATTACH VDISK\r\n"
        L"ONLINE DISK\r\n"
        L"ATTRIBUTES DISK CLEAR READONLY\r\n"
        L"CREATE PARTITION PRIMARY\r\n"
        L"FORMAT FS=NTFS LABEL=\"LayerMount\" QUICK\r\n";

    // Write script to temp file
    wchar_t tempDir[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tempDir);

    std::wstring scriptPath = std::wstring(tempDir) + L"ovfs_diskpart_" + GenerateId() + L".txt";

    // Write as UTF-16 LE BOM for diskpart compatibility.
    HANDLE hFile = ::CreateFileW(scriptPath.c_str(), GENERIC_WRITE, 0,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return ::GetLastError();

    // Write BOM + content, verifying both succeed with exact byte counts.
    // A partial write (disk full, IO error) would otherwise leave diskpart
    // running against a truncated script: VHD half-initialized, half-
    // formatted, stranded attach state. Bail before launching diskpart.
    auto writeAll = [&](const void* data, DWORD bytes) -> DWORD {
        DWORD w = 0;
        if (!::WriteFile(hFile, data, bytes, &w, nullptr)) {
            return ::GetLastError();
        }
        if (w != bytes) {
            return ERROR_WRITE_FAULT;
        }
        return ERROR_SUCCESS;
    };
    const BYTE bom[] = { 0xFF, 0xFE };
    DWORD werr = writeAll(bom, sizeof(bom));
    if (werr == ERROR_SUCCESS) {
        werr = writeAll(script.c_str(),
                        static_cast<DWORD>(script.size() * sizeof(wchar_t)));
    }
    if (werr == ERROR_SUCCESS) {
        if (!::CloseHandle(hFile)) {
            werr = ::GetLastError();
        }
    } else {
        ::CloseHandle(hFile);
    }
    if (werr != ERROR_SUCCESS) {
        ::DeleteFileW(scriptPath.c_str());
        return werr;
    }

    // Run diskpart
    std::wstring cmdLine = L"diskpart /s \"" + scriptPath + L"\"";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    BOOL created = ::CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr, nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    if (!created) {
        DWORD err = ::GetLastError();
        ::DeleteFileW(scriptPath.c_str());
        return err;
    }

    // Same timeout handling as FormatVolume (above). A hung diskpart can
    // still hold the VHD attached, so terminate on timeout before deleting
    // the script -- deleting it while diskpart is still reading produces
    // nondeterministic partial states.
    const DWORD waitResult = ::WaitForSingleObject(pi.hProcess, 60000);
    if (waitResult == WAIT_TIMEOUT) {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, INFINITE);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        ::DeleteFileW(scriptPath.c_str());
        return ERROR_TIMEOUT;
    }
    if (waitResult == WAIT_FAILED) {
        const DWORD waitErr = ::GetLastError();
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(pi.hThread);
        ::DeleteFileW(scriptPath.c_str());
        return waitErr;
    }

    DWORD exitCode = 1;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);

    ::DeleteFileW(scriptPath.c_str());

    return (exitCode == 0) ? ERROR_SUCCESS : ERROR_UNRECOGNIZED_VOLUME;
}

// ===========================================================================
// 4.7 — ImportDirectory
// ===========================================================================

DWORD VHDLayerManager::ImportDirectory(const std::wstring& directoryPath,
                                        const std::wstring& vhdPath,
                                        ULONGLONG sizeBytes) {
    namespace fs = std::filesystem;

    // Auto-calculate size if not specified. Accumulate with overflow
    // checks: an adversarial or anomalous tree (sparse sizes, junction
    // loops, large files) could wrap a plain ULONGLONG sum and produce a
    // VHD far smaller than needed -- the import would then fail during
    // copy after we've already done the costly VHD create+attach. Detect
    // overflow up front.
    if (sizeBytes == 0) {
        ULONGLONG totalSize = 0;
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(directoryPath, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::error_code fsec;
            const auto fsz = entry.file_size(fsec);
            if (fsec) continue;
            const ULONGLONG add = static_cast<ULONGLONG>(fsz);
            if (add > (std::numeric_limits<ULONGLONG>::max)() - totalSize) {
                return ERROR_ARITHMETIC_OVERFLOW;
            }
            totalSize += add;
        }
        // Add 20% overhead, enforce 100 MB minimum. totalSize/5 cannot
        // overflow (smaller than totalSize), but totalSize + (totalSize/5)
        // can -- guard against it.
        const ULONGLONG overhead = totalSize / 5;
        if (overhead > (std::numeric_limits<ULONGLONG>::max)() - totalSize) {
            return ERROR_ARITHMETIC_OVERFLOW;
        }
        sizeBytes = totalSize + overhead;
        constexpr ULONGLONG kMinSize = 100ULL * 1024 * 1024;
        if (sizeBytes < kMinSize) sizeBytes = kMinSize;
    }

    // Create the VHD
    VhdHandle createHandle;
    DWORD result = CreateVHD(vhdPath, sizeBytes, true, createHandle);
    if (result != ERROR_SUCCESS) return result;
    createHandle.Close();  // Close create handle before attach

    // Attach the VHD. Transient: process-scoped so a crash mid-import releases
    // the attach automatically, and no drive letter so Windows AutoPlay doesn't
    // flash for a fleeting volume.
    VhdHandle attachHandle;
    std::wstring physicalPath;
    result = AttachVHD(vhdPath, false, attachHandle, physicalPath,
                       AttachLifetime::ProcessScoped,
                       /*suppressDriveLetter=*/ true);
    if (result != ERROR_SUCCESS) {
        fs::remove(vhdPath);
        return result;
    }

    // Initialize (partition + format)
    result = InitializeVHD(physicalPath, vhdPath);
    if (result != ERROR_SUCCESS) {
        attachHandle.Close();
        DetachVHD(vhdPath);
        fs::remove(vhdPath);
        return result;
    }

    // Discover the volume GUID
    std::wstring volumeGuid;
    result = GetVolumeGuidForPhysicalDisk(physicalPath, volumeGuid);
    if (result != ERROR_SUCCESS) {
        attachHandle.Close();
        DetachVHD(vhdPath);
        fs::remove(vhdPath);
        return result;
    }

    // Create unique temp mount point
    std::wstring tempMount = workingDir_ + L"\\temp_mounts\\" + GenerateId();
    fs::create_directories(tempMount);
    std::wstring tempMountSlash = EnsureTrailingBackslash(tempMount);

    // Mount the volume
    if (!::SetVolumeMountPointW(tempMountSlash.c_str(), volumeGuid.c_str())) {
        DWORD err = ::GetLastError();
        fs::remove_all(tempMount);
        attachHandle.Close();
        DetachVHD(vhdPath);
        fs::remove(vhdPath);
        return err;
    }

    // Copy directory contents
    std::error_code ec;
    fs::copy(directoryPath, tempMount,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);

    DWORD copyResult = ec ? ERROR_WRITE_FAULT : ERROR_SUCCESS;

    // Cleanup: unmount, remove temp dir, detach. The previous version
    // ignored every return value -- a cleanup failure could strand a
    // mount point or leave the VHD attached under the service account,
    // and callers saw success. Capture the mount-point + temp-dir errors
    // and surface them. DetachVHD here is redundant for the ProcessScoped
    // attach above (closing attachHandle already releases the attach), so
    // its best-effort failure is benign -- don't let it mask a successful
    // import.
    std::error_code rmEc;
    DWORD cleanupErr = ERROR_SUCCESS;
    if (!::DeleteVolumeMountPointW(tempMountSlash.c_str())) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            cleanupErr = err;
        }
    }
    fs::remove_all(tempMount, rmEc);
    if (rmEc && cleanupErr == ERROR_SUCCESS) {
        cleanupErr = ERROR_DIR_NOT_EMPTY;
    }
    attachHandle.Close();
    DetachVHD(vhdPath);  // redundant for ProcessScoped; ignore return.

    if (copyResult != ERROR_SUCCESS) {
        fs::remove(vhdPath, rmEc);
        return copyResult;
    }
    // Copy succeeded but cleanup didn't: report the cleanup error so the
    // operator can recover the stranded resource.
    return cleanupErr;
}

// ===========================================================================
// 4.8 — ExportToDirectory
// ===========================================================================

DWORD VHDLayerManager::ExportToDirectory(const std::wstring& vhdPath,
                                          const std::wstring& directoryPath) {
    namespace fs = std::filesystem;

    // Attach read-only. Transient: process-scoped + drive-letter-suppressed
    // (see ImportDirectory for rationale).
    VhdHandle attachHandle;
    std::wstring physicalPath;
    DWORD result = AttachVHD(vhdPath, true, attachHandle, physicalPath,
                             AttachLifetime::ProcessScoped,
                             /*suppressDriveLetter=*/ true);
    if (result != ERROR_SUCCESS) return result;

    // Discover the volume GUID
    std::wstring volumeGuid;
    result = GetVolumeGuidForPhysicalDisk(physicalPath, volumeGuid);
    if (result != ERROR_SUCCESS) {
        attachHandle.Close();
        DetachVHD(vhdPath);
        return result;
    }

    // Create unique temp mount point
    std::wstring tempMount = workingDir_ + L"\\temp_mounts\\" + GenerateId();
    fs::create_directories(tempMount);
    std::wstring tempMountSlash = EnsureTrailingBackslash(tempMount);

    // Mount the volume
    if (!::SetVolumeMountPointW(tempMountSlash.c_str(), volumeGuid.c_str())) {
        DWORD err = ::GetLastError();
        std::error_code ec;
        fs::remove_all(tempMount, ec);
        attachHandle.Close();
        DetachVHD(vhdPath);
        return err;
    }

    // Ensure target directory exists
    fs::create_directories(directoryPath);

    // Copy user-visible contents. Skip NTFS system directories at the volume
    // root — `System Volume Information` has a SYSTEM-only ACL even under
    // elevation and is not user content; `$RECYCLE.BIN` is similarly internal.
    // Tolerate per-entry permission errors below the root so restrictive ACLs
    // on user files do not abort the whole export.
    static const wchar_t* const kSkipAtRoot[] = {
        L"System Volume Information",
        L"$RECYCLE.BIN",
    };

    const fs::path srcRoot = tempMount;
    const fs::path dstRoot = directoryPath;

    size_t copiedEntries = 0;
    size_t failedEntries = 0;
    std::error_code ec;

    for (const auto& topEntry : fs::directory_iterator(srcRoot, ec)) {
        const std::wstring name = topEntry.path().filename().wstring();
        bool skip = false;
        for (const wchar_t* s : kSkipAtRoot) {
            if (_wcsicmp(name.c_str(), s) == 0) { skip = true; break; }
        }
        if (skip) continue;

        const fs::path dstTop = dstRoot / topEntry.path().filename();
        std::error_code copyEc;
        fs::copy(topEntry.path(), dstTop,
                 fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing |
                 fs::copy_options::copy_symlinks,
                 copyEc);
        if (!copyEc) {
            ++copiedEntries;
            continue;
        }

        // Wholesale copy failed — walk manually with per-entry tolerance so a
        // single restricted file or directory doesn't abort the rest.
        std::error_code it_ec;
        fs::recursive_directory_iterator it(topEntry.path(),
            fs::directory_options::skip_permission_denied, it_ec);
        fs::recursive_directory_iterator end;
        if (it_ec) { ++failedEntries; continue; }
        for (; it != end; it.increment(it_ec)) {
            if (it_ec) { ++failedEntries; it_ec.clear(); continue; }
            std::error_code rel_ec;
            const auto rel = fs::relative(it->path(), srcRoot, rel_ec);
            if (rel_ec) { ++failedEntries; continue; }
            const auto dst = dstRoot / rel;
            std::error_code one_ec;
            if (it->is_directory(one_ec)) {
                fs::create_directories(dst, one_ec);
                if (!one_ec) ++copiedEntries; else ++failedEntries;
            } else if (it->is_regular_file(one_ec)) {
                fs::create_directories(dst.parent_path(), one_ec);
                fs::copy_file(it->path(), dst,
                              fs::copy_options::overwrite_existing, one_ec);
                if (!one_ec) ++copiedEntries; else ++failedEntries;
            }
        }
    }

    // Succeed unless the top-level iterator itself failed and nothing landed.
    DWORD copyResult = (ec && copiedEntries == 0) ? ERROR_READ_FAULT
                                                   : ERROR_SUCCESS;

    // Cleanup: unmount, remove temp dir, detach. Same pattern as
    // ImportDirectory above -- mount-point and temp-dir failures surface;
    // DetachVHD is redundant for the ProcessScoped attach (handle close
    // already released it) so its best-effort failure stays silent.
    DWORD cleanupErr = ERROR_SUCCESS;
    if (!::DeleteVolumeMountPointW(tempMountSlash.c_str())) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            cleanupErr = err;
        }
    }
    std::error_code cleanupEc;
    fs::remove_all(tempMount, cleanupEc);
    if (cleanupEc && cleanupErr == ERROR_SUCCESS) {
        cleanupErr = ERROR_DIR_NOT_EMPTY;
    }
    attachHandle.Close();
    DetachVHD(vhdPath);  // redundant for ProcessScoped; ignore return.

    // Export copy succeeded and cleanup succeeded -> SUCCESS.
    // Export copy failed -> return the copy error (primary failure).
    // Export copy succeeded but cleanup failed -> return cleanup error
    // so the operator can recover the stranded resource.
    if (copyResult != ERROR_SUCCESS) return copyResult;
    return cleanupErr;
}

} // namespace LayerMount::VHD
