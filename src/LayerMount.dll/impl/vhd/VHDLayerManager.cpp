#include "VHDLayerManager.h"
#include "Manifest.h"
#include "VolumeGuid.h"
#include "../ElevationUtil.h"
#include "../LayerPath.h"
#include "../PathUtil.h"

#include <limits>

namespace LayerMount::VHD {

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

std::wstring EnsureTrailingBackslash(const std::wstring& path) {
    if (path.empty() || path.back() == L'\\') return path;
    return path + L'\\';
}

VHDLayerManager::VHDLayerManager(const std::wstring& workingDir)
    : workingDir_(workingDir)
    , manifest_(std::make_unique<Manifest>())
{
    std::filesystem::create_directories(workingDir_);

    auto manifestPath = Manifest::DefaultPath(workingDir_);
    manifestLock_ = ManifestLock(manifestPath);
    if (std::filesystem::exists(manifestPath)) {
        manifest_->Load(manifestPath);
    }
}

VHDLayerManager::~VHDLayerManager() {
    if (manifestLock_.Held()) {
        try {
            manifest_->Save(Manifest::DefaultPath(workingDir_));
        }
        catch (...) {
        }
    }
}

DWORD VHDLayerManager::CheckElevation() {
    return ::LayerMount::CheckElevation();
}

std::wstring VHDLayerManager::GenerateId() {
    GUID guid{};
    HRESULT hr = ::CoCreateGuid(&guid);
    if (FAILED(hr)) return L"";

    wchar_t buf[40]{};
    int len = ::StringFromGUID2(guid, buf, 40);
    if (len == 0) return L"";

    std::wstring id(buf);
    if (id.size() >= 2 && id.front() == L'{' && id.back() == L'}') {
        id = id.substr(1, id.size() - 2);
    }
    return id;
}

DWORD VHDLayerManager::OpenVHD(const std::wstring& path, VhdHandle& outHandle) {
    VIRTUAL_STORAGE_TYPE storageType{};
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    std::wstring ext = std::filesystem::path(path).extension().wstring();
    for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));

    if (ext == L".vhdx") {
        storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    } else if (ext == L".vhd") {
        storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
    } else {
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

Manifest& VHDLayerManager::GetManifest() { return *manifest_; }
const Manifest& VHDLayerManager::GetManifest() const { return *manifest_; }

DWORD VHDLayerManager::CreateVHD(const std::wstring& path, ULONGLONG sizeBytes,
                                  bool dynamic, VhdHandle& outHandle) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

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

    CREATE_VIRTUAL_DISK_FLAG flags = dynamic
        ? CREATE_VIRTUAL_DISK_FLAG_NONE
        : CREATE_VIRTUAL_DISK_FLAG_FULL_PHYSICAL_ALLOCATION;

    // Version 2 create parameters require VIRTUAL_DISK_ACCESS_NONE.
    result = ::CreateVirtualDisk(
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

DWORD VHDLayerManager::AttachVHD(const std::wstring& path, bool readOnly,
                                  VhdHandle& outHandle,
                                  std::wstring& outPhysicalPath,
                                  AttachLifetime lifetime,
                                  bool suppressDriveLetter) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    outPhysicalPath.clear();

    VhdHandle handle;
    result = OpenVHD(path, handle);
    if (result != ERROR_SUCCESS) return result;

    ATTACH_VIRTUAL_DISK_PARAMETERS attachParams{};
    attachParams.Version = ATTACH_VIRTUAL_DISK_VERSION_1;

    // Without PERMANENT_LIFETIME the OS detaches the disk when the attach
    // handle closes, including when the process dies.
    ATTACH_VIRTUAL_DISK_FLAG flags =
        (lifetime == AttachLifetime::Permanent)
            ? ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME
            : ATTACH_VIRTUAL_DISK_FLAG_NONE;
    if (readOnly) {
        flags = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(
            flags | ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY);
    }
    if (suppressDriveLetter) {
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

    WCHAR diskPath[MAX_PATH]{};
    ULONG diskPathSize = sizeof(diskPath);
    result = ::GetVirtualDiskPhysicalPath(handle.Get(), &diskPathSize, diskPath);
    if (result != ERROR_SUCCESS) {
        ::DetachVirtualDisk(handle.Get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        return result;
    }

    outPhysicalPath = diskPath;
    outHandle = std::move(handle);
    return ERROR_SUCCESS;
}

DWORD VHDLayerManager::DetachVHD(const std::wstring& path) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    VhdHandle handle;
    result = OpenVHD(path, handle);
    if (result != ERROR_SUCCESS) return result;

    result = ::DetachVirtualDisk(
        handle.Get(),
        DETACH_VIRTUAL_DISK_FLAG_NONE,
        0);

    return result;
}

DWORD VHDLayerManager::CreateDifferencingVHD(const std::wstring& childPath,
                                              const std::wstring& parentPath,
                                              VhdHandle& outHandle) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

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

    // Version 2 create parameters require VIRTUAL_DISK_ACCESS_NONE.
    result = ::CreateVirtualDisk(
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

DWORD VHDLayerManager::MergeVHD(const std::wstring& childPath) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    VhdHandle handle;
    result = OpenVHD(childPath, handle);
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

static const GUID PARTITION_BASIC_DATA_ID =
    { 0xebd0a0a2, 0xb9e5, 0x4433, { 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };

DWORD VHDLayerManager::InitializeVHD(const std::wstring& physicalDiskPath,
                                      const std::wstring& vhdPath) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

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

    SET_DISK_ATTRIBUTES diskAttrs{};
    diskAttrs.Version = sizeof(SET_DISK_ATTRIBUTES);
    diskAttrs.Persist = TRUE;
    diskAttrs.Attributes = 0;
    diskAttrs.AttributesMask = DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY;
    ::DeviceIoControl(hDisk, IOCTL_DISK_SET_DISK_ATTRIBUTES,
                      &diskAttrs, sizeof(diskAttrs),
                      nullptr, 0, &bytesReturned, nullptr);

    DISK_GEOMETRY_EX geom{};
    ok = ::DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                           nullptr, 0,
                           &geom, sizeof(geom), &bytesReturned, nullptr);
    if (!ok) {
        ::CloseHandle(hDisk);
        return InitializeVHDDiskpart(vhdPath);
    }
    LONGLONG totalDiskSize = geom.DiskSize.QuadPart;

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

    ::DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES,
                      nullptr, 0, nullptr, 0, &bytesReturned, nullptr);

    constexpr LONGLONG kPartitionOffsetBytes = 1048576;
    constexpr LONGLONG kTailPaddingBytes = 1048576;

    LONGLONG partitionLength = totalDiskSize - kPartitionOffsetBytes - kTailPaddingBytes;
    if (partitionLength <= 0) {
        ::CloseHandle(hDisk);
        return ERROR_DISK_TOO_FRAGMENTED;
    }

    // DRIVE_LAYOUT_INFORMATION_EX declares one PartitionEntry, so its sizeof
    // holds a one-partition layout.
    alignas(8) BYTE layoutBuf[sizeof(DRIVE_LAYOUT_INFORMATION_EX)]{};
    auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(layoutBuf);

    layout->PartitionStyle = PARTITION_STYLE_GPT;
    layout->PartitionCount = 1;
    layout->Gpt.DiskId = diskId;
    layout->Gpt.StartingUsableOffset.QuadPart = kPartitionOffsetBytes;
    layout->Gpt.UsableLength.QuadPart = totalDiskSize - kPartitionOffsetBytes - kTailPaddingBytes;
    layout->Gpt.MaxPartitionCount = 128;

    GUID partId{};
    ::CoCreateGuid(&partId);

    layout->PartitionEntry[0].PartitionStyle = PARTITION_STYLE_GPT;
    layout->PartitionEntry[0].StartingOffset.QuadPart = kPartitionOffsetBytes;
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

    ::DeviceIoControl(hDisk, IOCTL_DISK_UPDATE_PROPERTIES,
                      nullptr, 0, nullptr, 0, &bytesReturned, nullptr);

    ::CloseHandle(hDisk);

    // The OS adds the new volume some time after the layout change, so the
    // lookup retries with a longer delay each time.
    std::wstring volumeGuid;
    DWORD discoverResult = ERROR_NOT_FOUND;
    const DWORD delays[] = { 100, 200, 400, 800, 1600 };

    for (DWORD delay : delays) {
        discoverResult = GetVolumeGuidForPhysicalDisk(physicalDiskPath, volumeGuid);
        if (discoverResult == ERROR_SUCCESS) break;
        if (discoverResult != ERROR_NOT_FOUND &&
            discoverResult != ERROR_DEV_NOT_EXIST &&
            discoverResult != ERROR_NOT_READY) {
            return discoverResult;
        }
        ::Sleep(delay);
    }

    if (discoverResult != ERROR_SUCCESS) {
        return InitializeVHDDiskpart(vhdPath);
    }

    return FormatVolume(volumeGuid, L"LayerMount");
}

DWORD VHDLayerManager::FormatVolume(const std::wstring& volumeGuid,
                                     const std::wstring& label) {
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

DWORD VHDLayerManager::InitializeVHDDiskpart(const std::wstring& vhdPath) {
    std::wstring script =
        L"SELECT VDISK FILE=\"" + vhdPath + L"\"\r\n"
        L"ATTACH VDISK\r\n"
        L"ONLINE DISK\r\n"
        L"ATTRIBUTES DISK CLEAR READONLY\r\n"
        L"CREATE PARTITION PRIMARY\r\n"
        L"FORMAT FS=NTFS LABEL=\"LayerMount\" QUICK\r\n";

    wchar_t tempDir[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, tempDir);

    std::wstring scriptPath = std::wstring(tempDir) + L"ovfs_diskpart_" + GenerateId() + L".txt";

    HANDLE hFile = ::CreateFileW(scriptPath.c_str(), GENERIC_WRITE, 0,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return ::GetLastError();

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
    // diskpart needs the BOM to read the script as UTF-16 LE.
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

DWORD VHDLayerManager::ImportDirectory(const std::wstring& directoryPath,
                                        const std::wstring& vhdPath,
                                        ULONGLONG sizeBytes) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    namespace fs = std::filesystem;

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
        const ULONGLONG overhead = totalSize / 5;
        if (overhead > (std::numeric_limits<ULONGLONG>::max)() - totalSize) {
            return ERROR_ARITHMETIC_OVERFLOW;
        }
        sizeBytes = totalSize + overhead;
        constexpr ULONGLONG kMinSize = 100ULL * 1024 * 1024;
        if (sizeBytes < kMinSize) sizeBytes = kMinSize;
    }

    VhdHandle createHandle;
    result = CreateVHD(vhdPath, sizeBytes, true, createHandle);
    if (result != ERROR_SUCCESS) return result;
    createHandle.Close();

    VhdHandle attachHandle;
    std::wstring physicalPath;
    result = AttachVHD(vhdPath, false, attachHandle, physicalPath,
                       AttachLifetime::ProcessScoped,
                       /*suppressDriveLetter=*/ true);
    if (result != ERROR_SUCCESS) {
        fs::remove(vhdPath);
        return result;
    }

    result = InitializeVHD(physicalPath, vhdPath);
    if (result != ERROR_SUCCESS) {
        attachHandle.Close();
        DetachVHD(vhdPath);
        fs::remove(vhdPath);
        return result;
    }

    std::wstring volumeGuid;
    result = GetVolumeGuidForPhysicalDisk(physicalPath, volumeGuid);
    if (result != ERROR_SUCCESS) {
        attachHandle.Close();
        DetachVHD(vhdPath);
        fs::remove(vhdPath);
        return result;
    }

    std::wstring tempMount = JoinDirPath(workingDir_, L"temp_mounts\\" + GenerateId());
    fs::create_directories(tempMount);
    std::wstring tempMountSlash = EnsureTrailingBackslash(tempMount);

    if (!::SetVolumeMountPointW(tempMountSlash.c_str(), volumeGuid.c_str())) {
        DWORD err = ::GetLastError();
        fs::remove_all(tempMount);
        attachHandle.Close();
        DetachVHD(vhdPath);
        fs::remove(vhdPath);
        return err;
    }

    std::error_code ec;
    fs::copy(directoryPath, tempMount,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);

    DWORD copyResult = ec ? ERROR_WRITE_FAULT : ERROR_SUCCESS;

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
    DetachVHD(vhdPath);

    if (copyResult != ERROR_SUCCESS) {
        fs::remove(vhdPath, rmEc);
        return copyResult;
    }
    return cleanupErr;
}

DWORD VHDLayerManager::ExportToDirectory(const std::wstring& vhdPath,
                                          const std::wstring& directoryPath) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    namespace fs = std::filesystem;

    VhdHandle attachHandle;
    std::wstring physicalPath;
    result = AttachVHD(vhdPath, true, attachHandle, physicalPath,
                       AttachLifetime::ProcessScoped,
                       /*suppressDriveLetter=*/ true);
    if (result != ERROR_SUCCESS) return result;

    std::wstring volumeGuid;
    result = GetVolumeGuidForPhysicalDisk(physicalPath, volumeGuid);
    if (result != ERROR_SUCCESS) {
        attachHandle.Close();
        DetachVHD(vhdPath);
        return result;
    }

    std::wstring tempMount = JoinDirPath(workingDir_, L"temp_mounts\\" + GenerateId());
    fs::create_directories(tempMount);
    std::wstring tempMountSlash = EnsureTrailingBackslash(tempMount);

    if (!::SetVolumeMountPointW(tempMountSlash.c_str(), volumeGuid.c_str())) {
        DWORD err = ::GetLastError();
        std::error_code ec;
        fs::remove_all(tempMount, ec);
        attachHandle.Close();
        DetachVHD(vhdPath);
        return err;
    }

    fs::create_directories(directoryPath);

    // NTFS volume internals. System Volume Information grants access only to
    // SYSTEM, so even an elevated copy fails on it.
    static const wchar_t* const kSkipAtRoot[] = {
        L"System Volume Information",
        L"$RECYCLE.BIN",
    };

    const fs::path srcRoot = tempMount;
    const fs::path dstRoot = directoryPath;

    // An entry that fails to copy, such as a file with a restrictive ACL,
    // does not stop the export of the others.
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

    DWORD copyResult = (ec && copiedEntries == 0) ? ERROR_READ_FAULT
                                                   : ERROR_SUCCESS;

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
    DetachVHD(vhdPath);

    if (copyResult != ERROR_SUCCESS) return copyResult;
    return cleanupErr;
}

}
