// HandleTable.h -- Internal header. Typed registry that maps opaque ABI
// handles to their C++ impl payloads.
//
// Encoding of the 64-bit handle value:
//   [63:48] 16-bit per-kind magic (kMagicLayerMount / kMagicFile / ...)
//   [47:24] 24-bit generation counter (per-slot; bumped on free)
//   [23:0]  24-bit slot index
// => up to 16,777,215 concurrent handles per kind (slot 0 is reserved
//    for "invalid"); 16M generations before wrap.
//
// Resolve rejects stale handles (generation mismatch), wrong-kind
// handles (magic mismatch), and invalid slot indices. A stale/bad
// handle returns nullptr; the ABI shim translates that to E_HANDLE.
//
// NOT installed. Consumers of LayerMount.dll do not see this header.

#pragma once

#include "HandleTypes.h"
#include "../public/LayerMount.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace LayerMount::abi {

// Encoding helpers shared by all typed tables.
namespace detail {

constexpr std::uint64_t kSlotBits       = 24;
constexpr std::uint64_t kGenerationBits = 24;
constexpr std::uint64_t kMagicBits      = 16;

constexpr std::uint64_t kSlotMask       = (1ull << kSlotBits) - 1;
constexpr std::uint64_t kGenerationMask = (1ull << kGenerationBits) - 1;
constexpr std::uint64_t kMagicMask      = (1ull << kMagicBits) - 1;

constexpr std::uint32_t kMaxSlot       = static_cast<std::uint32_t>(kSlotMask);
constexpr std::uint32_t kMaxGeneration = static_cast<std::uint32_t>(kGenerationMask);

inline std::uint64_t Encode(std::uint16_t magic,
                            std::uint32_t generation,
                            std::uint32_t slot) noexcept
{
    return (static_cast<std::uint64_t>(magic)      << (kSlotBits + kGenerationBits))
         | (static_cast<std::uint64_t>(generation) << kSlotBits)
         | (static_cast<std::uint64_t>(slot));
}

inline std::uint16_t DecodeMagic(std::uint64_t value) noexcept
{
    return static_cast<std::uint16_t>((value >> (kSlotBits + kGenerationBits)) & kMagicMask);
}

inline std::uint32_t DecodeGeneration(std::uint64_t value) noexcept
{
    return static_cast<std::uint32_t>((value >> kSlotBits) & kGenerationMask);
}

inline std::uint32_t DecodeSlot(std::uint64_t value) noexcept
{
    return static_cast<std::uint32_t>(value & kSlotMask);
}

} // namespace detail

// -------------------------------------------------------------------------
// HandleTable<T, Magic>
//
// Owns shared_ptr<T> payloads. Allocate returns a 64-bit opaque value
// the caller reinterpret_casts to its public handle type. Resolve takes
// a shared lock and returns a shared_ptr that pins the payload for the
// duration of the caller's use; Allocate and Free take an exclusive
// lock. The shared_ptr payload means a concurrent Free cannot destroy a
// payload while another thread still holds a Resolve result -- the slot
// is marked dead but the payload outlives the slot until the last
// observed reference drops. Payload-internal synchronization remains
// each impl type's responsibility.
// -------------------------------------------------------------------------
template <typename T, std::uint16_t Magic>
class HandleTable {
public:
    HandleTable() = default;
    HandleTable(const HandleTable&) = delete;
    HandleTable& operator=(const HandleTable&) = delete;

    // Returns the encoded handle value, or 0 on allocation failure /
    // slot exhaustion. The table takes ownership of `payload`.
    std::uint64_t Allocate(std::unique_ptr<T> payload)
    {
        if (!payload) {
            return 0;
        }
        // Promote to shared ownership so Resolve can pin the payload.
        std::shared_ptr<T> shared(std::move(payload));
        return AllocateShared(std::move(shared));
    }

    // Overload that takes a shared_ptr directly, for call sites that
    // already hold shared ownership (e.g. LayerMountOpenFile pins the parent
    // LayerMountHolder via its shared_ptr from the handle table).
    std::uint64_t Allocate(std::shared_ptr<T> payload)
    {
        if (!payload) {
            return 0;
        }
        return AllocateShared(std::move(payload));
    }

