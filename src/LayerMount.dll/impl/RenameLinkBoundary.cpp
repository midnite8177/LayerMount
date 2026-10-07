#include "RenameLinkBoundary.h"
#include "CopyUp.h"
#include "LayerPath.h"
#include "PathResolver.h"

namespace LayerMount {

namespace {

// What the merged view shows at the parent of normalizedPath, with a link
// followed. The overlay root is a directory.
LinkTarget ShownParentTarget(const PathResolver& pathResolver, const std::wstring& normalizedPath) {
    const std::wstring parent = ParentOfNormalizedPath(normalizedPath);
    if (parent.empty()) {
        return LinkTarget{LinkTargetKind::Directory, STATUS_SUCCESS};
    }
    return ShownLinkTarget(pathResolver.ResolvePath(parent));
}

}

NTSTATUS CheckRenameLinkBoundary(const LayerConfig& config,
                                 const WhiteoutManager& whiteoutMgr,
                                 const PathResolver& pathResolver,
                                 const std::wstring& oldNorm,
                                 const std::wstring& newNorm,
                                 RenameLinks* links) {
    const LinkInView source = FindLinkAbove(config, whiteoutMgr, oldNorm);
    const LinkInView destination = FindLinkAbove(config, whiteoutMgr, newNorm);
    if (source.stop == LinkStop::Unreadable || destination.stop == LinkStop::Unreadable) {
        return STATUS_ACCESS_DENIED;
    }

    links->sourceInLinkTarget = source.stop == LinkStop::Link;
    links->destinationInLinkTarget = destination.stop == LinkStop::Link;
    links->lowerLinkToCopyUp.reset();
    const bool sameLink = source.stop == destination.stop && source.path == destination.path;
    const bool withinLowerLink =
        sameLink && source.stop == LinkStop::Link && source.source == LayerSource::Lower;
    if (sameLink && !withinLowerLink) {
        return STATUS_SUCCESS;
    }
    const LinkTarget parent = ShownParentTarget(pathResolver, newNorm);
    switch (parent.kind) {
    case LinkTargetKind::NoDirectory:
        return STATUS_SUCCESS;
    case LinkTargetKind::Failed:
        return parent.failure;
    case LinkTargetKind::Directory:
        break;
    }
    if (!sameLink) {
        return STATUS_NOT_SAME_DEVICE;
    }
    links->lowerLinkToCopyUp = source.path;
    return STATUS_SUCCESS;
}

NTSTATUS CopyUpRenameLink(CopyUp& copyUp, const RenameLinks& links) {
    if (!links.lowerLinkToCopyUp.has_value()) {
        return STATUS_SUCCESS;
    }
    return copyUp.CopyUpDirectory(*links.lowerLinkToCopyUp);
}

}
