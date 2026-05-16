#include "VSSManager.h"
#include "Manifest.h"

#include <objbase.h>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace LayerMount::VSS {

// ===========================================================================
// Helpers — shared with VHDLayerManager (duplicated here to avoid cross-lib
// link dependency; these are small, self-contained utilities).
// ===========================================================================

static std::wstring StripTrailingBackslash(const std::wstring& path) {
    if (!path.empty() && path.back() == L'\\')
        return path.substr(0, path.size() - 1);
    return path;
}

// Extract a Win32 DWORD from an HRESULT. Used to surface VSS COM
// failures through the ListSnapshots DWORD contract: if the HRESULT
// already wraps a Win32 error (FACILITY_WIN32) we unwrap it; otherwise
// fall back to ERROR_GEN_FAILURE so callers see a non-zero code.
static DWORD Win32FromHresult(HRESULT hr) {
    if (hr == S_OK) return ERROR_SUCCESS;
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        return static_cast<DWORD>(HRESULT_CODE(hr));
    }
    return ERROR_GEN_FAILURE;
}

// ===========================================================================
// VSSManager — construction / destruction
// ===========================================================================

VSSManager::VSSManager(LayerMount::VHD::Manifest* manifest)
    : manifest_(manifest) {
}

VSSManager::~VSSManager() {
    // Releasing HeldSnapshot::VssBackupPtr instances auto-deletes non-persistent
    // snapshots. Persistent snapshots are not deleted (by design).
    // std::map::clear triggers VssBackupPtr destructors via HeldSnapshot.
    snapshots_.clear();
}

// ===========================================================================
// CheckElevation — same pattern as VHDLayerManager
// ===========================================================================

DWORD VSSManager::CheckElevation() {
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
// WaitForAsync — wait for IVssAsync, query status, release
// ===========================================================================

DWORD VSSManager::WaitForAsync(IVssAsync* pAsync) {
    if (!pAsync) return ERROR_INVALID_PARAMETER;

    HRESULT hr = pAsync->Wait();
    if (FAILED(hr)) {
        pAsync->Release();
        return HResultToDword(hr);
    }

    HRESULT hrStatus = S_OK;
    hr = pAsync->QueryStatus(&hrStatus, nullptr);
    pAsync->Release();

    if (FAILED(hr)) return HResultToDword(hr);
    if (FAILED(hrStatus)) return HResultToDword(hrStatus);

    return ERROR_SUCCESS;
}

// ===========================================================================
// HResultToDword — granular HRESULT-to-DWORD mapping
// ===========================================================================

DWORD VSSManager::HResultToDword(HRESULT hr) {
    if (SUCCEEDED(hr)) return ERROR_SUCCESS;

    switch (hr) {
        case E_ACCESSDENIED:      return ERROR_ACCESS_DENIED;
        case E_OUTOFMEMORY:       return ERROR_NOT_ENOUGH_MEMORY;
        case E_INVALIDARG:        return ERROR_INVALID_PARAMETER;
        case CO_E_NOTINITIALIZED: return ERROR_NOT_READY;
    }

    // VSS-specific errors
    switch (static_cast<DWORD>(hr)) {
        case VSS_E_OBJECT_NOT_FOUND:                    return ERROR_NOT_FOUND;
        case VSS_E_VOLUME_NOT_SUPPORTED:                return ERROR_NOT_SUPPORTED;
        case VSS_E_BAD_STATE:                           return ERROR_BAD_COMMAND;
        case VSS_E_MAXIMUM_NUMBER_OF_SNAPSHOTS_REACHED: return ERROR_DISK_FULL;
        case VSS_E_INSUFFICIENT_STORAGE:                return ERROR_DISK_FULL;
        case VSS_E_PROVIDER_NOT_REGISTERED:             return ERROR_SERVICE_NOT_FOUND;
    }

    // Extract Win32 code if the facility is FACILITY_WIN32
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        return HRESULT_CODE(hr);
    }

    return ERROR_UNIDENTIFIED_ERROR;
}

// ===========================================================================
// GUID <-> string conversion
// ===========================================================================

