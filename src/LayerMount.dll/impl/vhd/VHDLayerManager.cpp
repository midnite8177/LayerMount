#include "VHDLayerManager.h"
#include "VolumeGuid.h"
#include "../ElevationUtil.h"
#include "../LayerMount.h"
#include "../LayerPath.h"
#include "../NtStatusUtil.h"
#include "../PathUtil.h"

#include <limits>

namespace LayerMount::VHD {

namespace {

// VirtDisk picks its default block or sector size for this value.
constexpr ULONG kSystemDefaultSize = 0;
// A differencing disk with this maximum size takes the size of its parent.
constexpr ULONGLONG kParentSize = 0;

}

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

VHDLayerManager::VHDLayerManager(const HostPath& workingDir)
    : workingDir_(workingDir)
{
    std::filesystem::create_directories(workingDir_.ForWin32());
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

DWORD VHDLayerManager::OpenVHD(const HostPath& path, VhdHandle& outHandle) {
    VIRTUAL_STORAGE_TYPE storageType{};
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    std::wstring ext = std::filesystem::path(path.ForWin32()).extension().wstring();
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
        path.ForWin32().c_str(),
        VIRTUAL_DISK_ACCESS_ALL,
        OPEN_VIRTUAL_DISK_FLAG_NONE,
        &openParams,
        outHandle.Put());

    return result;
}

DWORD VHDLayerManager::CreateVHD(const HostPath& path, ULONGLONG sizeBytes,
                                  VhdAllocation allocation) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    VIRTUAL_STORAGE_TYPE storageType{};
    storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    CREATE_VIRTUAL_DISK_PARAMETERS createParams{};
    createParams.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    createParams.Version2.UniqueId = GUID_NULL;
    createParams.Version2.MaximumSize = sizeBytes;
    createParams.Version2.BlockSizeInBytes = kSystemDefaultSize;
    createParams.Version2.SectorSizeInBytes = kSystemDefaultSize;
    createParams.Version2.ParentPath = nullptr;
    createParams.Version2.SourcePath = nullptr;
    createParams.Version2.PhysicalSectorSizeInBytes = 0;

    CREATE_VIRTUAL_DISK_FLAG flags = (allocation == VhdAllocation::Dynamic)
        ? CREATE_VIRTUAL_DISK_FLAG_NONE
        : CREATE_VIRTUAL_DISK_FLAG_FULL_PHYSICAL_ALLOCATION;

    VhdHandle created;
    // Version 2 create parameters require VIRTUAL_DISK_ACCESS_NONE.
    result = ::CreateVirtualDisk(
        &storageType,
        path.ForWin32().c_str(),
        VIRTUAL_DISK_ACCESS_NONE,
        nullptr,
        flags,
        0,
        &createParams,
        nullptr,
        created.Put());

    return result;
}

DWORD VHDLayerManager::AttachVHD(const HostPath& path, const AttachOptions& options,
                                  AttachedVhd& out) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    out.physicalPath.clear();

    VhdHandle handle;
    result = OpenVHD(path, handle);
    if (result != ERROR_SUCCESS) return result;

    ATTACH_VIRTUAL_DISK_PARAMETERS attachParams{};
    attachParams.Version = ATTACH_VIRTUAL_DISK_VERSION_1;

    // Without PERMANENT_LIFETIME the OS detaches the disk when the attach
    // handle closes, including when the process dies.
    ATTACH_VIRTUAL_DISK_FLAG flags =
        (options.lifetime == AttachLifetime::Permanent)
            ? ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME
            : ATTACH_VIRTUAL_DISK_FLAG_NONE;
    if (options.access == AttachAccess::ReadOnly) {
        flags = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(
            flags | ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY);
    }
    if (options.driveLetter == DriveLetter::Suppress) {
        flags = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(
            flags | ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER);
    }

    result = ::AttachVirtualDisk(
        handle.Get(),
        nullptr,
        flags,
        0,
        &attachParams,
        nullptr);

    if (result != ERROR_SUCCESS) return result;

    WCHAR diskPath[MAX_PATH]{};
    ULONG diskPathSize = sizeof(diskPath);
    result = ::GetVirtualDiskPhysicalPath(handle.Get(), &diskPathSize, diskPath);
    if (result != ERROR_SUCCESS) {
        ::DetachVirtualDisk(handle.Get(), DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        return result;
    }

    out.physicalPath = diskPath;
    out.handle = std::move(handle);
    return ERROR_SUCCESS;
}

