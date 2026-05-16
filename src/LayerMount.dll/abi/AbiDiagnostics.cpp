// AbiDiagnostics.cpp -- Diagnostics, eventing, and process-tracker ABI
// entry points.
//
// LayerMountGetStats              snapshot the engine's LayerMountStats atomics
//                              into a POD LM_STATS.
// LayerMountSetEventCallback      install a host callback on the per-overlay
//                              EventEmitter slot; engine code fans out
//                              warnings / copy-up / whiteout / access-
//                              denied events through it.
// LayerMountProcessTrackerEnable  runtime toggle for the tracker.
// LayerMountProcessTrackerSetRules  (re-)load rules JSON from disk.
// LayerMountProcessTrackerExportJson  caller-buffer JSON export.
// LayerMountProcessTrackerExportCsv   caller-buffer CSV export.

#include "../public/LayerMount.h"
#include "AbiGuard.h"
#include "ErrorTls.h"
#include "EventEmitter.h"
#include "HandleTable.h"
#include "HandleTypes.h"
#include "../impl/LayerMount.h"
#include "../impl/ProcessTracker.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace {

// Convert a UTF-8 std::string to a std::wstring via MultiByteToWideChar.
// Used to bridge ProcessTracker's UTF-8 export output to the ABI's
// UTF-16 buffer convention. Returns S_OK + populated wide string on
// success; HRESULT_FROM_WIN32(...) on failure so the caller can surface
// conversion failure as a real ABI error instead of silently reporting
// a successful empty export. MB_ERR_INVALID_CHARS makes malformed UTF-8
// explicit rather than silently substituting replacement characters.
HRESULT Utf8ToWide(const std::string& utf8, std::wstring* out) {
    out->clear();
    if (utf8.empty()) return S_OK;
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             utf8.c_str(),
                                             static_cast<int>(utf8.size()),
                                             nullptr, 0);
    if (needed <= 0) {
        const DWORD err = ::GetLastError();
        return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_NO_UNICODE_TRANSLATION);
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              utf8.c_str(),
                                              static_cast<int>(utf8.size()),
                                              wide.data(), needed);
    if (written <= 0) {
        const DWORD err = ::GetLastError();
        return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_NO_UNICODE_TRANSLATION);
    }
    wide.resize(static_cast<size_t>(written));
    *out = std::move(wide);
    return S_OK;
}

// Two-call buffer emit. NUL terminator counted.
HRESULT EmitWString(const std::wstring& src,
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

} // namespace

