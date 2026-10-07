#pragma once

#include "LayerMount.h"

#include <optional>
#include <string>

namespace LayerMount {

class CopyUp;
class PathResolver;
class WhiteoutManager;

// What the link check of a rename found above its source and destination.
struct RenameLinks {
    // The source is under a link, in the upper or in a lower. A rename that
    // passed the check moves the entry within that link's target.
    bool sourceInLinkTarget;
    // The destination is under a link, in the upper or in a lower. A
    // destination that is the link entry itself is not.
    bool destinationInLinkTarget;
    // The lower link above both the source and the destination, when the
    // merged view shows the destination's parent as a directory or as a
    // link that leads to one. The rename copies it up as a link before it
    // moves the entry, so the move acts on the link target.
    std::optional<std::wstring> lowerLinkToCopyUp;
};

// Checks a rename against the links that FindLinkAbove finds above oldNorm
// and newNorm, fills *links, and changes nothing. Both paths are in
// NormalizePath form. A component that either lookup cannot read can be a
// link, so the rename fails with STATUS_ACCESS_DENIED. When the two links
// differ and the merged view shows the destination's parent as a
// directory, the rename fails with STATUS_NOT_SAME_DEVICE, as overlayfs
// fails a rename out of a symlink target with EXDEV. That covers a rename
// between the overlay and a link target, and between two link targets.
// A parent that is a link counts as a directory only when the link leads
// to one. Overlayfs looks up both parents before that check, so a
// destination parent that is missing, a file, or a link that leads to no
// directory passes here and fails the rename later with its own status.
// When the links differ, or both are the same lower link, a failed open
// or read of the target of a parent link returns the status of that
// failure.
NTSTATUS CheckRenameLinkBoundary(const LayerConfig& config,
                                 const WhiteoutManager& whiteoutMgr,
                                 const PathResolver& pathResolver,
                                 const std::wstring& oldNorm,
                                 const std::wstring& newNorm,
                                 RenameLinks* links);

// Copies links.lowerLinkToCopyUp up as a link with CopyUp::CopyUpDirectory.
// Returns STATUS_SUCCESS and writes nothing when the rename has no lower
// link to copy up.
NTSTATUS CopyUpRenameLink(CopyUp& copyUp, const RenameLinks& links);

}
