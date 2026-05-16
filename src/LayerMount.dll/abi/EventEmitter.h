// EventEmitter.h -- Internal helper that owns the host's LM_EVENT_CALLBACK
// slot and synchronizes Set / Clear / Emit. Used by engine code (impl/) to
// fan out warnings, copy-up events, whiteout creations, and access-denied
// events without taking a hard dependency on the ABI shim layer.
//
// Header-only. Included by both impl/ (emit sites) and abi/ (the
// LayerMountSetEventCallback shim).

#pragma once

#include "../public/LayerMount.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace LayerMount::abi {

class EventEmitter {
public:
    EventEmitter() noexcept = default;
    EventEmitter(const EventEmitter&) = delete;
    EventEmitter& operator=(const EventEmitter&) = delete;

    void Set(LM_EVENT_CALLBACK callback, void* userContext) noexcept {
        std::unique_lock lock(mutex_);
        callback_ = callback;
        userContext_ = userContext;
    }

    // Clears the callback slot and drains any in-flight Emit calls. On
    // return, no further invocations of the previously-registered
    // callback are in progress or can be started, so the caller may
    // safely free the userContext (e.g. a managed GCHandle). Callers
    // must not invoke Clear from inside the callback itself -- doing so
    // would deadlock on the inflight drain.
    void Clear() noexcept {
        {
            std::unique_lock lock(mutex_);
            callback_ = nullptr;
            userContext_ = nullptr;
        }
        // Wait for concurrent Emit calls that snapshot a now-stale cb
        // to finish. New Emits will observe the null callback under the
        // shared lock and skip the increment.
        while (inflight_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
    }

    // Fan an event out to the registered callback. Safe to call from any
    // thread. No-op when no callback is installed (the common case during
    // bring-up; until 4.7 wires LayerMountSetEventCallback the slot is
    // always empty).
    void Emit(LM_EVENT_TYPE type,
              HRESULT        hr,
              PCWSTR         relativePath,
              PCWSTR         message,
              DWORD          pid = 0) const noexcept
    {
        // Snapshot the slot under shared lock and pin the call as
        // in-flight before releasing the lock. Incrementing inflight_
        // inside the shared lock guarantees Clear cannot observe
        // inflight == 0 while an Emit is between "read cb" and
        // "invoke cb".
        LM_EVENT_CALLBACK cb = nullptr;
        void* ctx = nullptr;
        {
            std::shared_lock lock(mutex_);
            cb = callback_;
            ctx = userContext_;
            if (cb == nullptr) return;
            inflight_.fetch_add(1, std::memory_order_acquire);
        }

        FILETIME now;
        ::GetSystemTimeAsFileTime(&now);

        LM_EVENT evt{};
        evt.type         = type;
        evt.hr           = hr;
        evt.relativePath = relativePath;
        evt.message      = message;
        evt.timestamp    = (static_cast<UINT64>(now.dwHighDateTime) << 32) |
                            now.dwLowDateTime;
        evt.pid          = pid;
        cb(&evt, ctx);
        inflight_.fetch_sub(1, std::memory_order_release);
    }

private:
    mutable std::shared_mutex  mutex_;
    LM_EVENT_CALLBACK         callback_    = nullptr;
    void*                      userContext_ = nullptr;
    mutable std::atomic<std::uint32_t> inflight_{0};
};

} // namespace LayerMount::abi
