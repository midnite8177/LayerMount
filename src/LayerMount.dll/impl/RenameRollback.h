#pragma once

#include "LayerMount.h"

#include <string>

namespace LayerMount {

class PathResolver;
class WhiteoutManager;
class Cache;
class RenameDestinationAside;

// What an undo of a rename does with the opaque marker of the entry it
// moves back.
enum class UndoOpaqueMarker {
    Keep,
    // The entry was not opaque before the rename, and the rename marks it
    // opaque when a lower holds the new name. The undo removes the marker.
    Remove,
};

// A rename that moved an upper entry from oldUpperPath to newNorm. oldNorm
// and newNorm are in NormalizePath form.
struct UpperRename {
    std::wstring oldNorm;
    std::wstring newNorm;
    std::wstring oldUpperPath;
    UndoOpaqueMarker opaqueMarker;
};

// The result of a rename's move of its source: its status, whether it left
// a metacopy shell at the new name, and what an undo of the move needs.
struct MovedSource {
    NTSTATUS status;
    bool stagedShell;
    UpperRename rename;
};

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
    // When the entry cannot move back, removes the whiteout at
    // rename.newNorm, so the moved entry stays visible.
    NTSTATUS WhiteOutSource(const UpperRename& rename, WhiteoutType type);

    // Removes the upper whiteout at rename.newNorm. A whiteout left beside
    // the renamed entry hides it once this upper serves as a lower. When
    // the removal fails, the rename moves its entry back with MoveBack,
    // removes the whiteout at rename.oldNorm, and fails with the error of
    // the removal. When the entry cannot move back, the rename stays in
    // effect, and destinationAside keeps the replaced destination in the
    // work directory.
    NTSTATUS RemoveDestinationWhiteoutOrUndo(const UpperRename& rename,
                                             RenameDestinationAside* destinationAside);

    // Moves the upper entry at rename.newNorm back to rename.oldUpperPath
    // and returns the status of that move. When it moves back and
    // rename.opaqueMarker is UndoOpaqueMarker::Remove, also removes the
    // opaque marker at rename.oldNorm. A failure of that removal emits a
    // warning and does not change the returned status.
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
