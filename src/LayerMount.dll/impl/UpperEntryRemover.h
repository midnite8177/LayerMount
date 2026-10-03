#pragma once

#include "LayerMount.h"

#include <string>

namespace LayerMount {

class PathResolver;
class WhiteoutManager;
class Cache;

// Removes an entry from the merged view. The caller makes the delete
// checks first.
class UpperEntryRemover {
public:
    UpperEntryRemover(ConfigRef config,
                      PathResolver& pathResolver,
                      WhiteoutManager& whiteoutMgr,
                      Cache& cache);

    // Removes the upper entry at normalized, with its opaque marker, and
    // writes a whiteout when a lower holds the name. A junction or a
    // directory symbolic link goes, and its target stays. Returns the
    // status of a failed removal or whiteout write.
    NTSTATUS Remove(const std::wstring& normalized);

private:
    // Overlayfs gives a link no opaque marker. The marker check goes
    // through a link into its target, so a reparse point is never checked
    // or cleared as opaque.
    void RemoveOpaqueUnlessReparsePoint(const std::wstring& normalized, DWORD upperAttrs);

    const LayerConfig& config_;
    PathResolver& pathResolver_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
};

}
