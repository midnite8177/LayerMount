#include "RenameRollback.h"
#include "Cache.h"
#include "CopyUp.h"
#include "LayerPath.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "NtStatusUtil.h"
#include "../abi/EventEmitter.h"

namespace LayerMount {

void WarnRecordLeftBehind(const abi::EventEmitter& events,
                          const UpperEntryMove& move,
                          const std::wstring& relativePath) {
    if (NT_SUCCESS(move.recordLeftBehind)) {
        return;
    }
    events.Emit(LM_EVT_WARNING, HresultFromNtStatus(move.recordLeftBehind),
                relativePath.c_str(), L"A sidecar record did not move with its entry");
}

RenameRollback::RenameRollback(ConfigRef config,
                               PathResolver& pathResolver,
                               WhiteoutManager& whiteoutMgr,
                               Cache& cache,
                               const abi::EventEmitter& events)
    : config_(config.Get())
    , pathResolver_(pathResolver)
    , whiteoutMgr_(whiteoutMgr)
    , cache_(cache)
    , events_(events) {
}

NTSTATUS RenameRollback::WhiteOutSource(const UpperRename& rename, WhiteoutType type) {
    const NTSTATUS whiteout = whiteoutMgr_.CreateWhiteout(rename.oldNorm, type);
    if (NT_SUCCESS(whiteout)) {
        return STATUS_SUCCESS;
    }
    const NTSTATUS moveBack = MoveBack(rename);
    if (NT_SUCCESS(moveBack)) {
        return whiteout;
    }
    WarnUndoFailed(HresultFromNtStatus(moveBack), rename.newNorm,
                   L"The undo of a failed rename could not move the entry back");
    const NTSTATUS destinationWhiteout = whiteoutMgr_.RemoveWhiteout(rename.newNorm);
    if (!NT_SUCCESS(destinationWhiteout)) {
        WarnUndoFailed(HresultFromNtStatus(destinationWhiteout), rename.newNorm,
                       L"The undo of a failed rename could not remove the whiteout at the new name");
    }
    return whiteout;
}

NTSTATUS RenameRollback::RemoveDestinationWhiteoutOrUndo(
    const UpperRename& rename, RenameDestinationAside* destinationAside) {
    const NTSTATUS removeStatus = whiteoutMgr_.RemoveWhiteout(rename.newNorm);
    if (NT_SUCCESS(removeStatus)) {
        return STATUS_SUCCESS;
    }
    const NTSTATUS moveBack = MoveBack(rename);
    if (!NT_SUCCESS(moveBack)) {
        WarnUndoFailed(HresultFromNtStatus(moveBack), rename.newNorm,
                       L"The undo of a failed rename could not move the entry back");
        destinationAside->Release();
        return removeStatus;
    }
    const NTSTATUS sourceWhiteout = whiteoutMgr_.RemoveWhiteout(rename.oldNorm);
    if (!NT_SUCCESS(sourceWhiteout)) {
        WarnUndoFailed(HresultFromNtStatus(sourceWhiteout), rename.oldNorm,
                       L"The undo of a failed rename could not remove the whiteout at the old name");
    }
    return removeStatus;
}

NTSTATUS RenameRollback::MoveBack(const UpperRename& rename) {
    const UpperEntryMove move = MoveUpperEntry(pathResolver_.GetStoredUpperPath(rename.newNorm),
                                               rename.oldUpperPath, ReplaceExisting::No, config_);
    const NTSTATUS moveBack = move.status;
    WarnRecordLeftBehind(events_, move, NT_SUCCESS(moveBack) ? rename.oldNorm : rename.newNorm);
    if (NT_SUCCESS(moveBack) && rename.opaqueMarker == UndoOpaqueMarker::Remove &&
        !whiteoutMgr_.RemoveOpaque(rename.oldNorm)) {
        WarnUndoFailed(E_FAIL, rename.oldNorm,
                       L"The undo of a failed rename could not remove the opaque marker it wrote");
    }
    cache_.InvalidateWithAncestors(rename.oldNorm);
    cache_.InvalidateWithAncestors(rename.newNorm);
    return moveBack;
}

void RenameRollback::WarnUndoFailed(HRESULT failure, const std::wstring& relativePath,
                                    PCWSTR message) const {
    events_.Emit(LM_EVT_WARNING, failure, relativePath.c_str(), message);
}

}
