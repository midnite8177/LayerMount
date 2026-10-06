#include "FileRename.h"
#include "CopyUp.h"
#include "PathResolver.h"
#include "RenameRollback.h"

#include <utility>

namespace LayerMount {

FileRename::FileRename(ConfigRef config,
                       PathResolver& pathResolver,
                       CopyUp& copyUp,
                       const abi::EventEmitter& events)
    : config_(config.Get())
    , pathResolver_(pathResolver)
    , copyUp_(copyUp)
    , events_(events) {
}

MovedFile FileRename::MoveToUpper(const std::wstring& oldRelativePath,
                                  const std::wstring& newRelativePath,
                                  FileRenameReplace replace,
                                  RenameCopyUp copyUpMode) {
    const std::wstring oldNorm = NormalizePath(oldRelativePath);
    const std::wstring newNorm = NormalizePath(newRelativePath);

    NTSTATUS status = copyUp_.EnsureUpperParent(newNorm);
    if (!NT_SUCCESS(status)) {
        return {status, false, {}};
    }

    bool stagedShell = false;
    if (!pathResolver_.ExistsInUpper(oldNorm)) {
        const FileCopyUpResult copied = copyUpMode == RenameCopyUp::FullCopy
            ? FileCopyUpResult{copyUp_.CopyUpFile(oldNorm), false}
            : copyUp_.CopyUpFileOrShell(oldNorm, kShellAtAnySize);
        if (!NT_SUCCESS(copied.status)) {
            return {copied.status, false, {}};
        }
        stagedShell = copied.stagedShell;
    }

    std::wstring oldUpperPath = pathResolver_.GetStoredUpperPath(oldRelativePath);
    const std::wstring newUpperPath =
        pathResolver_.GetUpperPathForNewEntry(CallerPath(newRelativePath));
    const UpperEntryMove move = replace == FileRenameReplace::InPlace
        ? MoveUpperEntryLeavingStuckRecords(oldUpperPath, newUpperPath, ReplaceExisting::Yes,
                                            config_)
        : MoveUpperEntry(oldUpperPath, newUpperPath,
                         replace == FileRenameReplace::No ? ReplaceExisting::No
                                                          : ReplaceExisting::Yes,
                         config_);
    WarnRecordLeftBehind(events_, move, NT_SUCCESS(move.status) ? newNorm : oldNorm);
    return {move.status, stagedShell, std::move(oldUpperPath)};
}

}
