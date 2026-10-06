#pragma once

#include "WindowsNtStatus.h"

#include <string>

namespace LayerMount {

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

// The result of a rename's move or of a later step.
struct RenameStepResult {
    NTSTATUS status;
    // True when the step failed and left an entry at the new name, so the
    // replaced destination must not come back there.
    bool newNameOccupied;
};

// The result of a rename's move of its source: its status, whether it left
// a metacopy shell at the new name, whether a failed move left an entry at
// the new name, and what an undo of the move needs.
struct MovedSource {
    NTSTATUS status;
    bool stagedShell;
    bool newNameOccupied;
    UpperRename rename;
};

}
