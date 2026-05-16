// AbiCore.cpp -- Lifecycle ABI entry points:
// LayerMountCreate, LayerMountDestroy, LayerMountGetVersion, LayerMountGetLastErrorMessage.

#include "../public/LayerMount.h"
#include "AbiGuard.h"
#include "ErrorTls.h"
#include "HandleTable.h"
#include "HandleTypes.h"
#include "../impl/LayerMount.h"
#include "../impl/NtStatusUtil.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace {

// LM_CONFIG (POD) -> LayerConfig (C++ owning struct). Pre-validation in
// LayerMountCreate has already guaranteed that:
//   - upperPath, workDirPath are non-null
//   - if lowerPathCount > 0, lowerPaths is non-null and every entry is non-null
// String copies / vector growth may throw std::bad_alloc -- callers must
// invoke this only inside the LM_ABI_BEGIN/END guard.
::LayerMount::LayerConfig TranslateConfig(const LM_CONFIG& src) {
    ::LayerMount::LayerConfig dst;
    dst.upperPath = src.upperPath;
    dst.workDirPath = src.workDirPath;
    if (src.processRulesPath != nullptr) {
        dst.processRulesPath = src.processRulesPath;
    }
    dst.enableProcessTracking = (src.enableProcessTracking != FALSE);
    if (src.accessLogCapacity != 0) {
        dst.accessLogCapacity = static_cast<size_t>(src.accessLogCapacity);
    }
    if (src.pathCacheCapacity != 0) {
        // Thread the configured cache size through to LayerConfig so the
        // Cache ctor honors it. Zero means "engine default" -- preserve
        // backward-compat for hosts that did not set this field.
        dst.pathCacheCapacity = static_cast<size_t>(src.pathCacheCapacity);
    }
    dst.hostCapabilities = src.hostCapabilities;
    if (src.lowerPathCount > 0) {
        dst.lowerPaths.reserve(src.lowerPathCount);
        for (UINT32 i = 0; i < src.lowerPathCount; ++i) {
            dst.lowerPaths.emplace_back(src.lowerPaths[i]);
        }
    }
    // Discarded fields:
    //   src._reserved0 -- alignment padding only
    return dst;
}

} // namespace