std::wstring VSSManager::GuidToString(const VSS_ID& guid) {
    // StringFromGUID2 produces {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
    wchar_t buf[40]{};
    int len = ::StringFromGUID2(guid, buf, 40);
    if (len == 0) return L"";

    std::wstring id(buf);
    // Strip braces for a cleaner ID
    if (id.size() >= 2 && id.front() == L'{' && id.back() == L'}') {
        id = id.substr(1, id.size() - 2);
    }
    return id;
}

bool VSSManager::StringToGuid(const std::wstring& str, VSS_ID& outGuid) {
    // CLSIDFromString expects braces: {xxxxxxxx-xxxx-...}
    std::wstring braced = L"{" + str + L"}";
    HRESULT hr = ::CLSIDFromString(braced.c_str(), &outGuid);
    return SUCCEEDED(hr);
}

// ===========================================================================
// NowTimestamp — ISO 8601 UTC
// ===========================================================================

std::wstring VSSManager::NowTimestamp() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);

    std::wostringstream oss;
    oss << std::setfill(L'0')
        << (utc.tm_year + 1900) << L'-'
        << std::setw(2) << (utc.tm_mon + 1) << L'-'
        << std::setw(2) << utc.tm_mday << L'T'
        << std::setw(2) << utc.tm_hour << L':'
        << std::setw(2) << utc.tm_min << L':'
        << std::setw(2) << utc.tm_sec << L'Z';
    return oss.str();
}

