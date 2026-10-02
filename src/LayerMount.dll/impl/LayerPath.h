#pragma once

#include "LayerMount.h"

namespace LayerMount {

class WhiteoutManager;

// Returns layerPath with one trailing separator. A layerPath that already
// ends in one, such as a drive root or a shadow-copy device root, gets no
// second one, because FindFirstFileW refuses a doubled separator in an
// extended-form (\\?\) path, though a plain path accepts one.
std::wstring LayerDirWithSeparator(const std::wstring& layerPath);

// Returns the FindFirstFileW search pattern for dirRelativePath in the
// layer at layerPath. The pattern never holds a doubled separator.
std::wstring JoinLayerScanPath(const std::wstring& layerPath,
                               const std::wstring& dirRelativePath);

// Walks dirRelativePath from the layer root and returns true at the first
// component that is a non-directory. A component that does not exist ends
// the walk with false. Any other failure to read a component's attributes
// returns true, so a layer that the walk cannot read hides what is below
// it. Costs one GetFileAttributesW per component.
bool HasNonDirectorySelfOrAncestorInLayer(const std::wstring& dirRelativePath,
                                          const std::wstring& layerPath);

// A directory in one layer. dirNorm is normalized and relative to the layer
// root; an empty dirNorm is the root.
struct LayerDirectory {
    const WhiteoutManager& whiteoutMgr;
    const std::wstring& layerPath;
    const std::wstring& dirNorm;
};

// Whether a probe or a scan of the layer found the directory as a directory.
enum class DirectoryProbe {
    Found,
    Missed,
};

enum class LowerVisibility {
    Visible,
    HiddenByOpaqueMarker,
    HiddenByNonDirectory,
};

// Whether the lowers below the layer can hold entries under the directory.
// An opaque marker at the directory or at an ancestor in the layer hides
// them, and so does a non-directory there. A directory the layer was found
// to hold has no non-directory at its path or above, so only a miss walks
// the path.
LowerVisibility LowersBelow(const LayerDirectory& dir, DirectoryProbe probe);

}
