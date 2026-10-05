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

// The result of FileRename::MoveToUpper.
struct MovedFile {
    NTSTATUS status;
    // True when the rename copied the source up as a metacopy shell.
    bool stagedShell;
    // The upper path of the source before the move, where a failed
    // rename moves the file back.
    std::wstring oldUpperPath;
};

// Moves a file of the merged view to a new name in the upper.
class FileRename {
public:
    FileRename(ConfigRef config, PathResolver& pathResolver, CopyUp& copyUp);

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
    // ReplaceExisting::No fails with STATUS_OBJECT_NAME_COLLISION when an
    // entry holds the new upper path. When a lower holds the source, the
    // caller writes the whiteout at the old name.
    MovedFile MoveToUpper(const std::wstring& oldRelativePath,
                          const std::wstring& newRelativePath,
                          ReplaceExisting replace,
                          RenameCopyUp copyUpMode);

private:
    const LayerConfig& config_;
    PathResolver& pathResolver_;
    CopyUp& copyUp_;
};

}
