#include "ComScope.h"

namespace LayerMount {

namespace {

// Per-thread ref-count. Incremented on every ComScope ctor that found
// the thread already initialized (by this DLL), decremented on every
// matching dtor. The outermost scope owns the CoInitializeEx /
// CoUninitialize pair.
thread_local int g_comRefCount = 0;

} // namespace

ComScope::ComScope() noexcept
    : initResult_(E_FAIL)
    , ownsInit_(false)
{
    if (g_comRefCount > 0) {
        // Nested scope on a thread we previously initialized. Just bump
        // the count and report S_FALSE so callers know init was a no-op.
        ++g_comRefCount;
        initResult_ = S_FALSE;
        return;
    }

    initResult_ = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(initResult_)) {
        ownsInit_ = true;
        g_comRefCount = 1;
    } else if (initResult_ == S_FALSE) {
        // COM was already initialized on this thread with a compatible
        // mode, by something other than this DLL (caller code, another
        // library). Treat it as a nested scope so we don't call
        // CoUninitialize on a state we don't own.
        ownsInit_ = true; // we still need to pair with CoUninitialize
        g_comRefCount = 1;
    }
    // On RPC_E_CHANGED_MODE or other failure: ownsInit_ stays false,
    // Ok() returns false, VSS shims bail out with a descriptive error.
}

ComScope::~ComScope() noexcept
{
    if (!ownsInit_) {
        if (initResult_ == S_FALSE && g_comRefCount > 0) {
            // Nested no-op scope.
            --g_comRefCount;
        }
        return;
    }

    if (--g_comRefCount == 0) {
        ::CoUninitialize();
    }
}

} // namespace LayerMount