extern "C" {

LM_API HRESULT LM_CALL LayerMountGetVersion(UINT32* major,
                                           UINT32* minor,
                                           UINT32* patch,
                                           UINT32* abiVersion)
{
    // Each out param is independently optional so a caller that only
    // wants the ABI version (the only field that gates compatibility)
    // need not allocate the other three.
    if (major      != nullptr) *major      = LM_VER_MAJOR;
    if (minor      != nullptr) *minor      = LM_VER_MINOR;
    if (patch      != nullptr) *patch      = LM_VER_PATCH;
    if (abiVersion != nullptr) *abiVersion = LM_ABI_VERSION;
    return S_OK;
}

LM_API HRESULT LM_CALL LayerMountGetLastErrorMessage(HRESULT  hr,
                                                    PWSTR    buffer,
                                                    SIZE_T   bufferChars,
                                                    SIZE_T*  requiredChars)
{
    // Direct passthrough -- ErrorTls::Last already implements the
    // two-call buffer pattern (sizing call, fits, ERROR_MORE_DATA) and
    // the per-thread HRESULT-match filter.
    return ::LayerMount::abi::ErrorTls::Last(hr, buffer, bufferChars, requiredChars);
}

LM_API HRESULT LM_CALL LayerMountCreate(const LM_CONFIG* config,
                                       LM_HANDLE* outHandle)
{
    using namespace ::LayerMount::abi;

    if (outHandle == nullptr) return E_POINTER;
    if (config    == nullptr) return E_POINTER;
    if (config->structSize < sizeof(LM_CONFIG)) return E_INVALIDARG;
    if (config->abiVersion != LM_ABI_VERSION) {
        ErrorTls::Set(E_INVALIDARG,
            L"LayerMountCreate: unsupported abiVersion (rebuild against current LayerMount.h).");
        return E_INVALIDARG;
    }
    if (config->upperPath   == nullptr) return E_INVALIDARG;
    if (config->workDirPath == nullptr) return E_INVALIDARG;
    if (config->lowerPathCount > 0 && config->lowerPaths == nullptr) return E_INVALIDARG;
    for (UINT32 i = 0; i < config->lowerPathCount; ++i) {
        if (config->lowerPaths[i] == nullptr) return E_INVALIDARG;
    }

    LM_ABI_BEGIN();

    ::LayerMount::LayerConfig layerCfg = TranslateConfig(*config);

    std::wstring err;
    if (!layerCfg.Validate(err)) {
        throw LayerMountAbiException(E_INVALIDARG, std::move(err));
    }
    if (!layerCfg.Prepare(err)) {
        throw LayerMountAbiException(E_FAIL, std::move(err));
    }

    auto holder = std::make_unique<LayerMountHolder>();
    holder->core = std::make_unique<::LayerMount::LayerMount>(std::move(layerCfg));
    const std::uint64_t encoded = Handles().mount.Allocate(std::move(holder));
    if (encoded == 0) {
        ErrorTls::Set(E_OUTOFMEMORY, L"LayerMount handle table exhausted.");
        return E_OUTOFMEMORY;
    }

    *outHandle = reinterpret_cast<LM_HANDLE>(static_cast<uintptr_t>(encoded));
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountCreateTransient(PCWSTR workDir,
                                                 UINT32 hostCapabilities,
                                                 LM_HANDLE* outHandle)
{
    if (workDir   == nullptr) return E_INVALIDARG;
    if (outHandle == nullptr) return E_POINTER;

    // Best-effort create: if the path can't be made, LayerMountCreate below
    // surfaces the precise Win32 error via LayerMountGetLastErrorMessage.
    std::error_code ec;
    std::filesystem::create_directories(workDir, ec);

    LM_CONFIG cfg{};
    cfg.structSize            = sizeof(cfg);
    cfg.abiVersion            = LM_ABI_VERSION;
    cfg.hostCapabilities      = hostCapabilities;
    cfg.enableProcessTracking = FALSE;
    cfg.lowerPathCount        = 0;
    cfg.lowerPaths            = nullptr;
    cfg.upperPath             = workDir;
    cfg.workDirPath           = workDir;
    cfg.processRulesPath      = nullptr;
    cfg.accessLogCapacity     = 0;
    cfg.pathCacheCapacity     = 0;

    return LayerMountCreate(&cfg, outHandle);
}

LM_API HRESULT LM_CALL LayerMountDestroy(LM_HANDLE handle)
{
    using namespace ::LayerMount::abi;

    if (handle == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));

    // Pin the holder for the duration of the checks below. Resolve
    // returns a shared_ptr so the payload cannot be destroyed out from
    // under us even if another thread calls Free concurrently.
    auto holder = Handles().mount.Resolve(encoded);
    if (holder == nullptr) {
        return E_HANDLE;
    }
    if (holder->hostAttached.load(std::memory_order_acquire)) {
        ErrorTls::Set(E_ILLEGAL_METHOD_CALL,
            L"LayerMountDestroy: instance is host-attached. The host adapter must "
            L"unmount and clear the attached flag (LayerMountSetHostAttached(handle, FALSE)) "
            L"before destroy.");
        return E_ILLEGAL_METHOD_CALL;
    }
    // Reject destroy while any child file handles are still outstanding.
    // File holders pin the parent via shared_ptr anyway, so the engine
    // can't actually be destroyed out from under them -- but that
    // safety net lets leaked file handles silently retire the overlay
    // slot and leave orphan state. Surface the misuse instead.
    if (holder->childCount.load(std::memory_order_acquire) != 0) {
        ErrorTls::Set(E_ILLEGAL_METHOD_CALL,
            L"LayerMountDestroy: instance has live file handles. Close every "
            L"LM_FILE_HANDLE obtained via LayerMountOpenFile / LayerMountCreateFile "
            L"before destroying the overlay.");
        return E_ILLEGAL_METHOD_CALL;
    }

    // Free returns nullptr if a concurrent thread has already freed the
    // slot between our Resolve and our Free; that's a use-after-free on
    // the caller's side, but the table itself stays consistent. Treat
    // it the same as a stale handle.
    auto freed = Handles().mount.Free(encoded);
    if (freed == nullptr) {
        return E_HANDLE;
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountSetHostAttached(LM_HANDLE handle, BOOL attached)
{
    using namespace ::LayerMount::abi;

    if (handle == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto holder = Handles().mount.Resolve(encoded);
    if (holder == nullptr) {
        return E_HANDLE;
    }
    holder->hostAttached.store(attached != FALSE, std::memory_order_release);
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountHResultToNtStatus(HRESULT hr, NTSTATUS* outStatus)
{
    if (outStatus == nullptr) return E_POINTER;

    if (hr == S_OK) {
        *outStatus = STATUS_SUCCESS;
        return S_OK;
    }

    // LayerMount-specific and well-known COM codes first. Hosts return these
    // verbatim from the C ABI and rely on a stable translation.
    switch (hr) {
        case E_HANDLE:              *outStatus = STATUS_INVALID_HANDLE;       return S_OK;
        case E_ILLEGAL_METHOD_CALL: *outStatus = STATUS_INVALID_DEVICE_STATE; return S_OK;
        case E_INVALIDARG:          *outStatus = STATUS_INVALID_PARAMETER;    return S_OK;
        case E_OUTOFMEMORY:         *outStatus = STATUS_NO_MEMORY;            return S_OK;
        case E_ACCESSDENIED:        *outStatus = STATUS_ACCESS_DENIED;        return S_OK;
        case E_POINTER:             *outStatus = STATUS_INVALID_PARAMETER;    return S_OK;
        case E_NOTIMPL:             *outStatus = STATUS_NOT_IMPLEMENTED;      return S_OK;
        case E_ABORT:               *outStatus = STATUS_CANCELLED;            return S_OK;
        case E_FAIL:                *outStatus = STATUS_UNSUCCESSFUL;         return S_OK;
        default: break;
    }

    // FACILITY_WIN32 -- the most common shape emitted by the engine
    // (HRESULT_FROM_WIN32 wrapping a ::GetLastError() result).
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) {
        *outStatus = ::LayerMount::NtStatusFromWin32(
            static_cast<DWORD>(HRESULT_CODE(hr)));
        return S_OK;
    }

    // FACILITY_NT_BIT -- HRESULT_FROM_NT(ntstatus). Invert the bit to
    // recover the original NTSTATUS.
    if (hr & 0x10000000L) {
        *outStatus = static_cast<NTSTATUS>(hr & ~0x10000000L);
        return S_OK;
    }

    *outStatus = STATUS_UNSUCCESSFUL;
    return S_OK;
}

} // extern "C"
