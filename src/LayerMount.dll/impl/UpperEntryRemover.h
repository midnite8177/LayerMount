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

    // Removes the upper entry at normalized as RemoveUpperEntryOfKind does,
    // with the opaque marker of a directory, and writes a whiteout when a
    // lower holds the name. A link keeps the markers of its target. Returns
    // the status of a failed probe, removal or whiteout write.
    NTSTATUS Remove(const std::wstring& normalized);

private:
    const LayerConfig& config_;
    PathResolver& pathResolver_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
};

}
