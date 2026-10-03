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
// it. A layer that holds the directory but cannot list it gives the status
// of that scan's Win32 error and no entries, whether it is the upper or a
// lower. An unsafe or reserved path gives STATUS_SUCCESS and no entries.
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
