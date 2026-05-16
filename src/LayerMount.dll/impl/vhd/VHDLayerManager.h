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
#include <mutex>
#include <filesystem>

#include "Manifest.h"

namespace LayerMount::VHD {

// ---------------------------------------------------------------------------
// AttachLifetime — whether an attach survives the creating process.
// ---------------------------------------------------------------------------
// Permanent       — passes ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME.
//                   The VHD stays attached even after handle close / process
//                   death. Use for standalone `layermount vhd attach`.
// ProcessScoped   — no permanent flag. OS releases the attach when the
//                   process handle closes (normal exit, crash, TerminateProcess).
//                   Use for mount-child VHD attaches so hard-kill cleanup
//                   happens automatically.
enum class AttachLifetime { Permanent, ProcessScoped };

// ---------------------------------------------------------------------------
// VhdHandle — RAII wrapper for HANDLE from VHD APIs
// ---------------------------------------------------------------------------
// Calls CloseHandle only in destructor. Does NOT call DetachVirtualDisk.
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

// ---------------------------------------------------------------------------
// VHDLayerManager — VHD lifecycle management
// ---------------------------------------------------------------------------

class VHDLayerManager {
public:
    explicit VHDLayerManager(const std::wstring& workingDir);
    ~VHDLayerManager();

    VHDLayerManager(const VHDLayerManager&) = delete;
    VHDLayerManager& operator=(const VHDLayerManager&) = delete;

    // Check whether the current process is running elevated (admin).
    // Returns ERROR_SUCCESS if elevated, ERROR_PRIVILEGE_NOT_HELD (1314) if not.
    static DWORD CheckElevation();

    // 4.1 — Create a new VHDX file.
    // |dynamic| = true for sparse/expandable, false for fixed allocation.
    DWORD CreateVHD(const std::wstring& path, ULONGLONG sizeBytes, bool dynamic,
                    VhdHandle& outHandle);

    // 4.2 — Attach (mount) / detach a VHD.
    // |lifetime| controls whether the attach survives process death. Default
    // is Permanent for backward compatibility with existing callers; the
    // mount-child code path in MountCommand passes ProcessScoped explicitly.
    // |suppressDriveLetter| adds ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER so
    // the Windows Mount Manager does NOT auto-assign a drive letter to the
    // attached volume. Use for transient attaches (import/export/mount-backing)
    // where a drive letter would only trigger a disruptive AutoPlay flash.
    DWORD AttachVHD(const std::wstring& path, bool readOnly,
                    VhdHandle& outHandle, std::wstring& outPhysicalPath,
                    AttachLifetime lifetime = AttachLifetime::Permanent,
                    bool suppressDriveLetter = false);
    DWORD DetachVHD(const std::wstring& path);

    // 4.3 — Create a differencing (child) VHDX whose parent is |parentPath|.
    DWORD CreateDifferencingVHD(const std::wstring& childPath,
                                const std::wstring& parentPath,
                                VhdHandle& outHandle);

    // 4.4 — Merge a child VHDX's changes back into its parent.
    DWORD MergeVHD(const std::wstring& childPath);

    // 4.5 — Initialize a newly attached VHD: partition (GPT) and format (NTFS).
    // |physicalDiskPath| is the \\.\PhysicalDriveN path from AttachVHD.
    // |vhdPath| is the original VHDX file path (needed for diskpart fallback).
    DWORD InitializeVHD(const std::wstring& physicalDiskPath,
                        const std::wstring& vhdPath);

    // 4.7 — Import a directory into a new VHDX.
    // If |sizeBytes| == 0, auto-calculate from directory contents + 20% overhead.
    DWORD ImportDirectory(const std::wstring& directoryPath,
                          const std::wstring& vhdPath,
                          ULONGLONG sizeBytes = 0);

    // 4.8 — Export a VHDX's contents to a directory.
    DWORD ExportToDirectory(const std::wstring& vhdPath,
                            const std::wstring& directoryPath);

    // Manifest access
    Manifest& GetManifest();
    const Manifest& GetManifest() const;

    const std::wstring& WorkingDirectory() const { return workingDir_; }

private:
    // Open an existing VHD/VHDX file. Uses OPEN_VIRTUAL_DISK_VERSION_1 with
    // VIRTUAL_DISK_ACCESS_ALL. Detects VHDX vs VHD by file extension.
    DWORD OpenVHD(const std::wstring& path, VhdHandle& outHandle);

    // Diskpart-based fallback for InitializeVHD.
    DWORD InitializeVHDDiskpart(const std::wstring& vhdPath);

    // Format a volume as NTFS via format.com.
    // |volumeGuid| must include trailing backslash: \\?\Volume{GUID}\.
    DWORD FormatVolume(const std::wstring& volumeGuid, const std::wstring& label);

    // Generate a new UUID string via CoCreateGuid + StringFromGUID2.
    static std::wstring GenerateId();

    std::wstring workingDir_;
    std::unique_ptr<Manifest> manifest_;
    ManifestLock manifestLock_;  // Held for the manager's lifetime to serialize
                                 // cross-process manifest Load-mutate-Save.
    std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// String conversion helpers (UTF-16 <-> UTF-8 for JSON serialization)
// ---------------------------------------------------------------------------

std::string WideToUtf8(const std::wstring& wide);
std::wstring Utf8ToWide(const std::string& utf8);

// Trailing backslash helpers for volume GUID paths
std::wstring EnsureTrailingBackslash(const std::wstring& path);
std::wstring StripTrailingBackslash(const std::wstring& path);

} // namespace LayerMount::VHD
