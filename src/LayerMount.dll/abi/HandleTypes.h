// HandleTypes.h -- Internal header. Maps each public opaque handle kind
// to its C++ impl type and assigns each a per-kind magic constant.
//
// NOT installed. DLL consumers must never include this file.
//
// Handle encoding (see HandleTable.h for the full scheme):
//   bits [63:48] per-kind magic (so a wrong-kind handle fails the Resolve)
//   bits [47:24] 24-bit generation counter (stale-handle detection)
//   bits [23:0]  24-bit slot index
//
// Sizing: the encoding requires a 64-bit pointer. Windows x64 is the
// only build target (project decisions memo 2026-04-14).

#pragma once

#include "../public/LayerMount.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

// Forward declarations of impl types. Including their full headers from
// here would drag impl-side dependencies into every TU that touches a
// handle; the impl side owns the includes.
//
// Note: the outer namespace `LayerMount` contains a class named
// `LayerMount`, so every reference to this namespace from inside
// `LayerMount::abi` must be root-qualified (`::LayerMount::...`) to keep
// the compiler from resolving `LayerMount` to the inner class.
namespace LayerMount {
    class LayerMount;
    struct FileContext;
    namespace VHD { class VHDLayerManager; class VhdHandle; }
    namespace VSS { class VSSManager; struct SnapshotInfo; }
    namespace LayerImage { class LayerImageManager; }
}

namespace LayerMount::abi {

static_assert(sizeof(void*) == 8,
              "LayerMount.dll handle encoding requires 64-bit pointers.");

// -------------------------------------------------------------------------
// Per-kind magic constants. Keep these distinct and stable; changing a
// magic invalidates every handle ever handed out by a previous build.
// -------------------------------------------------------------------------
constexpr std::uint16_t kMagicLayerMount      = 0xFE01;
constexpr std::uint16_t kMagicFile         = 0xFE02;
constexpr std::uint16_t kMagicVhd          = 0xFE03;
constexpr std::uint16_t kMagicVssSnapshot  = 0xFE04;
constexpr std::uint16_t kMagicImage        = 0xFE05;

// -------------------------------------------------------------------------
// Payload typedefs -- one per public handle kind.
// -------------------------------------------------------------------------

// LM_HANDLE -> LayerMountHolder, which binds the core engine instance to
// the host-attached flag a host adapter toggles around its mount
// lifetime. Wrapping the core in a holder keeps host-specific lifecycle
// state out of the engine class (impl/ stays host-agnostic).
// `hostAttached` is updated by LayerMountSetHostAttached and
// observed by LayerMountDestroy. `childCount` tracks live child
// file handles opened through LayerMountOpenFile / LayerMountCreateFile;
// LayerMountDestroy refuses to release the instance while any child is
// still outstanding (backstops the lifecycle contract against caller
// misuse). File holders additionally pin the parent via a shared_ptr,
// so a premature LayerMountDestroy cannot be bypassed to cause
// use-after-free even if the child-count check is skipped.
struct LayerMountHolder {
    std::unique_ptr<::LayerMount::LayerMount> core;
    std::atomic<bool>                       hostAttached{false};
    std::atomic<std::uint32_t>              childCount{0};
};
using LayerMountPayload = LayerMountHolder;

// LM_FILE_HANDLE -> FileHolder. Bundles the per-open FileContext (which
// owns the NT handle) with a shared_ptr to the parent LayerMountHolder.
// Holding the parent by shared_ptr pins the LayerMount engine for the
// lifetime of any outstanding file handle: even if LayerMountDestroy is
// called out of order, the engine stays alive until every FileHolder
// has been released. The `mount` raw pointer is a cached accessor
// (equal to `parentOwner->core.get()`) for call-site convenience.
// Lifecycle contract: every FileHolder must still be released via
// LayerMountCloseFile before LayerMountDestroy succeeds -- see the
// childCount backstop on LayerMountHolder.
struct FileHolder {
    std::shared_ptr<LayerMountHolder>            parentOwner;
    ::LayerMount::LayerMount*                   mount = nullptr;
    std::unique_ptr<::LayerMount::FileContext> ctx;
};
using FilePayload = FileHolder;

// LM_VHD_HANDLE -> VhdHolder, which binds the manager, the open handle,
// and the path into one opaque payload. This keeps the ABI from having
// to expose either the manager or the handle separately.
//
// `open` is populated only between a successful LayerMountVhdAttach and the
// matching LayerMountVhdDetach / LayerMountVhdClose. For
// AttachLifetime::ProcessScoped the OS releases the attach when the last
// handle closes, so the holder must keep the VhdHandle alive across the
// Attach -> Detach window; for Permanent lifetime the VHD stays attached
// regardless, but holding the handle keeps the semantics uniform.
//
// The readOnly / suppressDriveLetter / lifetime fields echo the values
// captured from LM_VHD_CONFIG on Create/Open so the parameter-free
// LayerMountVhdAttach shim can forward them to VHDLayerManager::AttachVHD
// without re-taking a config.
//
// attachedPhysicalPath caches the \\.\PhysicalDriveN path returned by
// AttachVHD. It supports the two-call buffer pattern on LayerMountVhdAttach
// without re-attaching (AttachVHD is not idempotent) and is cleared on
// Detach so a subsequent Attach can populate it again.
struct VhdHolder {
    ::LayerMount::VHD::VHDLayerManager*            manager = nullptr;
    std::unique_ptr<::LayerMount::VHD::VhdHandle>  open;
    std::wstring                                  path;
    std::wstring                                  attachedPhysicalPath;
    BOOL                                          readOnly = FALSE;
    BOOL                                          suppressDriveLetter = FALSE;
    LM_VHD_ATTACH_LIFETIME                       lifetime = LM_VHD_ATTACH_PERMANENT;
    // Serializes mutations to `open` / `attachedPhysicalPath` so
    // concurrent Attach / Detach / GetVolumeGuid on the same handle
    // observe a consistent state. Path / config fields above are set
    // once at create time and then read-only.
    std::mutex                                    stateMutex;
};
using VhdPayload = VhdHolder;

// LM_VSS_SNAPSHOT_HANDLE -> VssSnapshotHolder. The VSS subsystem is
// session-based (VSSManager owns the COM scope); the handle binds the
// owning session to a concrete snapshot so callers can use snapshot
// handles without re-entering the manager.
struct VssSnapshotHolder {
    ::LayerMount::VSS::VSSManager* manager = nullptr;
    std::unique_ptr<::LayerMount::VSS::SnapshotInfo> info;
};
using VssSnapshotPayload = VssSnapshotHolder;

// LM_IMAGE_HANDLE -> ImageHolder. The layer-image subsystem is currently
// path-based (LayerImageManager has no long-lived per-image state); the
// holder lets future work attach per-image context without breaking the
// ABI.
struct ImageHolder {
    ::LayerMount::LayerImage::LayerImageManager* manager = nullptr;
    std::wstring imagePath;
};
using ImagePayload = ImageHolder;

} // namespace LayerMount::abi
