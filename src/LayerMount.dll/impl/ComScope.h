// ComScope.h -- Per-thread ref-counted COM initializer.
//
// The VSS shims enter COM via CoInitializeEx(COINIT_MULTITHREADED). Any
// caller that already has a different apartment model on this thread
// would collide with a raw CoInitializeEx call, so the DLL can't
// naively initialize on behalf of its caller. Instead each VSS entry
// point opens a ComScope on entry: the first scope on a thread runs
// CoInitializeEx, nested scopes bump a thread-local counter only, and
// the outermost close calls CoUninitialize. Counter-based, not
// apartment-model-aware -- callers that already own the thread's COM
// state must ensure they've picked an initialization compatible with
// COINIT_MULTITHREADED (or the ComScope's CoInitializeEx call returns
// RPC_E_CHANGED_MODE, which the scope records and propagates).
//
// Internal -- not visible through the public C ABI.

#pragma once

#include <windows.h>
#include <objbase.h>

namespace LayerMount {

class ComScope {
public:
    ComScope() noexcept;
    ~ComScope() noexcept;

    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ComScope(ComScope&&) = delete;
    ComScope& operator=(ComScope&&) = delete;

    // HRESULT from the underlying CoInitializeEx for the outermost
    // scope. For nested scopes returns S_FALSE. Callers that care
    // (VSS shims) check this before using COM.
    HRESULT InitResult() const noexcept { return initResult_; }

    // True when this scope successfully established (or incremented)
    // the COM reference on this thread.
    bool Ok() const noexcept { return SUCCEEDED(initResult_) || initResult_ == S_FALSE; }

private:
    HRESULT initResult_;
    bool    ownsInit_; // true when this scope's dtor must CoUninitialize
};

} // namespace LayerMount