extern "C" {

LM_API HRESULT LM_CALL LayerMountGetStats(LM_HANDLE  handle,
                                         LM_STATS*  outStats)
{
    using namespace ::LayerMount::abi;

    if (handle   == nullptr) return E_HANDLE;
    if (outStats == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    // Snapshot every atomic with memory_order_relaxed -- stats are
    // advisory (dashboards, test assertions), so we don't need ordering
    // guarantees across fields. Each std::atomic.load() reads coherently.
    const auto& s = mountHolder->core->Stats();
    outStats->cacheHits                    = s.cacheHits.load(std::memory_order_relaxed);
    outStats->cacheMisses                  = s.cacheMisses.load(std::memory_order_relaxed);
    outStats->copyUpCount                  = s.copyUpCount.load(std::memory_order_relaxed);
    outStats->readCount                    = s.readCount.load(std::memory_order_relaxed);
    outStats->writeCount                   = s.writeCount.load(std::memory_order_relaxed);
    outStats->activeHandles                = s.activeHandles.load(std::memory_order_relaxed);
    outStats->bytesRead                    = s.bytesRead.load(std::memory_order_relaxed);
    outStats->bytesWritten                 = s.bytesWritten.load(std::memory_order_relaxed);
    outStats->cleanupMetadataFailureCount  = s.cleanupMetadataFailureCount.load(std::memory_order_relaxed);
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountSetEventCallback(LM_HANDLE         handle,
                                                 LM_EVENT_CALLBACK callback,
                                                 void*              userContext)
{
    using namespace ::LayerMount::abi;

    if (handle == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    // callback == nullptr uninstalls. The EventEmitter is the single
    // source of truth; engine subsystems already hold a pointer to it
    // and emit through it unconditionally (a NULL slot is a cheap no-op).
    if (callback == nullptr) {
        mountHolder->core->Events().Clear();
    } else {
        mountHolder->core->Events().Set(callback, userContext);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountProcessTrackerEnable(LM_HANDLE handle,
                                                     BOOL       enabled)
{
    using namespace ::LayerMount::abi;

    if (handle == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    return mountHolder->core->SetProcessTrackerEnabled(enabled != FALSE);

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountProcessTrackerSetRules(LM_HANDLE handle,
                                                       PCWSTR     rulesPath)
{
    using namespace ::LayerMount::abi;

    if (handle    == nullptr) return E_HANDLE;
    if (rulesPath == nullptr || *rulesPath == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    auto tracker = mountHolder->core->Tracker();
    if (tracker == nullptr) {
        ErrorTls::Set(E_ILLEGAL_METHOD_CALL,
                      L"LayerMountProcessTrackerSetRules: process tracker is not "
                      L"enabled. Call LayerMountProcessTrackerEnable(..., TRUE) first.");
        return E_ILLEGAL_METHOD_CALL;
    }

    // LoadRules returns false on parse / IO failure; translate to a
    // generic E_FAIL with a descriptive TLS message since ProcessTracker
    // doesn't surface a more precise code today.
    if (!tracker->LoadRules(rulesPath)) {
        ErrorTls::Set(E_FAIL,
                      L"LayerMountProcessTrackerSetRules: failed to load rules "
                      L"file (parse error or path not accessible).");
        return E_FAIL;
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountProcessTrackerExportJson(LM_HANDLE handle,
                                                         PWSTR      buffer,
                                                         SIZE_T     bufferChars,
                                                         SIZE_T*    requiredChars)
{
    using namespace ::LayerMount::abi;

    if (handle == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    auto tracker = mountHolder->core->Tracker();
    if (tracker == nullptr) {
        ErrorTls::Set(E_ILLEGAL_METHOD_CALL,
                      L"LayerMountProcessTrackerExportJson: process tracker is not "
                      L"enabled.");
        return E_ILLEGAL_METHOD_CALL;
    }

    std::wstring content;
    const HRESULT convHr = Utf8ToWide(tracker->ExportLogAsJson(), &content);
    if (FAILED(convHr)) {
        ErrorTls::Set(convHr,
                      L"LayerMountProcessTrackerExportJson: failed to convert "
                      L"tracker log from UTF-8 to UTF-16.");
        return convHr;
    }
    return EmitWString(content, buffer, bufferChars, requiredChars);

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountProcessTrackerExportCsv(LM_HANDLE handle,
                                                        PWSTR      buffer,
                                                        SIZE_T     bufferChars,
                                                        SIZE_T*    requiredChars)
{
    using namespace ::LayerMount::abi;

    if (handle == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    auto tracker = mountHolder->core->Tracker();
    if (tracker == nullptr) {
        ErrorTls::Set(E_ILLEGAL_METHOD_CALL,
                      L"LayerMountProcessTrackerExportCsv: process tracker is not "
                      L"enabled.");
        return E_ILLEGAL_METHOD_CALL;
    }

    std::wstring content;
    const HRESULT convHr = Utf8ToWide(tracker->ExportLogAsCsv(), &content);
    if (FAILED(convHr)) {
        ErrorTls::Set(convHr,
                      L"LayerMountProcessTrackerExportCsv: failed to convert "
                      L"tracker log from UTF-8 to UTF-16.");
        return convHr;
    }
    return EmitWString(content, buffer, bufferChars, requiredChars);

    LM_ABI_END();
}

} // extern "C"