DWORD VHDLayerManager::DetachVHD(const HostPath& path) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    VhdHandle handle;
    result = OpenVHD(path, handle);
    if (result != ERROR_SUCCESS) return result;

    return DetachVHD(handle);
}

DWORD VHDLayerManager::DetachVHD(const VhdHandle& attachHandle) {
    const DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    return ::DetachVirtualDisk(
        attachHandle.Get(),
        DETACH_VIRTUAL_DISK_FLAG_NONE,
        0);
}

DWORD VHDLayerManager::CreateDifferencingVHD(const HostPath& childPath,
                                              const HostPath& parentPath) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    VIRTUAL_STORAGE_TYPE storageType{};
    storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHDX;
    storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;

    CREATE_VIRTUAL_DISK_PARAMETERS createParams{};
    createParams.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    createParams.Version2.UniqueId = GUID_NULL;
    createParams.Version2.MaximumSize = kParentSize;
    createParams.Version2.BlockSizeInBytes = kSystemDefaultSize;
    createParams.Version2.SectorSizeInBytes = kSystemDefaultSize;
    // VirtDisk removes the \\?\ prefix from a short parent. It keeps the
    // prefix in the parent locator only for a parent past MAX_PATH. See ADR 0008.
    const std::wstring parentForWin32 = parentPath.ForWin32();
    createParams.Version2.ParentPath = parentForWin32.c_str();
    createParams.Version2.SourcePath = nullptr;
    createParams.Version2.PhysicalSectorSizeInBytes = 0;

    VhdHandle created;
    // Version 2 create parameters require VIRTUAL_DISK_ACCESS_NONE.
    result = ::CreateVirtualDisk(
        &storageType,
        childPath.ForWin32().c_str(),
        VIRTUAL_DISK_ACCESS_NONE,
        nullptr,
        CREATE_VIRTUAL_DISK_FLAG_NONE,
        0,
        &createParams,
        nullptr,
        created.Put());

    return result;
}

DWORD VHDLayerManager::MergeVHD(const HostPath& childPath) {
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
        nullptr);

    return result;
}

static const GUID PARTITION_BASIC_DATA_ID =
    { 0xebd0a0a2, 0xb9e5, 0x4433, { 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 } };

DWORD VHDLayerManager::InitializeVHD(const std::wstring& physicalDiskPath,
                                      const HostPath& vhdPath) {
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

DWORD VHDLayerManager::InitializeVHDDiskpart(const HostPath& vhdPath) {
    // diskpart reads this path from a script file, not through a Win32 call,
    // so it gets the path as the caller gave it.
    std::wstring script =
        L"SELECT VDISK FILE=\"" + vhdPath.Text() + L"\"\r\n"
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

// The entries directly under `directoryPath` that an import copies. The list
// leaves out every entry whose name opens as `.overlay`, file or directory,
// in any case and with or without trailing dots or spaces. On a listing
// error the function sets `ec`.
static std::vector<std::filesystem::directory_entry> ImportedRootEntries(
        const HostPath& directoryPath, std::error_code& ec) {
    std::vector<std::filesystem::directory_entry> entries;
    std::filesystem::directory_iterator it(directoryPath.ForWin32(), ec);
    for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!FirstSegmentOpensAsSidecar(it->path().filename().wstring())) {
            entries.push_back(*it);
        }
    }
    return entries;
}

