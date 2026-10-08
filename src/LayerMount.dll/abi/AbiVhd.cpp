// AbiVhd.cpp -- VHD-primitive ABI entry points.
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
//   Detach:         detaches through the held VhdHandle, or by path
//                   when the holder has no attach; then releases the
//                   handle and clears the cached physical path.
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
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

// Every VHDLayerManager entry point reports a Win32 DWORD. ERROR_SUCCESS
// is the only success code; everything else is mapped into the Win32
// HRESULT facility.
inline HRESULT HresultFromWin32Dword(DWORD code) noexcept {
    return code == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(code);
}

std::wstring ExportFailureMessage(const ::LayerMount::VHD::ExportResult& result) {
    using ::LayerMount::VHD::ExportStep;
    std::wstring what;
    if (result.step == ExportStep::Copy) {
        what = L"copy '" + result.entry + L"' from the VHD";
    } else if (result.entry == L"\\") {
        what = L"list the root of the VHD volume";
    } else {
        what = L"list '" + result.entry + L"' in the VHD";
    }
    return L"LayerMountVhdExport could not " + what + L". The destination is incomplete.";
}

inline ::LayerMount::VHD::AttachLifetime ToManagerLifetime(
    LM_VHD_ATTACH_LIFETIME lt) noexcept
{
    return (lt == LM_VHD_ATTACH_PROCESS_SCOPED)
        ? ::LayerMount::VHD::AttachLifetime::ProcessScoped
        : ::LayerMount::VHD::AttachLifetime::Permanent;
}

inline ::LayerMount::VHD::AttachOptions ToAttachOptions(const LM_VHD_CONFIG& config) noexcept
{
    return {
        config.readOnly != FALSE ? ::LayerMount::VHD::AttachAccess::ReadOnly
                                 : ::LayerMount::VHD::AttachAccess::ReadWrite,
        ToManagerLifetime(config.lifetime),
        config.suppressDriveLetter != FALSE ? ::LayerMount::VHD::DriveLetter::Suppress
                                            : ::LayerMount::VHD::DriveLetter::Assign,
    };
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

}