std::wstring VSSManager::FormatVssTimestamp(VSS_TIMESTAMP ts) {
    // VSS_TIMESTAMP is a LONGLONG representing 100-nanosecond intervals
    // since January 1, 1601 UTC — same encoding as FILETIME.
    FILETIME ft{};
    ft.dwLowDateTime  = static_cast<DWORD>(ts & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>((ts >> 32) & 0xFFFFFFFFULL);

    SYSTEMTIME st{};
    if (!::FileTimeToSystemTime(&ft, &st)) return L"";

    std::wostringstream oss;
    oss << std::setfill(L'0')
        << st.wYear << L'-'
        << std::setw(2) << st.wMonth << L'-'
        << std::setw(2) << st.wDay << L'T'
        << std::setw(2) << st.wHour << L':'
        << std::setw(2) << st.wMinute << L':'
        << std::setw(2) << st.wSecond << L'Z';
    return oss.str();
}

// ===========================================================================
// 5.1 + 5.3 — CreateSnapshot
// ===========================================================================

DWORD VSSManager::CreateSnapshot(const std::wstring& volumePath, bool persistent,
                                  std::wstring& outSnapshotId,
                                  std::wstring& outDevicePath) {
    std::lock_guard<std::mutex> lock(mutex_);

    // --- 1. Validate elevation ---
    DWORD result = CheckElevation();
    if (result != ERROR_SUCCESS) return result;

    // --- 2. Validate volumePath is a volume root (must end with backslash) ---
    if (volumePath.empty() || volumePath.back() != L'\\') {
        return ERROR_INVALID_PARAMETER;
    }

    // --- 3. Create a fresh IVssBackupComponents ---
    IVssBackupComponents* backup = nullptr;
    HRESULT hr = ::CreateVssBackupComponents(&backup);
    if (FAILED(hr)) return HResultToDword(hr);

    hr = backup->InitializeForBackup();
    if (FAILED(hr)) {
        backup->Release();
        return HResultToDword(hr);
    }

    LONG context = persistent ? VSS_CTX_CLIENT_ACCESSIBLE
                              : static_cast<LONG>(VSS_CTX_BACKUP);
    hr = backup->SetContext(context);
    if (FAILED(hr)) {
        backup->Release();
        return HResultToDword(hr);
    }

    // --- 4. For non-persistent: gather writer metadata for data consistency ---
    if (!persistent) {
        hr = backup->SetBackupState(FALSE, TRUE, VSS_BT_FULL, FALSE);
        if (FAILED(hr)) {
            backup->AbortBackup();
            backup->Release();
            return HResultToDword(hr);
        }

        IVssAsync* async = nullptr;
        hr = backup->GatherWriterMetadata(&async);
        if (FAILED(hr)) {
            backup->AbortBackup();
            backup->Release();
            return HResultToDword(hr);
        }
        result = WaitForAsync(async); // WaitForAsync releases async
        if (result != ERROR_SUCCESS) {
            backup->AbortBackup();
            backup->Release();
            return result;
        }

        backup->FreeWriterMetadata();
    }

    // --- 5. Start snapshot set ---
    VSS_ID snapshotSetId = GUID_NULL;
    hr = backup->StartSnapshotSet(&snapshotSetId);
    if (FAILED(hr)) {
        backup->AbortBackup();
        backup->Release();
        return HResultToDword(hr);
    }

    // --- 6. Add volume to snapshot set ---
    VSS_ID snapshotId = GUID_NULL;
    hr = backup->AddToSnapshotSet(
        const_cast<VSS_PWSZ>(volumePath.c_str()),
        GUID_NULL, &snapshotId);
    if (FAILED(hr)) {
        backup->AbortBackup();
        backup->Release();
        return HResultToDword(hr);
    }

    // --- 7. For non-persistent: prepare for backup (requires SetBackupState) ---
    if (!persistent) {
        IVssAsync* async = nullptr;
        hr = backup->PrepareForBackup(&async);
        if (FAILED(hr)) {
            backup->AbortBackup();
            backup->Release();
            return HResultToDword(hr);
        }
        result = WaitForAsync(async);
        if (result != ERROR_SUCCESS) {
            backup->AbortBackup();
            backup->Release();
            return result;
        }
    }

    // --- 8. Execute the snapshot ---
    {
        IVssAsync* async = nullptr;
        hr = backup->DoSnapshotSet(&async);
        if (FAILED(hr)) {
            backup->AbortBackup();
            backup->Release();
            return HResultToDword(hr);
        }
        result = WaitForAsync(async);
        if (result != ERROR_SUCCESS) {
            // After DoSnapshotSet failure, AbortBackup is not needed — the set
            // was never committed. Just release the instance.
            backup->Release();
            return result;
        }
    }

    // --- 9. Get snapshot properties ---
    VSS_SNAPSHOT_PROP prop{};
    hr = backup->GetSnapshotProperties(snapshotId, &prop);
    if (FAILED(hr)) {
        // Snapshot was created but we can't retrieve its path. Two
        // sub-cases, treated differently:
        //
        //   * Non-persistent: releasing the IVssBackupComponents below
        //     auto-deletes the snapshot via VSS_CTX_BACKUP semantics, so
        //     no extra cleanup is required.
        //   * Persistent (VSS_CTX_CLIENT_ACCESSIBLE): the snapshot
        //     survives backup->Release(). Without an explicit
        //     DeleteSnapshots here it would leak in VSS forever -- the
        //     caller never sees the ID (we are returning an error) and
        //     manual recovery requires `vssadmin list shadows` followed
        //     by `vssadmin delete shadows`. Best-effort: ignore the
        //     delete result so we do not mask the original
        //     GetSnapshotProperties HRESULT, which is what the caller
        //     needs to know about.
        if (persistent) {
            LONG deletedCount = 0;
            VSS_ID nonDeleted = GUID_NULL;
            backup->DeleteSnapshots(snapshotId, VSS_OBJECT_SNAPSHOT,
                                    FALSE, &deletedCount, &nonDeleted);
        }
        backup->Release();
        return HResultToDword(hr);
    }

    // --- 10. Extract and normalize device path (strip trailing backslash) ---
    std::wstring devicePath = StripTrailingBackslash(
        std::wstring(prop.m_pwszSnapshotDeviceObject));

    // --- 11. Free snapshot properties ---
    ::VssFreeSnapshotProperties(&prop);

    // --- 12. Build SnapshotInfo and store ---
    std::wstring idStr = GuidToString(snapshotId);

    HeldSnapshot held;
    held.info.id = idStr;
    held.info.vssId = snapshotId;
    held.info.volumePath = volumePath;
    held.info.devicePath = devicePath;
    held.info.persistent = persistent;
    held.info.createdAt = NowTimestamp();

    if (persistent) {
        // Persistent: release the instance immediately — snapshot lives
        // independently of the IVssBackupComponents lifetime.
        backup->Release();
    } else {
        // Non-persistent: hold the instance alive — releasing it
        // auto-deletes the snapshot.
        held.backup = VssBackupPtr(backup);
    }

    snapshots_[idStr] = std::move(held);

    // --- 13. Update manifest if available ---
    if (manifest_) {
        LayerMount::VHD::LayerEntry entry;
        entry.id = idStr;
        entry.type = LayerMount::VHD::LayerType::VSS;
        entry.path = devicePath;
        entry.mountStatus = L"mounted";
        entry.createdAt = snapshots_[idStr].info.createdAt;
        entry.metadata[L"volumePath"] = volumePath;
        entry.metadata[L"persistent"] = persistent ? L"true" : L"false";
        entry.metadata[L"vssId"] = GuidToString(snapshotId);
        manifest_->AddLayer(entry);
    }

    // --- 14. Set output parameters ---
    outSnapshotId = idStr;
    outDevicePath = devicePath;

    return ERROR_SUCCESS;
}

// ===========================================================================
// 5.5 — DeleteSnapshot
// ===========================================================================

DWORD VSSManager::DeleteSnapshot(const std::wstring& snapshotId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = snapshots_.find(snapshotId);

    // Resolve the raw VSS_ID: tracked entries carry it alongside SnapshotInfo;
    // untracked ids must parse the string form.
    VSS_ID vssId = GUID_NULL;
    if (it != snapshots_.end()) {
        vssId = it->second.info.vssId;
    } else if (!StringToGuid(snapshotId, vssId)) {
        return ERROR_INVALID_PARAMETER;
    }

    // Delete via a dedicated IVssBackupComponents under VSS_CTX_ALL. For
    // tracked non-persistent snapshots we cannot rely on the original
    // IVssBackupComponents::Release alone — observed behavior is that the
    // kernel shadow persists in Query results well past the Release, so an
    // explicit DeleteSnapshots makes the delete deterministic.
    IVssBackupComponents* bc = nullptr;
    HRESULT hr = ::CreateVssBackupComponents(&bc);
    if (FAILED(hr)) return HResultToDword(hr);

    hr = bc->InitializeForBackup();
    if (FAILED(hr)) {
        bc->Release();
        return HResultToDword(hr);
    }

    hr = bc->SetContext(VSS_CTX_ALL);
    if (FAILED(hr)) {
        bc->Release();
        return HResultToDword(hr);
    }

    LONG deletedCount = 0;
    VSS_ID nonDeletedId = GUID_NULL;
    hr = bc->DeleteSnapshots(vssId, VSS_OBJECT_SNAPSHOT,
                              TRUE, &deletedCount, &nonDeletedId);
    bc->Release();

    if (FAILED(hr)) return HResultToDword(hr);

    if (it != snapshots_.end()) {
        // Release our original IVssBackupComponents reference (for
        // non-persistent; persistent entries never held one).
        it->second.backup = VssBackupPtr();
        if (manifest_) manifest_->RemoveLayer(snapshotId);
        snapshots_.erase(it);
    }
    return ERROR_SUCCESS;
}

// ===========================================================================
// 5.5 — CleanupNonPersistent
// ===========================================================================

DWORD VSSManager::CleanupNonPersistent() {
    // Collect IDs under the lock, then delete each via the regular DeleteSnapshot
    // path (which takes the lock itself). DeleteSnapshot guarantees the VSS
    // shadow is gone via an explicit DeleteSnapshots call — releasing the held
    // IVssBackupComponents alone is not reliable.
    std::vector<std::wstring> toRemove;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, held] : snapshots_) {
            if (!held.info.persistent) toRemove.push_back(id);
        }
    }

    // Attempt every delete regardless of individual failures so one bad
    // snapshot does not strand the others, but surface the first error
    // code so callers (CLI + host shutdown) can tell the user cleanup
    // was partial instead of reporting a false success. Failed entries
    // remain tracked in snapshots_ so a subsequent Cleanup retries.
    DWORD firstError = ERROR_SUCCESS;
    for (const auto& id : toRemove) {
        DWORD dw = DeleteSnapshot(id);
        if (dw != ERROR_SUCCESS && firstError == ERROR_SUCCESS) {
            firstError = dw;
        }
    }
    return firstError;
}

