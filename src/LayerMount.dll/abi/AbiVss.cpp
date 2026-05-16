// AbiVss.cpp -- VSS-primitive ABI entry points.
// LayerMountVssCreateSnapshot / DeleteSnapshot / ListSnapshots /
// CleanupSnapshots. Each shim opens a ComScope on entry so consumers do
// not need to CoInitializeEx the thread they call from; nested ComScopes
// on a thread already initialized by the DLL are cheap counter bumps
// (see ComScope.h).
//
// The VSS subsystem is lazy-constructed on the engine via
// LayerMount::Vss(); first use from any shim allocates a VSSManager that
// persists for the overlay's lifetime. Note that VSSManager's VSS COM
// interface pointers (VssBackupPtr inside HeldSnapshot) are ref-counted
// across ABI calls -- each shim only holds COM for the duration of its
// body, but the underlying VSS service keeps the snapshots alive.

#include "../public/LayerMount.h"
#include "AbiGuard.h"
#include "ErrorTls.h"
#include "HandleTable.h"
#include "HandleTypes.h"
#include "../impl/LayerMount.h"
#include "../impl/ComScope.h"
#include "../impl/vss/VSSManager.h"

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

// Emit a wide string into a caller-provided two-call buffer. NUL
// terminator is counted in the required size. Returns S_OK on successful
// copy, S_OK on sizing call (buffer == nullptr OR chars == 0),
// HRESULT_FROM_WIN32(ERROR_MORE_DATA) when the caller's buffer is
// present but too small.
inline HRESULT EmitWString(const std::wstring& src,
                           PWSTR  buffer,
                           SIZE_T bufferChars,
                           SIZE_T* bufferRequired) noexcept
{
    const SIZE_T requiredChars = src.size() + 1;
    if (bufferRequired != nullptr) {
        *bufferRequired = requiredChars;
    }
    if (buffer == nullptr || bufferChars == 0) {
        return S_OK;
    }
    if (bufferChars < requiredChars) {
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }
    std::memcpy(buffer, src.c_str(), requiredChars * sizeof(wchar_t));
    return S_OK;
}

// Parse VSS's ISO 8601 "YYYY-MM-DDTHH:MM:SSZ" back to FILETIME as UINT64.
// Returns 0 on parse failure so the public LM_VSS_SNAPSHOT_INFO.createdAt
// degrades gracefully rather than leaking uninitialized stack.
inline UINT64 ParseIso8601UtcToFiletime(const std::wstring& iso) noexcept
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (::swscanf_s(iso.c_str(),
                    L"%4d-%2d-%2dT%2d:%2d:%2dZ",
                    &year, &month, &day, &hour, &minute, &second) != 6) {
        return 0;
    }
    SYSTEMTIME st{};
    st.wYear         = static_cast<WORD>(year);
    st.wMonth        = static_cast<WORD>(month);
    st.wDay          = static_cast<WORD>(day);
    st.wHour         = static_cast<WORD>(hour);
    st.wMinute       = static_cast<WORD>(minute);
    st.wSecond       = static_cast<WORD>(second);
    st.wMilliseconds = 0;
    FILETIME ft{};
    if (!::SystemTimeToFileTime(&st, &ft)) {
        return 0;
    }
    return (static_cast<UINT64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Convert the VSS id string (GUID without braces) to a raw GUID. Accepts
// the 36-char "xxxxxxxx-xxxx-..." form VSSManager::GuidToString emits.
// Returns a zeroed GUID on parse failure -- the info struct still carries
// the stringform id so callers can detect the mismatch via
// non-matching string/GUID pairs.
inline GUID ParseVssIdToGuid(const std::wstring& id) noexcept
{
    // UuidFromStringW is rpcrt4 and has no COM-apartment preconditions
    // (unlike IIDFromString / CLSIDFromString). It takes a mutable
    // RPC_WSTR; we defensively copy so an accidentally-const src doesn't
    // UB via const_cast.
    std::wstring mutable_id = id;
    GUID g{};
    if (::UuidFromStringW(reinterpret_cast<RPC_WSTR>(mutable_id.data()),
                          &g) != RPC_S_OK) {
        return GUID{};
    }
    return g;
}

// Convert a populated VSSManager SnapshotInfo to a public
// LM_VSS_SNAPSHOT_INFO entry, honoring the two-call buffer shape for
// the per-string fields. String buffers that don't fit leave the
// corresponding Chars field at 0 -- callers detect overflow by
// comparing Chars vs Required on a per-entry basis.
inline void ToPublicSnapshotInfo(const ::LayerMount::VSS::SnapshotInfo& src,
                                 LM_VSS_SNAPSHOT_INFO& dst) noexcept
{
    // Fixed-shape fields.
    const SIZE_T idCapChars = sizeof(dst.id) / sizeof(dst.id[0]);
    const SIZE_T idNeeded   = src.id.size() + 1;
    if (idNeeded <= idCapChars) {
        std::memcpy(dst.id, src.id.c_str(), idNeeded * sizeof(wchar_t));
    } else {
        // GUID-without-braces is 36 chars + NUL = 37, always fits. Defend
        // against a future format change by truncating + NUL-terminating.
        std::memcpy(dst.id, src.id.c_str(), (idCapChars - 1) * sizeof(wchar_t));
        dst.id[idCapChars - 1] = L'\0';
    }
    dst.vssId       = ParseVssIdToGuid(src.id);
    dst.persistent  = src.persistent ? TRUE : FALSE;
    dst.createdAt   = ParseIso8601UtcToFiletime(src.createdAt);

    // Per-string two-call-pattern fields. Always populate Required; copy
    // into the caller's buffer only when it fits. Shorts set Chars = 0
    // (no partial writes).
    const SIZE_T volumeNeeded = src.volumePath.size() + 1;
    dst.volumePathRequired = volumeNeeded;
    if (dst.volumePath != nullptr && dst.volumePathChars >= volumeNeeded) {
        std::memcpy(dst.volumePath, src.volumePath.c_str(),
                    volumeNeeded * sizeof(wchar_t));
        dst.volumePathChars = volumeNeeded;
    } else {
        dst.volumePathChars = 0;
    }

    const SIZE_T devNeeded = src.devicePath.size() + 1;
    dst.devicePathRequired = devNeeded;
    if (dst.devicePath != nullptr && dst.devicePathChars >= devNeeded) {
        std::memcpy(dst.devicePath, src.devicePath.c_str(),
                    devNeeded * sizeof(wchar_t));
        dst.devicePathChars = devNeeded;
    } else {
        dst.devicePathChars = 0;
    }
}

// Map ComScope's HRESULT into a human-readable TLS message so callers
// running under a conflicting apartment (RPC_E_CHANGED_MODE) get more
// than a raw error code.
inline HRESULT GuardComScope(const ::LayerMount::ComScope& scope) noexcept
{
    if (scope.Ok()) return S_OK;
    const HRESULT hr = scope.InitResult();
    ::LayerMount::abi::ErrorTls::Set(
        hr,
        L"VSS shim: CoInitializeEx failed. The caller's thread is in a COM "
        L"apartment mode incompatible with MTA (likely RPC_E_CHANGED_MODE). "
        L"VSS operations require a thread that tolerates COINIT_MULTITHREADED.");
    return hr;
}

} // namespace

