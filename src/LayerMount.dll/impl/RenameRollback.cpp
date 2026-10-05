#include "RenameRollback.h"
#include "Cache.h"
#include "CopyUp.h"
#include "LayerPath.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"

namespace LayerMount {

RenameRollback::RenameRollback(ConfigRef config,
                               PathResolver& pathResolver,
                               WhiteoutManager& whiteoutMgr,
                               Cache& cache)
    : config_(config.Get())
    , pathResolver_(pathResolver)
    , whiteoutMgr_(whiteoutMgr)
    , cache_(cache) {
}

NTSTATUS RenameRollback::WhiteOutSource(const UpperRename& rename, WhiteoutType type) {
    const NTSTATUS whiteout = whiteoutMgr_.CreateWhiteout(rename.oldNorm, type);
    if (NT_SUCCESS(whiteout)) {
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(MoveBack(rename))) {
        whiteoutMgr_.RemoveWhiteout(rename.newNorm);
    }
    return whiteout;
}

NTSTATUS RenameRollback::RemoveDestinationWhiteoutOrUndo(
    const UpperRename& rename, RenameDestinationAside* destinationAside) {
    const NTSTATUS removeStatus = whiteoutMgr_.RemoveWhiteout(rename.newNorm);
    if (NT_SUCCESS(removeStatus)) {
        return STATUS_SUCCESS;
    }
    if (NT_SUCCESS(MoveBack(rename))) {
        whiteoutMgr_.RemoveWhiteout(rename.oldNorm);
    } else {
        destinationAside->Release();
    }
    return removeStatus;
}

NTSTATUS RenameRollback::MoveBack(const UpperRename& rename) {
    const NTSTATUS moveBack = MoveUpperEntry(pathResolver_.GetStoredUpperPath(rename.newNorm),
                                             rename.oldUpperPath, ReplaceExisting::No, config_);
    if (NT_SUCCESS(moveBack) && rename.opaqueMarker == UndoOpaqueMarker::Remove) {
        whiteoutMgr_.RemoveOpaque(rename.oldNorm);
    }
    cache_.InvalidateWithAncestors(rename.oldNorm);
    cache_.InvalidateWithAncestors(rename.newNorm);
    return moveBack;
}

}