// ===========================================================================
// ValidateSnapshotPath
// ===========================================================================

DWORD VSSManager::ValidateSnapshotPath(const std::wstring& snapshotId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = snapshots_.find(snapshotId);
    if (it == snapshots_.end()) return ERROR_NOT_FOUND;

    // GetFileAttributesW works with \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN
    DWORD attrs = ::GetFileAttributesW(it->second.info.devicePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return ::GetLastError();
    }

    return ERROR_SUCCESS;
}

// ===========================================================================
// 5.4 — Query methods
// ===========================================================================

DWORD VSSManager::ListSnapshots(std::vector<SnapshotInfo>& out) const {
    // Enumerate all system snapshots via IVssBackupComponents::Query under
    // VSS_CTX_ALL. Queries are read-only; no mutex needed against snapshots_.
    out.clear();

    IVssBackupComponents* bc = nullptr;
    HRESULT hr = ::CreateVssBackupComponents(&bc);
    if (FAILED(hr)) {
        return Win32FromHresult(hr);
    }

    hr = bc->InitializeForBackup();
    if (FAILED(hr)) {
        bc->Release();
        return Win32FromHresult(hr);
    }

    hr = bc->SetContext(VSS_CTX_ALL);
    if (FAILED(hr)) {
        bc->Release();
        return Win32FromHresult(hr);
    }

    IVssEnumObject* pEnum = nullptr;
    hr = bc->Query(GUID_NULL, VSS_OBJECT_NONE, VSS_OBJECT_SNAPSHOT, &pEnum);
    if (FAILED(hr)) {
        bc->Release();
        return Win32FromHresult(hr);
    }
    if (pEnum == nullptr) {
        // VSS returned success with a null enumerator: nothing to list,
        // but not a failure.
        bc->Release();
        return ERROR_SUCCESS;
    }

    // Enumerate explicitly and inspect the terminal HRESULT. Previously the
    // loop stopped on any non-S_OK value (including real errors after one
    // or more rows), and always returned ERROR_SUCCESS -- so a partial
    // snapshot list looked complete to the caller. Now: on S_FALSE / no-
    // more-items we're done; on FAILED() return the mapped Win32 error
    // with whatever rows already fetched left for diagnostics.
    HRESULT terminalHr = S_OK;
    for (;;) {
        VSS_OBJECT_PROP prop{};
        ULONG fetched = 0;
        HRESULT nextHr = pEnum->Next(1, &prop, &fetched);
        if (nextHr == S_FALSE || fetched == 0) {
            break;
        }
        if (FAILED(nextHr)) {
            terminalHr = nextHr;
            break;
        }
        VSS_SNAPSHOT_PROP& snap = prop.Obj.Snap;

        SnapshotInfo info;
        info.id    = GuidToString(snap.m_SnapshotId);
        info.vssId = snap.m_SnapshotId;
        info.volumePath = snap.m_pwszOriginalVolumeName
            ? std::wstring(snap.m_pwszOriginalVolumeName) : std::wstring();
        info.devicePath = StripTrailingBackslash(
            snap.m_pwszSnapshotDeviceObject
                ? std::wstring(snap.m_pwszSnapshotDeviceObject) : std::wstring());
        info.persistent = (snap.m_lSnapshotAttributes & VSS_VOLSNAP_ATTR_PERSISTENT) != 0;
        info.createdAt  = FormatVssTimestamp(snap.m_tsCreationTimestamp);

        ::VssFreeSnapshotProperties(&snap);
        out.push_back(std::move(info));
    }

    pEnum->Release();
    bc->Release();
    if (FAILED(terminalHr)) {
        return Win32FromHresult(terminalHr);
    }
    return ERROR_SUCCESS;
}

std::optional<SnapshotInfo> VSSManager::GetSnapshot(const std::wstring& id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = snapshots_.find(id);
    if (it == snapshots_.end()) return std::nullopt;
    return it->second.info;
}

} // namespace LayerMount::VSS