// Sets `sizeBytes` to the capacity that ImportDirectory documents for a
// std::nullopt size, counting the files in and under `rootEntries`.
static DWORD SizeToFit(const std::vector<std::filesystem::directory_entry>& rootEntries,
                       ULONGLONG& sizeBytes) {
    namespace fs = std::filesystem;
    ULONGLONG totalSize = 0;
    const auto addFileSize = [&totalSize](const fs::directory_entry& entry) {
        std::error_code fsec;
        if (!entry.is_regular_file(fsec)) return true;
        const auto fsz = entry.file_size(fsec);
        if (fsec) return true;
        const ULONGLONG add = static_cast<ULONGLONG>(fsz);
        if (add > (std::numeric_limits<ULONGLONG>::max)() - totalSize) return false;
        totalSize += add;
        return true;
    };
    for (const auto& rootEntry : rootEntries) {
        if (!addFileSize(rootEntry)) return ERROR_ARITHMETIC_OVERFLOW;
        std::error_code walkEc;
        for (const auto& entry : fs::recursive_directory_iterator(rootEntry.path(), walkEc)) {
            if (!addFileSize(entry)) return ERROR_ARITHMETIC_OVERFLOW;
        }
    }
    const ULONGLONG overhead = totalSize / 5;
    if (overhead > (std::numeric_limits<ULONGLONG>::max)() - totalSize) {
        return ERROR_ARITHMETIC_OVERFLOW;
    }
    sizeBytes = totalSize + overhead;
    constexpr ULONGLONG kMinSize = 100ULL * 1024 * 1024;
    if (sizeBytes < kMinSize) sizeBytes = kMinSize;
    return ERROR_SUCCESS;
}

namespace {

// A directory with a volume mounted on it. Unmount, or the destructor when
// Unmount was not called, removes the mount point and the directory.
class TempMount {
public:
    explicit TempMount(std::wstring directory)
        : directory_(std::move(directory)),
          mountPoint_(EnsureTrailingBackslash(directory_)) {}

    ~TempMount() {
        if (mounted_) Unmount();
    }

    TempMount(const TempMount&) = delete;
    TempMount& operator=(const TempMount&) = delete;

    // Makes the directory and mounts the volume on it. When the mount fails,
    // it removes the directory again and returns the error of the mount.
    DWORD Mount(const std::wstring& volumeGuid) {
        std::filesystem::create_directories(directory_);
        if (!::SetVolumeMountPointW(mountPoint_.c_str(), volumeGuid.c_str())) {
            const DWORD err = ::GetLastError();
            std::error_code ec;
            std::filesystem::remove_all(directory_, ec);
            return err;
        }
        mounted_ = true;
        return ERROR_SUCCESS;
    }

    // A mount point that is already gone is not an error. A directory that
    // does not go away is ERROR_DIR_NOT_EMPTY, unless the mount point
    // removal failed first.
    DWORD Unmount() noexcept {
        mounted_ = false;
        DWORD cleanupErr = ERROR_SUCCESS;
        if (!::DeleteVolumeMountPointW(mountPoint_.c_str())) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
                cleanupErr = err;
            }
        }
        std::error_code ec;
        std::filesystem::remove_all(directory_, ec);
        if (ec && cleanupErr == ERROR_SUCCESS) {
            cleanupErr = ERROR_DIR_NOT_EMPTY;
        }
        return cleanupErr;
    }

    const std::wstring& Directory() const noexcept { return directory_; }

private:
    std::wstring directory_;
    std::wstring mountPoint_;
    bool mounted_ = false;
};

struct CopyFailure {
    std::error_code ec;
    std::filesystem::path entry;
    ExportStep step;
};

// Copies the link at `src` to `dst`, first removing a file or link already
// at `dst`, so a second export replaces the link as overwrite_existing
// replaces a file. symlink_status keeps a link at `dst` from being followed.
void CopySymlinkReplacing(const std::filesystem::path& src, const std::filesystem::path& dst,
                          std::error_code& ec) {
    namespace fs = std::filesystem;
    std::error_code probeEc;
    const fs::file_status existing = fs::symlink_status(dst, probeEc);
    if (fs::exists(existing) && !fs::is_directory(existing)) {
        fs::remove(dst, ec);
        if (ec) return;
    }
    fs::copy_symlink(src, dst, ec);
}