    // Returns a shared_ptr copy of the payload (pinned for the caller's
    // use) or nullptr on stale / wrong-kind / invalid input. A
    // concurrent Free can still retire the slot, but the payload itself
    // is kept alive until the returned shared_ptr is dropped.
    std::shared_ptr<T> Resolve(std::uint64_t handleValue) const noexcept
    {
        if (handleValue == 0) {
            return nullptr;
        }
        if (detail::DecodeMagic(handleValue) != Magic) {
            return nullptr;
        }
        const std::uint32_t slotIndex  = detail::DecodeSlot(handleValue);
        const std::uint32_t generation = detail::DecodeGeneration(handleValue);

        std::shared_lock lock(mutex_);
        if (slotIndex == 0 || slotIndex >= slots_.size()) {
            return nullptr;
        }
        const Slot& slot = slots_[slotIndex];
        if (!slot.live || slot.generation != generation) {
            return nullptr;
        }
        return slot.payload; // copy -- pins the payload for the caller
    }

    // Moves the payload out of the table and marks the slot free. Returns
    // nullptr on stale / wrong-kind / invalid input. The returned
    // shared_ptr may not be the last reference if a concurrent Resolve
    // beat this Free to the lock; actual destruction waits for all
    // observers to release.
    std::shared_ptr<T> Free(std::uint64_t handleValue) noexcept
    {
        if (handleValue == 0 || detail::DecodeMagic(handleValue) != Magic) {
            return nullptr;
        }
        const std::uint32_t slotIndex  = detail::DecodeSlot(handleValue);
        const std::uint32_t generation = detail::DecodeGeneration(handleValue);

        std::unique_lock lock(mutex_);
        if (slotIndex == 0 || slotIndex >= slots_.size()) {
            return nullptr;
        }
        Slot& slot = slots_[slotIndex];
        if (!slot.live || slot.generation != generation) {
            return nullptr;
        }
        std::shared_ptr<T> payload = std::move(slot.payload);
        slot.live = false;
        if (slot.generation >= detail::kMaxGeneration) {
            // Retire the slot rather than wrapping the generation counter.
            slot.generation = detail::kMaxGeneration;
        } else {
            slot.generation += 1;
            try {
                freeSlots_.push_back(slotIndex);
            } catch (...) {
                // If we cannot recycle the slot, leak it (finite but
                // rare). Do not propagate from noexcept.
                slot.generation = detail::kMaxGeneration;
            }
        }
        return payload;
    }

private:
    std::uint64_t AllocateShared(std::shared_ptr<T> payload)
    {
        std::unique_lock lock(mutex_);

        std::uint32_t slotIndex;
        if (!freeSlots_.empty()) {
            slotIndex = freeSlots_.back();
            freeSlots_.pop_back();
            slots_[slotIndex].payload = std::move(payload);
            slots_[slotIndex].live = true;
        } else {
            // kMaxSlot is the largest valid slot index (24-bit max value).
            // Slot 0 is reserved for "invalid", so the live range is
            // [1, kMaxSlot]. The next slot index handed out below is
            // slots_.size() (after lazy-pushing slot 0); reject only when
            // that index would exceed kMaxSlot. The previous bound
            // (slots_.size() >= kMaxSlot) lost the very last valid slot.
            if (slots_.size() > detail::kMaxSlot) {
                return 0; // slot space exhausted
            }
            // Slot 0 is reserved so a zeroed handle never resolves.
            if (slots_.empty()) {
                slots_.emplace_back();
                slots_.back().live = false;
            }
            slotIndex = static_cast<std::uint32_t>(slots_.size());
            slots_.emplace_back();
            slots_.back().payload = std::move(payload);
            slots_.back().generation = 1;
            slots_.back().live = true;
        }

        return detail::Encode(Magic, slots_[slotIndex].generation, slotIndex);
    }

    struct Slot {
        std::shared_ptr<T> payload;
        std::uint32_t      generation = 1;
        bool               live = false;
    };

    mutable std::shared_mutex mutex_;
    std::vector<Slot>         slots_;
    std::vector<std::uint32_t> freeSlots_;
};

// -------------------------------------------------------------------------
// Per-kind table typedefs and the registry accessor.
// -------------------------------------------------------------------------
using LayerMountTable      = HandleTable<LayerMountPayload,     kMagicLayerMount>;
using FileTable         = HandleTable<FilePayload,        kMagicFile>;
using VhdTable          = HandleTable<VhdPayload,         kMagicVhd>;
using VssSnapshotTable  = HandleTable<VssSnapshotPayload, kMagicVssSnapshot>;
using ImageTable        = HandleTable<ImagePayload,       kMagicImage>;

struct HandleRegistry {
    LayerMountTable  mount;
    FileTable        file;
    VhdTable         vhd;
    VssSnapshotTable vssSnapshot;
    ImageTable       image;
};

// Lazy-init singleton. Lives for the DLL's lifetime; cleaned up at
// process exit. No DllMain work required.
HandleRegistry& Handles() noexcept;

} // namespace LayerMount::abi
