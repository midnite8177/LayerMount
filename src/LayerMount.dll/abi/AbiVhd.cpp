// AbiVhd.cpp -- VHD-primitive ABI entry points (FR-23, FR-24).
// LayerMountVhdCreate / Open / Attach / Detach / Merge / Import / Export /
// Close. Every shim is a thin translator on top of
// impl/vhd/VHDLayerManager::{CreateVHD, CreateDifferencingVHD, AttachVHD,
// DetachVHD, MergeVHD, ImportDirectory, ExportToDirectory}.
//
// Sequencing contract:
//   Create / Open:  validates inputs and parks the path + attach config
//                   in the holder. The VHD file is NOT pre-opened -- the
//                   backing OpenVHD uses VIRTUAL_DISK_ACCESS_ALL which
//                   would conflict with a subsequent AttachVHD (also an
//                   OpenVirtualDisk) on the same path.
//   Attach:         calls AttachVHD(path, ...), caches the returned
//                   VhdHandle + physical path on the holder. Supports
//                   the two-call buffer pattern -- a short-buffer
//                   follow-up call does NOT re-attach; it just re-emits
//                   the cached path.
//   Detach:         calls DetachVHD(path), releases the cached
//                   VhdHandle, clears the physical path cache.
//   Merge:          calls MergeVHD(path). Fails at the Win32 layer if
//                   the VHD is currently attached -- the error bubbles
//                   up unchanged.
//   Import / Export: overlay-scoped (no VHD handle); dispatch through
//                   the overlay's lazy VHDLayerManager accessor.
//   Close:          frees the holder slot. For ProcessScoped attaches
//                   this releases the OS-side attach automatically when
//                   the cached VhdHandle destructs. Callers who used
//                   Permanent lifetime are responsible for Detach if
//                   they want the VHD offline.

#include "../public/LayerMount.h"
#include "AbiGuard.h"
#include "ErrorTls.h"
#include "HandleTable.h"
#include "HandleTypes.h"
#include "../impl/LayerMount.h"
#include "../impl/vhd/VHDLayerManager.h"
#include "../impl/vhd/Manifest.h"
#include "../impl/vhd/VolumeGuid.h"

#include "nlohmann/json.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

// Every VHDLayerManager entry point returns a Win32 DWORD. ERROR_SUCCESS
// is the only success code; everything else is mapped into the Win32
// HRESULT facility.
inline HRESULT HresultFromWin32Dword(DWORD code) noexcept {
    return code == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(code);
}

inline ::LayerMount::VHD::AttachLifetime ToManagerLifetime(
    LM_VHD_ATTACH_LIFETIME lt) noexcept
{
    return (lt == LM_VHD_ATTACH_PROCESS_SCOPED)
        ? ::LayerMount::VHD::AttachLifetime::ProcessScoped
        : ::LayerMount::VHD::AttachLifetime::Permanent;
}

// Two-call buffer emit for LayerMountVhdAttach's physical-path output. NUL
// terminator is counted in the required size, matching the rest of the
// ABI's string contracts.
inline HRESULT EmitPhysicalPath(const std::wstring& path,
                                PWSTR buffer,
                                SIZE_T bufferChars,
                                SIZE_T* bufferRequired) noexcept
{
    const SIZE_T requiredChars = path.size() + 1;
    if (bufferRequired != nullptr) {
        *bufferRequired = requiredChars;
    }
    if (buffer == nullptr || bufferChars == 0) {
        return S_OK;
    }
    if (bufferChars < requiredChars) {
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }
    std::memcpy(buffer, path.c_str(), requiredChars * sizeof(wchar_t));
    return S_OK;
}

} // namespace

