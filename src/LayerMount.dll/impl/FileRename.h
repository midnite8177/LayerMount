#pragma once

#include "LayerMount.h"
#include "LayerPath.h"

#include <string>

namespace LayerMount {

class PathResolver;
class CopyUp;

// How a rename copies up a file that only a lower holds.
enum class RenameCopyUp {
    // A metacopy shell, or the data where a shell cannot stand in for the
    // file.
    ShellWhenPossible,
    FullCopy,
};

// What FileRename::MoveToUpper does with an entry at the new upper path.
enum class FileRenameReplace {
    // The entry fails the rename with STATUS_OBJECT_NAME_COLLISION.
    No,
    // The caller moved the upper destination aside, or the upper had none.
    // The move replaces an entry at the new upper path, as MoveUpperEntry
    // does with ReplaceExisting::Yes.
    AfterAside,
    // The destination file stays at the new upper path, and the move
    // replaces it in one step. Once it has, a sidecar record that cannot
    // move stays at the key of the old path, the replaced file's record
    // goes, the file stays at the new path, and an LM_EVT_WARNING event
    // goes out, because the replaced file cannot come back.
    InPlace,
};

// The result of FileRename::MoveToUpper.
struct MovedFile {
    NTSTATUS status;
    // True when the rename copied the source up as a metacopy shell.
    bool stagedShell;
    // The upper path of the source before the move, where a failed
    // rename moves the file back.
    std::wstring oldUpperPath;
};

// Moves a file of the merged view to a new name in the upper. A sidecar
// record that does not move with the file emits an LM_EVT_WARNING event
// through events.
class FileRename {
public:
    FileRename(ConfigRef config,
               PathResolver& pathResolver,
               CopyUp& copyUp,
               const abi::EventEmitter& events);

    // Fails with STATUS_OBJECT_PATH_NOT_FOUND and writes nothing when
    // CopyUp::EnsureUpperParent refuses the new parent. Then copies a
    // source that only a lower holds up and moves the upper file to
    // newRelativePath, which names it in the caller's case.
    // RenameCopyUp::FullCopy copies the source's data.
    // RenameCopyUp::ShellWhenPossible leaves a metacopy shell, except that
    // the data copies in full without sparse-file support on the upper,
    // for a reparse point that a copy-up clones, such as a file symbolic
    // link, and for a file with a user alternate data stream. A source in
    // no layer fails with STATUS_OBJECT_NAME_NOT_FOUND.
    // When a lower holds the source, the caller writes the whiteout at the
    // old name.
    MovedFile MoveToUpper(const std::wstring& oldRelativePath,
                          const std::wstring& newRelativePath,
                          FileRenameReplace replace,
                          RenameCopyUp copyUpMode);

private:
    const LayerConfig& config_;
    PathResolver& pathResolver_;
    CopyUp& copyUp_;
    const abi::EventEmitter& events_;
};

}
