#pragma once

#include "LayerMount.h"
#include "RenameResult.h"

#include <string>

namespace LayerMount {

class PathResolver;
class WhiteoutManager;
class Cache;
struct UpperEntryMove;

// Emits an LM_EVT_WARNING event at relativePath when move left a sidecar
// record at the key of a path where its entry is not. relativePath is the
// path of the entry after the move.
void WarnRecordLeftBehind(const abi::EventEmitter& events,
                          const UpperEntryMove& move,
                          const std::wstring& relativePath);

// The steps of a rename that run after its entry moved, and the undo that
// they share when one of them fails. A step of the undo that fails emits an
// LM_EVT_WARNING event through events with the failure and the path.
class RenameRollback {
public:
    RenameRollback(ConfigRef config,
                   PathResolver& pathResolver,
                   WhiteoutManager& whiteoutMgr,
                   Cache& cache,
                   const abi::EventEmitter& events);

    // Writes a whiteout of the given type at rename.oldNorm. When the write
    // fails, undoes the rename with MoveBack and returns the write error.
    // When the entry cannot move back, the result's newNameOccupied is true,
    // and the call removes the whiteout at rename.newNorm so the moved entry
    // stays visible.
    RenameStepResult WhiteOutSource(const UpperRename& rename, WhiteoutType type);

    // Removes the upper whiteout at rename.newNorm. A whiteout left beside
    // the renamed entry hides it once this upper serves as a lower. When
    // the removal fails, the rename moves its entry back with MoveBack,
    // removes the whiteout at rename.oldNorm, and fails with the error of
    // the removal. When the entry cannot move back, the result's
    // newNameOccupied is true.
    RenameStepResult RemoveDestinationWhiteoutOrUndo(const UpperRename& rename);

    // Moves the upper entry at rename.newNorm back to rename.oldUpperPath
    // and returns the status of that move. When it moves back and
    // rename.opaqueMarker is UndoOpaqueMarker::Remove, also removes the
    // opaque marker at rename.oldNorm. A failure of that removal, and a
    // sidecar record that does not move with the entry, emit a warning and
    // do not change the returned status.
    NTSTATUS MoveBack(const UpperRename& rename);

private:
    // Emits the LM_EVT_WARNING event of an undo step that failed with
    // failure at relativePath.
    void WarnUndoFailed(HRESULT failure, const std::wstring& relativePath,
                        PCWSTR message) const;

    const LayerConfig& config_;
    PathResolver& pathResolver_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
    const abi::EventEmitter& events_;
};

}