extern "C" {

LM_API HRESULT LM_CALL LayerMountVhdCreate(LM_HANDLE            mount,
                                          const LM_VHD_CONFIG* config,
                                          LM_VHD_HANDLE*       outVhd)
{
    using namespace ::LayerMount::abi;

    if (mount == nullptr) return E_HANDLE;
    if (config  == nullptr) return E_POINTER;
    if (outVhd  == nullptr) return E_POINTER;
    if (config->structSize < sizeof(LM_VHD_CONFIG)) return E_INVALIDARG;
    if (config->path == nullptr || *config->path == L'\0') return E_INVALIDARG;
    if (config->kind == LM_VHD_KIND_DIFFERENCING &&
        (config->parentPath == nullptr || *config->parentPath == L'\0'))
    {
        return E_INVALIDARG;
    }
    if ((config->kind == LM_VHD_KIND_FIXED || config->kind == LM_VHD_KIND_DYNAMIC) &&
        config->sizeBytes == 0)
    {
        return E_INVALIDARG;
    }

    LM_ABI_BEGIN();

    const std::uint64_t encodedLayerMount =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encodedLayerMount);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    auto& manager = mountHolder->core->Vhd();

    // Backing call returns an open VhdHandle we immediately discard --
    // see the file-header note about VIRTUAL_DISK_ACCESS_ALL conflicting
    // with a subsequent Attach on the same path.
    ::LayerMount::VHD::VhdHandle scratch;
    DWORD dw = ERROR_SUCCESS;
    switch (config->kind) {
        case LM_VHD_KIND_FIXED:
            dw = manager.CreateVHD(config->path, config->sizeBytes,
                                   /*dynamic*/ false, scratch);
            break;
        case LM_VHD_KIND_DYNAMIC:
            dw = manager.CreateVHD(config->path, config->sizeBytes,
                                   /*dynamic*/ true, scratch);
            break;
        case LM_VHD_KIND_DIFFERENCING:
            dw = manager.CreateDifferencingVHD(config->path,
                                               config->parentPath,
                                               scratch);
            break;
        default:
            return E_INVALIDARG;
    }
    if (dw != ERROR_SUCCESS) {
        return HresultFromWin32Dword(dw);
    }
    scratch.Close();

    auto holder = std::make_unique<VhdHolder>();
    holder->manager             = &manager;
    holder->path                = config->path;
    holder->readOnly            = config->readOnly;
    holder->suppressDriveLetter = config->suppressDriveLetter;
    holder->lifetime            = config->lifetime;

    const std::uint64_t encodedVhd = Handles().vhd.Allocate(std::move(holder));
    if (encodedVhd == 0) {
        ErrorTls::Set(E_OUTOFMEMORY, L"LayerMountVhdCreate: VHD handle table exhausted.");
        return E_OUTOFMEMORY;
    }

    *outVhd = reinterpret_cast<LM_VHD_HANDLE>(static_cast<uintptr_t>(encodedVhd));
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdOpen(LM_HANDLE            mount,
                                        const LM_VHD_CONFIG* config,
                                        LM_VHD_HANDLE*       outVhd)
{
    using namespace ::LayerMount::abi;

    if (mount == nullptr) return E_HANDLE;
    if (config  == nullptr) return E_POINTER;
    if (outVhd  == nullptr) return E_POINTER;
    if (config->structSize < sizeof(LM_VHD_CONFIG)) return E_INVALIDARG;
    if (config->path == nullptr || *config->path == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encodedLayerMount =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encodedLayerMount);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    // Cheap path validation -- a full OpenVirtualDisk here would grab an
    // exclusive handle that conflicts with a follow-up Attach.
    const DWORD attrs = ::GetFileAttributesW(config->path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    }

    auto& manager = mountHolder->core->Vhd();

    auto holder = std::make_unique<VhdHolder>();
    holder->manager             = &manager;
    holder->path                = config->path;
    holder->readOnly            = config->readOnly;
    holder->suppressDriveLetter = config->suppressDriveLetter;
    holder->lifetime            = config->lifetime;

    const std::uint64_t encodedVhd = Handles().vhd.Allocate(std::move(holder));
    if (encodedVhd == 0) {
        ErrorTls::Set(E_OUTOFMEMORY, L"LayerMountVhdOpen: VHD handle table exhausted.");
        return E_OUTOFMEMORY;
    }

    *outVhd = reinterpret_cast<LM_VHD_HANDLE>(static_cast<uintptr_t>(encodedVhd));
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdAttach(LM_VHD_HANDLE vhd,
                                          PWSTR          physicalPathBuffer,
                                          SIZE_T         physicalPathChars,
                                          SIZE_T*        physicalPathRequired)
{
    using namespace ::LayerMount::abi;

    if (vhd == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(vhd));
    auto holder = Handles().vhd.Resolve(encoded);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    // Serialize state transitions on this handle so Attach / Detach /
    // GetVolumeGuid can't observe `open` and `attachedPhysicalPath`
    // mid-mutation.
    std::lock_guard<std::mutex> stateLock(holder->stateMutex);

    // Idempotent-style two-call pattern: if we've already attached, skip
    // the backing call and just re-emit the cached path. This lets
    // callers size-probe + fill without AttachVHD refusing the second
    // invocation (it's not reentrancy-safe on the same path).
    if (!holder->attachedPhysicalPath.empty()) {
        return EmitPhysicalPath(holder->attachedPhysicalPath,
                                physicalPathBuffer,
                                physicalPathChars,
                                physicalPathRequired);
    }

    auto attachedHandle = std::make_unique<::LayerMount::VHD::VhdHandle>();
    std::wstring physicalPath;
    const DWORD dw = holder->manager->AttachVHD(
        holder->path,
        holder->readOnly != FALSE,
        *attachedHandle,
        physicalPath,
        ToManagerLifetime(holder->lifetime),
        holder->suppressDriveLetter != FALSE);
    if (dw != ERROR_SUCCESS) {
        return HresultFromWin32Dword(dw);
    }

    // Keep the attach alive for ProcessScoped lifetime (OS releases on
    // last-handle-close); Permanent doesn't need the handle but holding
    // it anyway keeps the Detach path uniform.
    holder->open                 = std::move(attachedHandle);
    holder->attachedPhysicalPath = std::move(physicalPath);

    return EmitPhysicalPath(holder->attachedPhysicalPath,
                            physicalPathBuffer,
                            physicalPathChars,
                            physicalPathRequired);

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdDetach(LM_VHD_HANDLE vhd)
{
    using namespace ::LayerMount::abi;

    if (vhd == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(vhd));
    auto holder = Handles().vhd.Resolve(encoded);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    std::lock_guard<std::mutex> stateLock(holder->stateMutex);

    const DWORD dw = holder->manager->DetachVHD(holder->path);
    if (dw != ERROR_SUCCESS) {
        return HresultFromWin32Dword(dw);
    }

    // Order matters: drop the handle first (closes any ProcessScoped OS
    // attach that is still held), then clear the cached path so a later
    // Attach can populate fresh state.
    holder->open.reset();
    holder->attachedPhysicalPath.clear();
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdMerge(LM_VHD_HANDLE childVhd)
{
    using namespace ::LayerMount::abi;

    if (childVhd == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(childVhd));
    auto holder = Handles().vhd.Resolve(encoded);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    return HresultFromWin32Dword(holder->manager->MergeVHD(holder->path));

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdImport(LM_HANDLE mount,
                                          PCWSTR     directoryPath,
                                          PCWSTR     vhdPath,
                                          UINT64     sizeBytes)
{
    using namespace ::LayerMount::abi;

    if (mount       == nullptr) return E_HANDLE;
    if (directoryPath == nullptr || *directoryPath == L'\0') return E_INVALIDARG;
    if (vhdPath       == nullptr || *vhdPath       == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    // sizeBytes == 0 is the documented auto-size sentinel; pass through
    // unchanged.
    return HresultFromWin32Dword(
        mountHolder->core->Vhd().ImportDirectory(
            directoryPath, vhdPath, static_cast<ULONGLONG>(sizeBytes)));

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdExport(LM_HANDLE mount,
                                          PCWSTR     vhdPath,
                                          PCWSTR     directoryPath)
{
    using namespace ::LayerMount::abi;

    if (mount       == nullptr) return E_HANDLE;
    if (vhdPath       == nullptr || *vhdPath       == L'\0') return E_INVALIDARG;
    if (directoryPath == nullptr || *directoryPath == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    return HresultFromWin32Dword(
        mountHolder->core->Vhd().ExportToDirectory(vhdPath, directoryPath));

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdClose(LM_VHD_HANDLE vhd)
{
    using namespace ::LayerMount::abi;

    if (vhd == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(vhd));
    auto freed = Handles().vhd.Free(encoded);
    if (!freed) {
        return E_HANDLE;
    }
    // VhdHolder destructor releases the cached VhdHandle. Per the
    // AttachLifetime contract, that closes the OS-side attach for
    // ProcessScoped and is a no-op for Permanent (the attach outlives
    // the handle).
    return S_OK;

    LM_ABI_END();
}

// ---------------------------------------------------------------------------
// VHD volume-GUID + manifest primitives (FR-35).
// ---------------------------------------------------------------------------

LM_API HRESULT LM_CALL LayerMountVhdGetVolumeGuid(LM_VHD_HANDLE vhd,
                                                 PWSTR          buffer,
                                                 SIZE_T         bufferChars,
                                                 SIZE_T*        requiredChars)
{
    using namespace ::LayerMount::abi;

    if (vhd == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(vhd));
    auto holder = Handles().vhd.Resolve(encoded);
    if (holder == nullptr) return E_HANDLE;

    // Guard `open` against concurrent mutation by Attach/Detach.
    std::lock_guard<std::mutex> stateLock(holder->stateMutex);

    if (!holder->open || holder->open->Get() == INVALID_HANDLE_VALUE) {
        ErrorTls::Set(E_ILLEGAL_METHOD_CALL,
            L"LayerMountVhdGetVolumeGuid: must be called after LayerMountVhdAttach.");
        return E_ILLEGAL_METHOD_CALL;
    }

    std::wstring guid;
    DWORD err = ::LayerMount::VHD::GetVolumeGuidForVHD(holder->open->Get(), guid);
    if (err != ERROR_SUCCESS) {
        // Return the Win32 error wrapped as HRESULT so the caller can
        // distinguish ERROR_GEN_FAILURE (PnP not settled yet -> retry)
        // from a terminal failure.
        return HRESULT_FROM_WIN32(err);
    }

    const SIZE_T need = guid.size() + 1;
    if (requiredChars != nullptr) *requiredChars = need;
    if (buffer == nullptr || bufferChars == 0) return S_OK;
    if (bufferChars < need) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    std::memcpy(buffer, guid.c_str(), need * sizeof(wchar_t));
    return S_OK;

    LM_ABI_END();
}

} // extern "C"

namespace {

// Resolve the manifest path from an optional manifestDir. NULL / empty
// -> process cwd. The Manifest::DefaultPath helper supplies the canonical
// filename.
std::wstring ResolveManifestPath(PCWSTR manifestDir) {
    std::wstring dir;
    if (manifestDir != nullptr && *manifestDir != L'\0') {
        dir = manifestDir;
    } else {
        dir = std::filesystem::current_path().wstring();
    }
    return ::LayerMount::VHD::Manifest::DefaultPath(dir);
}

// Emit a wstring via the two-call buffer pattern used by LM_VHD_LAYER_INFO
// per-string fields. Returns TRUE iff the caller's buffer was populated
// (false if sizing-only or short). *requiredOut always receives the
// NUL-counted required size.
bool EmitLayerInfoString(const std::wstring& value,
                         PWSTR buffer, SIZE_T bufferChars,
                         SIZE_T* charsOut, SIZE_T* requiredOut) {
    const SIZE_T need = value.size() + 1;
    if (requiredOut != nullptr) *requiredOut = need;
    if (charsOut    != nullptr) *charsOut    = 0;
    if (buffer == nullptr || bufferChars < need) return false;
    std::memcpy(buffer, value.c_str(), need * sizeof(wchar_t));
    if (charsOut != nullptr) *charsOut = value.size();
    return true;
}

LM_VHD_LAYER_TYPE ToPublicLayerType(::LayerMount::VHD::LayerType t) {
    switch (t) {
        case ::LayerMount::VHD::LayerType::VHD:       return LM_VHD_LAYER_VHD;
        case ::LayerMount::VHD::LayerType::VSS:       return LM_VHD_LAYER_VSS;
        case ::LayerMount::VHD::LayerType::Directory:
        default:                                      return LM_VHD_LAYER_DIRECTORY;
    }
}

} // namespace

extern "C" {

LM_API HRESULT LM_CALL LayerMountVhdListLayers(LM_HANDLE          mount,
                                               PCWSTR              manifestDir,
                                               LM_VHD_LAYER_INFO* entries,
                                               UINT32              entriesCapacity,
                                               UINT32*             entriesWritten,
                                               UINT32*             entriesRequired)
{
    using namespace ::LayerMount::abi;

    if (mount == nullptr) return E_HANDLE;
    if (entriesRequired == nullptr) return E_POINTER;

    if (entriesWritten != nullptr) *entriesWritten = 0;
    *entriesRequired = 0;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) return E_HANDLE;

    const std::wstring manifestPath = ResolveManifestPath(manifestDir);

    ::LayerMount::VHD::Manifest m;
    DWORD loadErr = m.Load(manifestPath);
    if (loadErr == ERROR_FILE_NOT_FOUND) {
        // Idempotent: no manifest on disk -> zero entries, S_OK.
        return S_OK;
    }
    if (loadErr != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(loadErr);
    }

    auto layers = m.ListLayers();
    *entriesRequired = static_cast<UINT32>(layers.size());

    if (entries == nullptr || entriesCapacity == 0) {
        return S_OK;  // sizing call
    }
    if (entriesCapacity < layers.size()) {
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }

    for (size_t i = 0; i < layers.size(); ++i) {
        const auto* src = layers[i];
        LM_VHD_LAYER_INFO& dst = entries[i];

        // id: fixed WCHAR[64], NUL-terminated. Truncate long ids safely.
        constexpr SIZE_T kIdCap = sizeof(dst.id) / sizeof(WCHAR);
        const SIZE_T idLen = src->id.size() < (kIdCap - 1)
            ? src->id.size() : (kIdCap - 1);
        std::memcpy(dst.id, src->id.c_str(), idLen * sizeof(WCHAR));
        dst.id[idLen] = L'\0';

        dst.type = ToPublicLayerType(src->type);

        EmitLayerInfoString(src->path,        dst.path,
                            dst.pathChars,    &dst.pathChars,
                            &dst.pathRequired);
        EmitLayerInfoString(src->parentId,    dst.parentId,
                            dst.parentIdChars, &dst.parentIdChars,
                            &dst.parentIdRequired);
        EmitLayerInfoString(src->mountStatus, dst.mountStatus,
                            dst.mountStatusChars, &dst.mountStatusChars,
                            &dst.mountStatusRequired);
        EmitLayerInfoString(src->volumeGuid,  dst.volumeGuid,
                            dst.volumeGuidChars, &dst.volumeGuidChars,
                            &dst.volumeGuidRequired);
        EmitLayerInfoString(src->createdAt,   dst.createdAt,
                            dst.createdAtChars, &dst.createdAtChars,
                            &dst.createdAtRequired);
    }
    if (entriesWritten != nullptr) {
        *entriesWritten = static_cast<UINT32>(layers.size());
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdUnregisterLayer(LM_HANDLE mount,
                                                    PCWSTR     layerId,
                                                    PCWSTR     manifestDir,
                                                    BOOL*      outRemoved)
{
    using namespace ::LayerMount::abi;

    if (mount  == nullptr) return E_HANDLE;
    if (layerId  == nullptr || *layerId == L'\0') return E_INVALIDARG;
    if (outRemoved == nullptr) return E_POINTER;

    *outRemoved = FALSE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) return E_HANDLE;

    const std::wstring manifestPath = ResolveManifestPath(manifestDir);

    ::LayerMount::VHD::ManifestLock lock(manifestPath);
    if (!lock.Held()) {
        // Another process owns the manifest lock or creation failed;
        // loading + saving without the lock would race their update.
        ErrorTls::Set(HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION),
            L"LayerMountVhdUnregisterLayer: could not acquire the cross-process "
            L"manifest lock. Another VHD tool may be operating on the same "
            L"working directory. Retry after it releases.");
        return HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION);
    }
    ::LayerMount::VHD::Manifest m;
    DWORD loadErr = m.Load(manifestPath);
    if (loadErr == ERROR_FILE_NOT_FOUND) {
        // Idempotent: missing manifest == nothing to remove.
        return S_OK;
    }
    if (loadErr != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(loadErr);
    }

    bool removed = m.RemoveLayer(layerId);
    if (removed) {
        DWORD saveErr = m.Save(manifestPath);
        if (saveErr != ERROR_SUCCESS) {
            return HRESULT_FROM_WIN32(saveErr);
        }
    }
    *outRemoved = removed ? TRUE : FALSE;
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdGetLayerMetadataJson(LM_HANDLE mount,
                                                        PCWSTR     manifestDir,
                                                        PCWSTR     layerId,
                                                        PWSTR      buffer,
                                                        SIZE_T     bufferChars,
                                                        SIZE_T*    requiredChars)
{
    using namespace ::LayerMount::abi;

    if (mount  == nullptr) return E_HANDLE;
    if (layerId  == nullptr || *layerId == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) return E_HANDLE;

    const std::wstring manifestPath = ResolveManifestPath(manifestDir);

    ::LayerMount::VHD::Manifest m;
    DWORD loadErr = m.Load(manifestPath);
    if (loadErr == ERROR_FILE_NOT_FOUND) {
        return STG_E_PATHNOTFOUND;
    }
    if (loadErr != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(loadErr);
    }

    const auto* entry = m.GetLayer(layerId);
    if (entry == nullptr) {
        return STG_E_PATHNOTFOUND;
    }

    // Serialize the metadata map to JSON. Use nlohmann::json with UTF-8
    // internally; convert back to wide for the output buffer.
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : entry->metadata) {
        // wstring -> utf8.
        auto wtou8 = [](const std::wstring& w) {
            if (w.empty()) return std::string();
            int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                        static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                static_cast<int>(w.size()),
                                s.data(), n, nullptr, nullptr);
            return s;
        };
        j[wtou8(k)] = wtou8(v);
    }

    const std::string u8 = j.dump();
    // utf8 -> wide.
    int wideCount = MultiByteToWideChar(CP_UTF8, 0, u8.data(),
                                        static_cast<int>(u8.size()),
                                        nullptr, 0);
    std::wstring out(static_cast<size_t>(wideCount), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(),
                        static_cast<int>(u8.size()),
                        out.data(), wideCount);

    const SIZE_T need = out.size() + 1;
    if (requiredChars != nullptr) *requiredChars = need;
    if (buffer == nullptr || bufferChars == 0) return S_OK;
    if (bufferChars < need) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    std::memcpy(buffer, out.c_str(), need * sizeof(wchar_t));
    return S_OK;

    LM_ABI_END();
}

} // extern "C"