// fs::copy does not report which entry failed, so this copies `top` to
// `topDst` with the same options (recursive, overwrite_existing,
// copy_symlinks), walking depth first, and records the first entry it cannot
// copy or list. An entry that is not a file, a directory or a symbolic link,
// such as a junction, fails with ERROR_NOT_SUPPORTED; the walk neither
// follows it nor copies it as a link.
CopyFailure CopyTree(const std::filesystem::directory_entry& top,
                     const std::filesystem::path& topDst) {
    namespace fs = std::filesystem;
    struct Frame {
        fs::path source;
        fs::path dst;
        fs::directory_iterator it;
        // Whether `it` still points at an entry the walk has copied.
        bool advance;
    };
    std::vector<Frame> pending;

    const auto copyEntry = [&pending](const fs::directory_entry& entry,
                                      const fs::path& dst) -> CopyFailure {
        std::error_code ec;
        const fs::file_status status = entry.symlink_status(ec);
        if (!ec) {
            if (fs::is_symlink(status)) {
                CopySymlinkReplacing(entry.path(), dst, ec);
            } else if (fs::is_directory(status)) {
                fs::create_directory(dst, entry.path(), ec);
                if (!ec) {
                    fs::directory_iterator it(entry.path(), ec);
                    if (ec) return {ec, entry.path(), ExportStep::List};
                    pending.push_back({entry.path(), dst, std::move(it), false});
                }
            } else if (fs::is_regular_file(status)) {
                fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing, ec);
            } else {
                ec = std::error_code(ERROR_NOT_SUPPORTED, std::system_category());
            }
        }
        if (ec) return {ec, entry.path(), ExportStep::Copy};
        return {};
    };

    CopyFailure failure = copyEntry(top, topDst);
    while (!failure.ec && !pending.empty()) {
        Frame& frame = pending.back();
        if (frame.advance) {
            std::error_code ec;
            frame.it.increment(ec);
            if (ec) return {ec, frame.source, ExportStep::List};
        }
        frame.advance = true;
        if (frame.it == fs::directory_iterator()) {
            pending.pop_back();
            continue;
        }
        // A copy, because copyEntry can grow `pending` and move the frame.
        const fs::directory_entry child = *frame.it;
        const fs::path childDst = frame.dst / child.path().filename();
        failure = copyEntry(child, childDst);
    }
    return failure;
}

// `entry` as a path from the root of the volume mounted at `volumeRoot`,
// such as `\sub\file.txt`.
std::wstring VolumePathOf(const std::filesystem::path& entry,
                          const std::filesystem::path& volumeRoot) {
    const std::filesystem::path relative = entry.lexically_relative(volumeRoot);
    return relative == L"." ? L"\\" : L"\\" + relative.wstring();
}

// Whether the export leaves out the entry `name` at the volume root when it
// copies into `dstRoot`.
bool SkippedAtVolumeRoot(const std::wstring& name, const std::filesystem::path& dstRoot) {
    // NTFS volume internals. System Volume Information grants access only to
    // SYSTEM, so even an elevated copy fails on it.
    static const wchar_t* const kSkipAtRoot[] = {
        L"System Volume Information",
        L"$RECYCLE.BIN",
    };
    for (const wchar_t* s : kSkipAtRoot) {
        if (_wcsicmp(name.c_str(), s) == 0) return true;
    }
    if (FirstSegmentOpensAsSidecar(name)) return true;
    // A name such as `OVERLA~1` can be the short name of a `.overlay`
    // already in the destination, and a copy under it would land inside.
    const std::optional<std::wstring> dstLongName = ExistingLongName(dstRoot, name);
    return dstLongName && FirstSegmentOpensAsSidecar(*dstLongName);
}

}

