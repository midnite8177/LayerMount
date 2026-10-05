#include "RenameLinkBoundary.h"
#include "CopyUp.h"
#include "LayerPath.h"
#include "PathResolver.h"

#include <filesystem>

namespace LayerMount {

namespace {

bool ParentShowsAsDirectory(const PathResolver& pathResolver, const std::wstring& normalizedPath) {
    const std::wstring parent = std::filesystem::path(normalizedPath).parent_path().wstring();
    if (parent.empty()) {
        return true;
    }
    const ResolvedPath shown = pathResolver.ResolvePath(parent);
    return shown.Found() && (shown.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
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
    links->lowerLinkToCopyUp.reset();
    const bool sameLink = source.stop == destination.stop && source.path == destination.path;
    if (!sameLink) {
        return ParentShowsAsDirectory(pathResolver, newNorm) ? STATUS_NOT_SAME_DEVICE
                                                             : STATUS_SUCCESS;
    }
    if (source.stop == LinkStop::Link && source.source == LayerSource::Lower &&
        ParentShowsAsDirectory(pathResolver, newNorm)) {
        links->lowerLinkToCopyUp = source.path;
    }
    return STATUS_SUCCESS;
}

NTSTATUS CopyUpRenameLink(CopyUp& copyUp, const RenameLinks& links) {
    if (!links.lowerLinkToCopyUp.has_value()) {
        return STATUS_SUCCESS;
    }
    return copyUp.CopyUpDirectory(*links.lowerLinkToCopyUp);
}

}