extern "C" {

LM_API HRESULT LM_CALL LayerMountVhdCreate(LM_HANDLE            mount,
                                          const LM_VHD_CONFIG* config,
                                          LM_VHD_HANDLE*       outVhd)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (mount == nullptr) return E_HANDLE;
    if (config  == nullptr) return E_POINTER;
    if (outVhd  == nullptr) return E_POINTER;
    if (!LM_STRUCT_SIZE_COVERS(config, LM_VHD_CONFIG, _reserved0)) return E_INVALIDARG;
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

    const ::LayerMount::HostPath vhdPath(config->path);
    DWORD dw = ERROR_SUCCESS;
    switch (config->kind) {
        case LM_VHD_KIND_FIXED:
            dw = manager.CreateVHD(vhdPath, config->sizeBytes,
                                   ::LayerMount::VHD::VhdAllocation::Fixed);
            break;
        case LM_VHD_KIND_DYNAMIC:
            dw = manager.CreateVHD(vhdPath, config->sizeBytes,
                                   ::LayerMount::VHD::VhdAllocation::Dynamic);
            break;
        case LM_VHD_KIND_DIFFERENCING:
            dw = manager.CreateDifferencingVHD(vhdPath,
                                               ::LayerMount::HostPath(config->parentPath));
            break;
        default:
            return E_INVALIDARG;
    }
    if (dw != ERROR_SUCCESS) {
        return HresultFromWin32Dword(dw);
    }

    auto holder = std::make_unique<VhdHolder>();
    holder->manager       = &manager;
    holder->path          = ::LayerMount::HostPath(config->path);
    holder->attachOptions = ToAttachOptions(*config);

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
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (mount == nullptr) return E_HANDLE;
    if (config  == nullptr) return E_POINTER;
    if (outVhd  == nullptr) return E_POINTER;
    if (!LM_STRUCT_SIZE_COVERS(config, LM_VHD_CONFIG, _reserved0)) return E_INVALIDARG;
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
    const DWORD attrs =
        ::GetFileAttributesW(::LayerMount::HostPath(config->path).ForWin32().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return HRESULT_FROM_WIN32(::GetLastError());
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        return HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
    }

    auto& manager = mountHolder->core->Vhd();

    auto holder = std::make_unique<VhdHolder>();
    holder->manager       = &manager;
    holder->path          = ::LayerMount::HostPath(config->path);
    holder->attachOptions = ToAttachOptions(*config);

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
    LM_ABI_ENTRY();
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

    ::LayerMount::VHD::AttachedVhd attached;
    const DWORD dw = holder->manager->AttachVHD(holder->path, holder->attachOptions, attached);
    if (dw != ERROR_SUCCESS) {
        return HresultFromWin32Dword(dw);
    }

    // Closing this handle ends a ProcessScoped attach at once, and
    // LayerMountVhdDetach needs it to detach.
    holder->open = std::make_unique<::LayerMount::VHD::VhdHandle>(std::move(attached.handle));
    holder->attachedPhysicalPath = std::move(attached.physicalPath);

    return EmitPhysicalPath(holder->attachedPhysicalPath,
                            physicalPathBuffer,
                            physicalPathChars,
                            physicalPathRequired);

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdDetach(LM_VHD_HANDLE vhd)
{
    LM_ABI_ENTRY();
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

    // A second open by path fails while this holder holds an attach.
    // With no attach, detach by path takes an earlier process's Permanent attach offline.
    const DWORD dw = holder->open
        ? holder->manager->DetachVHD(*holder->open)
        : holder->manager->DetachVHD(holder->path);
    // The held handle gets ERROR_NOT_READY once another route detached the disk.
    const bool alreadyDetached = holder->open && dw == ERROR_NOT_READY;
    if (dw != ERROR_SUCCESS && !alreadyDetached) {
        return HresultFromWin32Dword(dw);
    }

    holder->open.reset();
    holder->attachedPhysicalPath.clear();
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdMerge(LM_VHD_HANDLE childVhd)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (childVhd == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(childVhd));
    auto holder = Handles().vhd.Resolve(encoded);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    return HresultFromWin32Dword(
        holder->manager->MergeVHD(holder->path));

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdImport(LM_HANDLE mount,
                                          PCWSTR     directoryPath,
                                          PCWSTR     vhdPath,
                                          UINT64     sizeBytes)
{
    LM_ABI_ENTRY();
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

    const std::optional<ULONGLONG> capacity =
        sizeBytes == 0 ? std::nullopt : std::optional<ULONGLONG>(sizeBytes);
    return HresultFromWin32Dword(
        mountHolder->core->Vhd().ImportDirectory(
            ::LayerMount::HostPath(directoryPath), ::LayerMount::HostPath(vhdPath),
            capacity));

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdExport(LM_HANDLE mount,
                                          PCWSTR     vhdPath,
                                          PCWSTR     directoryPath)
{
    LM_ABI_ENTRY();
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

    const ::LayerMount::VHD::ExportResult result =
        mountHolder->core->Vhd().ExportToDirectory(
            ::LayerMount::HostPath(vhdPath), ::LayerMount::HostPath(directoryPath));
    const HRESULT hr = HresultFromWin32Dword(result.error);
    if (!result.entry.empty()) {
        ErrorTls::Set(hr, ExportFailureMessage(result).c_str());
    }
    return hr;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountVhdClose(LM_VHD_HANDLE vhd)
{
    LM_ABI_ENTRY();
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

LM_API HRESULT LM_CALL LayerMountVhdGetVolumeGuid(LM_VHD_HANDLE vhd,
                                                 PWSTR          buffer,
                                                 SIZE_T         bufferChars,
                                                 SIZE_T*        requiredChars)
{
    LM_ABI_ENTRY();
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

}

namespace {

::LayerMount::HostPath ResolveManifestPath(PCWSTR manifestDir) {
    std::wstring dir;
    if (manifestDir != nullptr && *manifestDir != L'\0') {
        dir = manifestDir;
    } else {
        dir = std::filesystem::current_path().wstring();
    }
    return ::LayerMount::VHD::Manifest::DefaultPath(::LayerMount::HostPath(dir));
}

constexpr DWORD kRegistryLockTimeoutMs = 30000;

HRESULT RegistryLockFailure(PCWSTR function) {
    const HRESULT hr = HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION);
    std::wstring message = function;
    message += L": could not get the cross-process layer registry lock within ";
    message += std::to_wstring(kRegistryLockTimeoutMs / 1000);
    message += L" seconds. Another VHD tool may be using the same registry. "
               L"Retry after it releases.";
    ::LayerMount::abi::ErrorTls::Set(hr, message.c_str());
    return hr;
}

// Resolves the registry path for manifestDir, takes its cross-process lock
// and loads it. The lock stays held for the life of this object. result is
// S_OK or the HRESULT that the entry point named by function returns for the
// failed step. With S_OK, exists is false when there is no registry file.
struct LockedRegistry {
    LockedRegistry(PCWSTR manifestDir, PCWSTR function)
        : path(ResolveManifestPath(manifestDir)),
          lock(path, kRegistryLockTimeoutMs) {
        if (!lock.Held()) {
            result = RegistryLockFailure(function);
            return;
        }
        const DWORD loadErr = manifest.Load(path);
        if (loadErr == ERROR_FILE_NOT_FOUND) return;
        if (loadErr != ERROR_SUCCESS) {
            result = HRESULT_FROM_WIN32(loadErr);
            return;
        }
        exists = true;
    }

    LockedRegistry(const LockedRegistry&) = delete;
    LockedRegistry& operator=(const LockedRegistry&) = delete;

    const ::LayerMount::HostPath path;
    ::LayerMount::VHD::ManifestLock lock;
    ::LayerMount::VHD::Manifest manifest;
    HRESULT result = S_OK;
    bool exists = false;
};

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

}

extern "C" {

LM_API HRESULT LM_CALL LayerMountVhdListLayers(LM_HANDLE          mount,
                                               PCWSTR              manifestDir,
                                               LM_VHD_LAYER_INFO* entries,
                                               UINT32              entriesCapacity,
                                               UINT32*             entriesWritten,
                                               UINT32*             entriesRequired)
{
    LM_ABI_ENTRY();
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

    LockedRegistry registry(manifestDir, L"LayerMountVhdListLayers");
    if (registry.result != S_OK) return registry.result;
    // Idempotent: no manifest on disk -> zero entries, S_OK.
    if (!registry.exists) return S_OK;

    auto layers = registry.manifest.ListLayers();
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
    LM_ABI_ENTRY();
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

    LockedRegistry registry(manifestDir, L"LayerMountVhdUnregisterLayer");
    if (registry.result != S_OK) return registry.result;
    // Idempotent: missing manifest == nothing to remove.
    if (!registry.exists) return S_OK;

    bool removed = registry.manifest.RemoveLayer(layerId);
    if (removed) {
        DWORD saveErr = registry.manifest.Save(registry.path);
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
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (mount  == nullptr) return E_HANDLE;
    if (layerId  == nullptr || *layerId == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) return E_HANDLE;

    LockedRegistry registry(manifestDir, L"LayerMountVhdGetLayerMetadataJson");
    if (registry.result != S_OK) return registry.result;
    if (!registry.exists) return STG_E_PATHNOTFOUND;

    const auto* entry = registry.manifest.GetLayer(layerId);
    if (entry == nullptr) {
        return STG_E_PATHNOTFOUND;
    }

    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : entry->metadata) {
        j[::LayerMount::VHD::WideToUtf8(k)] = ::LayerMount::VHD::WideToUtf8(v);
    }
    const std::wstring out = ::LayerMount::VHD::Utf8ToWide(j.dump());

    const SIZE_T need = out.size() + 1;
    if (requiredChars != nullptr) *requiredChars = need;
    if (buffer == nullptr || bufferChars == 0) return S_OK;
    if (bufferChars < need) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    std::memcpy(buffer, out.c_str(), need * sizeof(wchar_t));
    return S_OK;

    LM_ABI_END();
}

}