DWORD VHDLayerManager::ImportDirectory(const HostPath& directoryPath,
                                        const HostPath& vhdPath,
                                        std::optional<ULONGLONG> sizeBytes) {
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    namespace fs = std::filesystem;

    std::error_code listEc;
    const std::vector<fs::directory_entry> rootEntries =
        ImportedRootEntries(directoryPath, listEc);
    if (listEc) return ERROR_WRITE_FAULT;

    ULONGLONG capacity = 0;
    if (sizeBytes) {
        capacity = *sizeBytes;
    } else {
        result = SizeToFit(rootEntries, capacity);
        if (result != ERROR_SUCCESS) return result;
    }

    result = CreateVHD(vhdPath, capacity, VhdAllocation::Dynamic);
    if (result != ERROR_SUCCESS) return result;
    const std::wstring vhdFile = vhdPath.ForWin32();

    AttachedVhd attached;
    result = AttachVHD(vhdPath,
                       {AttachAccess::ReadWrite, AttachLifetime::ProcessScoped,
                        DriveLetter::Suppress},
                       attached);
    if (result != ERROR_SUCCESS) {
        fs::remove(vhdFile);
        return result;
    }

    const auto discardVhd = [&](DWORD error) {
        DetachVHD(attached.handle);
        attached.handle.Close();
        std::error_code removeEc;
        fs::remove(vhdFile, removeEc);
        return error;
    };

    result = InitializeVHD(attached.physicalPath, vhdPath);
    if (result != ERROR_SUCCESS) return discardVhd(result);

    std::wstring volumeGuid;
    result = GetVolumeGuidForPhysicalDisk(attached.physicalPath, volumeGuid);
    if (result != ERROR_SUCCESS) return discardVhd(result);

    TempMount mount(JoinDirPath(workingDir_.ForWin32(), L"temp_mounts\\" + GenerateId()));
    result = mount.Mount(volumeGuid);
    if (result != ERROR_SUCCESS) return discardVhd(result);

    std::error_code ec;
    for (const auto& rootEntry : rootEntries) {
        fs::copy(rootEntry.path(), fs::path(mount.Directory()) / rootEntry.path().filename(),
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) break;
    }

    const DWORD copyResult = ec ? ERROR_WRITE_FAULT : ERROR_SUCCESS;
    const DWORD cleanupErr = mount.Unmount();

    if (copyResult != ERROR_SUCCESS) return discardVhd(copyResult);
    DetachVHD(attached.handle);
    attached.handle.Close();
    return cleanupErr;
}

ExportResult VHDLayerManager::ExportToDirectory(const HostPath& vhdPath,
                                                const HostPath& directoryPath) {
    const auto withoutEntry = [](DWORD error) {
        return ExportResult{error, std::wstring(), ExportStep::Copy};
    };

    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return withoutEntry(result);

    namespace fs = std::filesystem;

    AttachedVhd attached;
    result = AttachVHD(vhdPath,
                       {AttachAccess::ReadOnly, AttachLifetime::ProcessScoped,
                        DriveLetter::Suppress},
                       attached);
    if (result != ERROR_SUCCESS) return withoutEntry(result);

    const auto detachVhd = [&](DWORD error) {
        DetachVHD(attached.handle);
        attached.handle.Close();
        return error;
    };

    std::wstring volumeGuid;
    result = GetVolumeGuidForPhysicalDisk(attached.physicalPath, volumeGuid);
    if (result != ERROR_SUCCESS) return withoutEntry(detachVhd(result));

    TempMount mount(JoinDirPath(workingDir_.ForWin32(), L"temp_mounts\\" + GenerateId()));
    result = mount.Mount(volumeGuid);
    if (result != ERROR_SUCCESS) return withoutEntry(detachVhd(result));

    const fs::path dstRoot = directoryPath.ForWin32();
    fs::create_directories(dstRoot);

    const fs::path srcRoot = mount.Directory();
    CopyFailure failure{};

    std::error_code listEc;
    fs::directory_iterator top(srcRoot, listEc);
    for (; !listEc && top != fs::directory_iterator(); top.increment(listEc)) {
        const fs::directory_entry& topEntry = *top;
        if (SkippedAtVolumeRoot(topEntry.path().filename().wstring(), dstRoot)) continue;
        failure = CopyTree(topEntry, dstRoot / topEntry.path().filename());
        if (failure.ec) break;
    }
    if (listEc) failure = {listEc, srcRoot, ExportStep::List};

    const DWORD cleanupErr = detachVhd(mount.Unmount());

    if (failure.ec) {
        return {Win32FromErrorCode(failure.ec), VolumePathOf(failure.entry, srcRoot),
                failure.step};
    }
    return withoutEntry(cleanupErr);
}

}
