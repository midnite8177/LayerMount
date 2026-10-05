#pragma once

#include "LayerMount.h"
#include "LayerPath.h"

#include <string>
#include <vector>

namespace LayerMount {

class WhiteoutManager;

// Merges the directory's entries across the upper and the lowers. An upper
// whiteout at the directory or at an ancestor hides the lowers, as it does
// for an open. A lower whiteout there hides that lower and the lowers below
// it. The merge lists the .wh. names of a directory that IsInLinkTarget
// accepts as ordinary entries, and reads no whiteout there. A layer that
// holds the directory but cannot list it gives the status of that scan's
// Win32 error and no entries, whether it is the upper or a lower. An unsafe
// path, or a path that IsReservedOverlayPath refuses, gives STATUS_SUCCESS
// and no entries.
MergedDirectory MergeDirectoryAcrossLayers(const LayerConfig& config,
                                           const WhiteoutManager& whiteoutMgr,
                                           const std::wstring& dirRelativePath);

// A merge of the directory at dirPath, and the ancestry of the directory in
// each layer that the merge reached: layers[0] is the upper, and
// layers[i + 1] is lower i. A merge of a child directory starts from it.
struct MergedDirectoryWithAncestry {
    MergedDirectory merged;
    std::wstring dirPath;
    std::vector<LayerAncestry> layers;
};

// Whether the merged view of the directory at dirNorm is in a link that
// ends the lookup, or under one, as FindLinkInView finds it. That is a
// junction or a directory symbolic link in the upper, or in the first lower
// that holds the path when no higher layer hides it. A component whose
// reparse tag the walk cannot read can be a link, so it counts too. In
// overlayfs the target of such a link is not an overlay directory, so a
// .wh. name there is an ordinary name. dirNorm must be the output of
// NormalizePath. Costs what FindLinkInView costs.
bool IsInLinkTarget(const LayerConfig& config,
                    const WhiteoutManager& whiteoutMgr,
                    const std::wstring& dirNorm);

// IsReservedRelativePath for a path that the overlay resolves. A path whose
// first .wh. segment is in a directory that IsInLinkTarget accepts is not
// reserved, because a .wh. name in a link target is an ordinary name, as in
// overlayfs. Reads the layers only for a path with a .wh. segment.
// normalized must be the output of NormalizePath. Use this at every gate
// that resolves a caller's path in the overlay. IsReservedRelativePath is
// the string check only, and a marker write uses it, because no marker
// goes into a link target.
bool IsReservedOverlayPath(const LayerConfig& config,
                           const WhiteoutManager& whiteoutMgr,
                           const std::wstring& normalized);

// Merges the directory as MergeDirectoryAcrossLayers does, and keeps the
// ancestry for MergeChildDirectory.
MergedDirectoryWithAncestry MergeDirectoryWithAncestry(const LayerConfig& config,
                                                       const WhiteoutManager& whiteoutMgr,
                                                       const std::wstring& dirRelativePath);

// Merges childName, a directory that the merge parent lists, with the same
// rules as MergeDirectoryAcrossLayers. Reads only the child's own path in
// each layer to find what hides the lowers. A layer that the merge of the
// parent did not reach adds nothing to the child.
MergedDirectoryWithAncestry MergeChildDirectory(const LayerConfig& config,
                                                const WhiteoutManager& whiteoutMgr,
                                                const MergedDirectoryWithAncestry& parent,
                                                const std::wstring& childName);

}