extern "C" {

LM_API HRESULT LM_CALL LayerMountVssCreateSnapshot(
    LM_HANDLE               mount,
    PCWSTR                   volumePath,
    BOOL                     persistent,
    LM_VSS_SNAPSHOT_HANDLE* outSnapshot,
    PWSTR                    idBuffer,
    SIZE_T                   idBufferChars,
    SIZE_T*                  idRequired,
    PWSTR                    devicePathBuffer,
    SIZE_T                   devicePathBufferChars,
    SIZE_T*                  devicePathRequired)
{
    using namespace ::LayerMount::abi;

    if (mount     == nullptr) return E_HANDLE;
    if (volumePath  == nullptr || *volumePath == L'\0') return E_INVALIDARG;
    if (outSnapshot == nullptr) return E_POINTER;

    // Always advertise the fixed required sizes so sizing probes can
    // discover them without provoking a snapshot.
    if (idRequired         != nullptr) *idRequired         = LM_VSS_ID_CHARS_REQUIRED;
    if (devicePathRequired != nullptr) *devicePathRequired = LM_VSS_DEVICE_PATH_CHARS_REQUIRED;

    // Sizing probe: caller passes no buffers and just wants the required
    // capacity. Side-effect-free (no VSS snapshot created).
    if (idBuffer == nullptr && idBufferChars == 0 &&
        devicePathBuffer == nullptr && devicePathBufferChars == 0)
    {
        return S_OK;
    }

    // Buffer-capacity pre-check. Refusing undersized buffers BEFORE
    // calling the VSS manager is the whole point of this ABI shape --
    // the old flow created the snapshot first and returned
    // ERROR_MORE_DATA afterward, so a caller that ran the idiomatic
    // sizing-then-fill loop created two snapshots for one request.
    if (idBuffer == nullptr || idBufferChars < LM_VSS_ID_CHARS_REQUIRED ||
        devicePathBuffer == nullptr || devicePathBufferChars < LM_VSS_DEVICE_PATH_CHARS_REQUIRED)
    {
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }

    LM_ABI_BEGIN();

    ::LayerMount::ComScope com;
    if (HRESULT hr = GuardComScope(com); FAILED(hr)) return hr;

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    auto& manager = mountHolder->core->Vss();

    std::wstring snapshotId;
    std::wstring devicePath;
    const DWORD dw = manager.CreateSnapshot(volumePath,
                                            persistent != FALSE,
                                            snapshotId,
                                            devicePath);
    if (dw != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(dw);
    }

    // Register the snapshot against an LM_VSS_SNAPSHOT_HANDLE so callers
    // can refer to it without round-tripping through the id string.
    auto info = std::make_unique<::LayerMount::VSS::SnapshotInfo>();
    info->id         = snapshotId;
    info->volumePath = volumePath;
    info->devicePath = devicePath;
    info->persistent = persistent != FALSE;
    // createdAt is not re-fetched here -- VSS doesn't hand it back from
    // Create; callers that need the timestamp use LayerMountVssListSnapshots.

    auto holder = std::make_unique<VssSnapshotHolder>();
    holder->manager = &manager;
    holder->info    = std::move(info);

    const std::uint64_t encodedSnap =
        Handles().vssSnapshot.Allocate(std::move(holder));
    if (encodedSnap == 0) {
        // Roll back the OS-side snapshot so a handle-table exhaustion
        // failure does not leak a shadow copy. Rollback is best-effort;
        // if VSS refuses the delete we still report the original OOM to
        // the caller.
        (void)manager.DeleteSnapshot(snapshotId);
        ErrorTls::Set(E_OUTOFMEMORY,
                      L"LayerMountVssCreateSnapshot: VSS handle table exhausted.");
        return E_OUTOFMEMORY;
    }
    *outSnapshot = reinterpret_cast<LM_VSS_SNAPSHOT_HANDLE>(
        static_cast<uintptr_t>(encodedSnap));

    // Buffer sizes were validated up front; the copy is guaranteed to
    // succeed. EmitWString still populates *Required for callers.
    if (HRESULT hrId = EmitWString(snapshotId, idBuffer,
                                   idBufferChars, idRequired);
        FAILED(hrId))
    {
        return hrId;
    }
    if (HRESULT hrDev = EmitWString(devicePath, devicePathBuffer,
                                    devicePathBufferChars, devicePathRequired);
        FAILED(hrDev))
    {
        return hrDev;
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVssCloseSnapshot(LM_VSS_SNAPSHOT_HANDLE snapshot)
{
    using namespace ::LayerMount::abi;

    if (snapshot == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(snapshot));
    auto freed = Handles().vssSnapshot.Free(encoded);
    if (!freed) {
        return E_HANDLE;
    }
    // freed drops the holder's reference. The underlying VSS snapshot
    // lifecycle is owned by VSSManager (non-persistent -> cleanup
    // deletes, persistent -> caller / backup admin deletes). This close
    // only retires the receipt handle.
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVssDeleteSnapshot(LM_HANDLE mount,
                                                  PCWSTR     snapshotId)
{
    using namespace ::LayerMount::abi;

    if (mount    == nullptr) return E_HANDLE;
    if (snapshotId == nullptr || *snapshotId == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    ::LayerMount::ComScope com;
    if (HRESULT hr = GuardComScope(com); FAILED(hr)) return hr;

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    const DWORD dw = mountHolder->core->Vss().DeleteSnapshot(snapshotId);
    if (dw != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(dw);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVssListSnapshots(LM_HANDLE             mount,
                                                 LM_VSS_SNAPSHOT_INFO* entries,
                                                 UINT32                 entriesCapacity,
                                                 UINT32*                entriesWritten,
                                                 UINT32*                entriesRequired)
{
    using namespace ::LayerMount::abi;

    if (mount == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    ::LayerMount::ComScope com;
    if (HRESULT hr = GuardComScope(com); FAILED(hr)) return hr;

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    std::vector<::LayerMount::VSS::SnapshotInfo> snapshots;
    {
        const DWORD dw = mountHolder->core->Vss().ListSnapshots(snapshots);
        if (dw != ERROR_SUCCESS) {
            return HRESULT_FROM_WIN32(dw);
        }
    }

    if (entriesRequired != nullptr) {
        *entriesRequired = static_cast<UINT32>(snapshots.size());
    }
    if (entriesWritten != nullptr) {
        *entriesWritten = 0;
    }

    // Sizing call: caller wants the entry count without entry fills.
    if (entries == nullptr || entriesCapacity == 0) {
        return S_OK;
    }

    // Short entries array: report required count, do not populate. The
    // per-entry string buffers the caller pre-supplied are untouched.
    if (entriesCapacity < snapshots.size()) {
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }

    for (UINT32 i = 0; i < static_cast<UINT32>(snapshots.size()); ++i) {
        ToPublicSnapshotInfo(snapshots[i], entries[i]);
    }
    if (entriesWritten != nullptr) {
        *entriesWritten = static_cast<UINT32>(snapshots.size());
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVssCleanupSnapshots(LM_HANDLE mount)
{
    using namespace ::LayerMount::abi;

    if (mount == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    ::LayerMount::ComScope com;
    if (HRESULT hr = GuardComScope(com); FAILED(hr)) return hr;

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    const DWORD dw = mountHolder->core->Vss().CleanupNonPersistent();
    if (dw != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(dw);
    }
    return S_OK;

    LM_ABI_END();
}

} // extern "C"
