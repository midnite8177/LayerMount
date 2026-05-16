#pragma once

#include <windows.h>
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <optional>

// Forward declaration — full definition in Manifest.h (LayerMount.VHD project)
namespace LayerMount::VHD { class Manifest; }

namespace LayerMount::VSS {

// ---------------------------------------------------------------------------
// SnapshotInfo — public metadata for a tracked VSS snapshot
// ---------------------------------------------------------------------------

struct SnapshotInfo {
    std::wstring id;           // GUID string (no braces)
    VSS_ID       vssId;        // Raw VSS GUID for API calls
    std::wstring volumePath;   // Source volume root (e.g., L"C:\\")
    std::wstring devicePath;   // NO trailing backslash
    bool         persistent;   // true = CLIENT_ACCESSIBLE, false = BACKUP
    std::wstring createdAt;    // ISO 8601
};

// ---------------------------------------------------------------------------
// VSSManager — VSS snapshot lifecycle management
// ---------------------------------------------------------------------------
// Follows the VHDLayerManager pattern: all public methods return DWORD
// (Windows error codes), output via reference parameters, no exceptions.
//
// Preconditions:
//   - Caller must initialize COM before using VSSManager
//     (CoInitializeEx with COINIT_MULTITHREADED recommended).
//   - VSS operations require administrator elevation.

class VSSManager {
public:
    // |manifest| is optional. If non-null, snapshot create/delete operations
    // will be recorded in the manifest as LayerType::VSS entries.
    explicit VSSManager(::LayerMount::VHD::Manifest* manifest = nullptr);
    ~VSSManager();

    VSSManager(const VSSManager&) = delete;
    VSSManager& operator=(const VSSManager&) = delete;

    // Check whether the current process is running elevated (admin).
    // Returns ERROR_SUCCESS if elevated, ERROR_PRIVILEGE_NOT_HELD (1314) if not.
    static DWORD CheckElevation();

    // 5.1 — Create a VSS snapshot of the given volume.
    // |volumePath| must be a volume root path with trailing backslash (e.g., L"C:\\").
    // |persistent| = true for CLIENT_ACCESSIBLE (survives reboot),
    //                false for BACKUP (auto-deleted when VSSManager is destroyed).
    // Returns snapshot ID and device path (without trailing backslash) via output params.
    DWORD CreateSnapshot(const std::wstring& volumePath, bool persistent,
                         std::wstring& outSnapshotId, std::wstring& outDevicePath);

    // 5.5 — Delete a specific snapshot by ID.
    // Persistent snapshots: creates a new IVssBackupComponents to call DeleteSnapshots.
    // Non-persistent snapshots: releases the held IVssBackupComponents (auto-deletes).
    DWORD DeleteSnapshot(const std::wstring& snapshotId);

    // 5.5 — Delete all non-persistent snapshots.
    // Call after LayerMount::Unmount() to clean up session-scoped snapshots.
    DWORD CleanupNonPersistent();

    // Verify that a tracked snapshot's device path is still accessible.
    // Returns ERROR_SUCCESS if GetFileAttributesW succeeds on the device path.
    DWORD ValidateSnapshotPath(const std::wstring& snapshotId);

    // 5.4 — Query methods.
    // ListSnapshots enumerates ALL snapshots present on the system via
    // IVssBackupComponents::Query under VSS_CTX_ALL — not just those created
    // in-process. Ownership (which tool created a snapshot) is not tracked;
    // all snapshots visible to VSS are returned with their persistence and
    // creation-timestamp metadata populated from VSS_SNAPSHOT_PROP.
    //
    // Returns ERROR_SUCCESS on success (including "no snapshots exist") and
    // a Win32 error code when the VSS provider, InitializeForBackup,
    // SetContext, or Query step failed -- previously these failures were
    // silently translated into an empty result, which masked VSS service
    // problems as "zero snapshots." `out` is always cleared on entry and
    // populated only on success.
    DWORD ListSnapshots(std::vector<SnapshotInfo>& out) const;
    std::optional<SnapshotInfo> GetSnapshot(const std::wstring& id) const;

private:
    // RAII wrapper for IVssBackupComponents* — prevents leaks during map
    // operations (move/erase). Parallel to VhdHandle in VHDLayerManager.
    struct VssBackupPtr {
        IVssBackupComponents* p = nullptr;

        VssBackupPtr() = default;
        explicit VssBackupPtr(IVssBackupComponents* ptr) : p(ptr) {}

        ~VssBackupPtr() {
            if (p) { p->Release(); p = nullptr; }
        }

        VssBackupPtr(const VssBackupPtr&) = delete;
        VssBackupPtr& operator=(const VssBackupPtr&) = delete;

        VssBackupPtr(VssBackupPtr&& o) noexcept : p(o.p) { o.p = nullptr; }

        VssBackupPtr& operator=(VssBackupPtr&& o) noexcept {
            if (this != &o) {
                if (p) p->Release();
                p = o.p;
                o.p = nullptr;
            }
            return *this;
        }
    };

    // Internal snapshot record. For non-persistent snapshots, |backup| holds
    // the IVssBackupComponents alive (releasing it auto-deletes the snapshot).
    // For persistent snapshots, |backup| is empty.
    struct HeldSnapshot {
        SnapshotInfo info;
        VssBackupPtr backup;
    };

    // Wait for an IVssAsync operation and release it. Returns translated DWORD.
    static DWORD WaitForAsync(IVssAsync* pAsync);

    // Translate HRESULT to DWORD error code.
    static DWORD HResultToDword(HRESULT hr);

    // GUID <-> string conversion (no braces).
    static std::wstring GuidToString(const VSS_ID& guid);
    static bool StringToGuid(const std::wstring& str, VSS_ID& outGuid);

    // Generate an ISO 8601 timestamp for the current time.
    static std::wstring NowTimestamp();

    // Format a VSS_TIMESTAMP (FILETIME-based 100ns-since-1601) as ISO 8601 UTC.
    static std::wstring FormatVssTimestamp(VSS_TIMESTAMP ts);

    // In-memory snapshot registry.
    std::map<std::wstring, HeldSnapshot> snapshots_;

    // Optional manifest integration.
    ::LayerMount::VHD::Manifest* manifest_;

    mutable std::mutex mutex_;
};

} // namespace LayerMount::VSS
