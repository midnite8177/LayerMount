#pragma once

// <virtdisk.h> ships only inside LayerMount.dll.
// External consumers reach VHD functionality via the C ABI (LayerMountVhd*
// exports in <LayerMount.h>); they must not see this header. The DLL and
// its static-lib flavor define LAYERMOUNT_INTERNAL via the shared sources
// props; any other TU that reaches here is misconfigured.
#ifndef LAYERMOUNT_INTERNAL
#error "<impl/vhd/VHDLayerManager.h> is internal to LayerMount.dll. Use the LayerMountVhd* C ABI (public/LayerMount.h) from external consumers."
#endif

#include <windows.h>
#include <virtdisk.h>
#include <winioctl.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <optional>

#include "../HostPath.h"
#include "../PathUtil.h"

namespace LayerMount::VHD {

// Whether an attach survives the creating process.
// Permanent       — passes ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME.
//                   The VHD stays attached even after handle close / process
//                   death. Use for standalone `layermount vhd attach`.
// ProcessScoped   — no permanent flag. OS releases the attach when the
//                   process handle closes (normal exit, crash, TerminateProcess).
//                   Use for mount-child VHD attaches so hard-kill cleanup
//                   happens automatically.
enum class AttachLifetime { Permanent, ProcessScoped };

// Dynamic expands as data is written; Fixed allocates the full size when the
// file is created.
enum class VhdAllocation { Fixed, Dynamic };

enum class AttachAccess { ReadOnly, ReadWrite };

// Suppress passes ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER so the Windows
// Mount Manager does not assign a drive letter to the attached volume. Use it
// for a transient attach, such as an import, an export or a mount backing,
// where a drive letter only opens an AutoPlay window.
enum class DriveLetter { Assign, Suppress };

// RAII wrapper for a HANDLE from the VHD APIs. Calls CloseHandle only in the
// destructor. Does NOT call DetachVirtualDisk.
// For attaches made with AttachLifetime::Permanent, the VHD stays attached
// even after the handle is closed. For AttachLifetime::ProcessScoped, the
// attach is tied to the process and is released by the OS on process death.
class VhdHandle {
public:
    VhdHandle() noexcept : handle_(INVALID_HANDLE_VALUE) {}
    explicit VhdHandle(HANDLE h) noexcept : handle_(h) {}
    ~VhdHandle() { Close(); }

    VhdHandle(const VhdHandle&) = delete;
    VhdHandle& operator=(const VhdHandle&) = delete;

    VhdHandle(VhdHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }

    VhdHandle& operator=(VhdHandle&& other) noexcept {
        if (this != &other) {
            Close();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HANDLE Get() const noexcept { return handle_; }

    // For use as an out-parameter: closes current handle, returns pointer to
    // internal storage so callers like CreateVirtualDisk can write directly.
    HANDLE* Put() noexcept {
        Close();
        return &handle_;
    }

    HANDLE Release() noexcept {
        HANDLE h = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return h;
    }

    void Close() noexcept {
        if (IsValid()) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool IsValid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }

    explicit operator bool() const noexcept { return IsValid(); }

private:
    HANDLE handle_;
};

struct AttachOptions {
    AttachAccess access;
    AttachLifetime lifetime;
    DriveLetter driveLetter;
};

// physicalPath is the \\.\PhysicalDriveN path of the attached disk.
struct AttachedVhd {
    VhdHandle handle;
    std::wstring physicalPath;
};

// The part of an export's copy walk that failed on an entry.
enum class ExportStep { Copy, List };

// What VHDLayerManager::ExportToDirectory returns. `error` is the Win32
// error, or ERROR_SUCCESS. When the copy walk failed, `entry` is the failing
// entry's path from the volume root, such as `\sub\file.txt`, or `\` for
// the root, and `step` says whether the walk could not copy that entry or
// list it. A failure outside the walk, such as elevation, attach, mount or
// unmount, leaves `entry` empty, and `step` then means nothing.
struct ExportResult {
    DWORD error;
    std::wstring entry;
    ExportStep step;
};

class VHDLayerManager {
public:
    explicit VHDLayerManager(const HostPath& workingDir);

    VHDLayerManager(const VHDLayerManager&) = delete;
    VHDLayerManager& operator=(const VHDLayerManager&) = delete;

    // Check whether the current process is running elevated (admin).
    // Returns ERROR_SUCCESS if elevated, ERROR_PRIVILEGE_NOT_HELD (1314) if not.
    static DWORD CheckElevation();

    // Closes the new file before returning, so a later attach can open it.
    DWORD CreateVHD(const HostPath& path, ULONGLONG sizeBytes,
                    VhdAllocation allocation);

    DWORD AttachVHD(const HostPath& path, const AttachOptions& options,
                    AttachedVhd& out);
    // Opens the VHD again, which fails while this process holds an attach
    // handle on it; use the handle overload then.
    DWORD DetachVHD(const HostPath& path);
    DWORD DetachVHD(const VhdHandle& attachHandle);

    // Closes the new file before returning, so a later attach can open it.
    DWORD CreateDifferencingVHD(const HostPath& childPath,
                                const HostPath& parentPath);

    DWORD MergeVHD(const HostPath& childPath);

    // Initialize a newly attached VHD: partition (GPT) and format (NTFS).
    // |physicalDiskPath| is the \\.\PhysicalDriveN path from AttachVHD.
    // |vhdPath| is the original VHDX file path (needed for diskpart fallback).
    DWORD InitializeVHD(const std::wstring& physicalDiskPath,
                        const HostPath& vhdPath);

    // With std::nullopt for sizeBytes, the VHD capacity is the size of the
    // files to import plus a fifth for file system overhead, and at least
    // 100 MiB. A file whose size cannot be read counts as empty. The result is
    // ERROR_ARITHMETIC_OVERFLOW when that capacity does not fit in a
    // ULONGLONG.
    DWORD ImportDirectory(const HostPath& directoryPath,
                          const HostPath& vhdPath,
                          std::optional<ULONGLONG> sizeBytes);

    // Copies the volume's user content into `directoryPath` and stops at the
    // first entry it cannot copy or list. An entry that is not a file, a
    // directory or a symbolic link, such as a junction, fails with
    // ERROR_NOT_SUPPORTED. The destination keeps the entries copied before
    // the failure.
    ExportResult ExportToDirectory(const HostPath& vhdPath,
                                   const HostPath& directoryPath);

private:
    DWORD OpenVHD(const HostPath& path, VhdHandle& outHandle);

    DWORD InitializeVHDDiskpart(const HostPath& vhdPath);

    // |volumeGuid| must include trailing backslash: \\?\Volume{GUID}\.
    DWORD FormatVolume(const std::wstring& volumeGuid, const std::wstring& label);

    static std::wstring GenerateId();

    HostPath workingDir_;
};

// UTF-16 <-> UTF-8 conversion for JSON serialization.
std::string WideToUtf8(const std::wstring& wide);
std::wstring Utf8ToWide(const std::string& utf8);

// Returns path with a trailing backslash added, unless it already has one.
std::wstring EnsureTrailingBackslash(const std::wstring& path);

}
