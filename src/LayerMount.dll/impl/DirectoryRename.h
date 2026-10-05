#pragma once

#include "LayerMount.h"
#include "DirectoryMerge.h"
#include "LayerPath.h"
#include "EntryCopy.h"

#include <string>

namespace LayerMount {

class PathResolver;
class WhiteoutManager;
class Cache;
class CopyUp;

// Renames a directory or a directory link in the merged view. Builds a copy
// of a lower source in the work directory and moves it to the new upper
// path, and moves an upper source there.
class DirectoryRename {
public:
    DirectoryRename(ConfigRef config,
                    PathResolver& pathResolver,
                    WhiteoutManager& whiteoutMgr,
                    Cache& cache,
                    CopyUp& copyUp);

    // The old and new paths of a rename, as the caller gives them.
    struct RenameCallerPaths {
        const CallerPath& oldPath;
        const CallerPath& newPath;
    };

    // Builds a copy of the merged view of the old directory in the work
    // directory, marks it opaque, moves it to the new upper path, and removes
    // the old upper entry. The copy inherits the ACEs that the new upper
    // parent gives. The caller writes the whiteout at the old path.
    // A Link source copies as a link, without its upper shadow and without
    // opacity.
    // ReplaceExisting::No fails with STATUS_OBJECT_NAME_COLLISION when the
    // destination exists in the merged view. The move fails with
    // STATUS_OBJECT_NAME_COLLISION when an entry holds the new upper path,
    // and that entry stays as it was. The new upper entry gets its name in
    // the case of paths.newPath. A failure leaves the old tree as it was and
    // tries to remove the copy in the work directory.
    // Warning: this call never replaces an entry. With ReplaceExisting::Yes,
    // the caller must first move the destination aside with
    // CopyUp::SetRenameDestinationAside.
    NTSTATUS RenameLowerDirectory(const RenameCallerPaths& paths,
                                  EntryKind sourceKind,
                                  ReplaceExisting replace);

    // Fails with the status of CopyUp::EnsureUpperParent and writes nothing
    // when that call refuses the new parent. Moves the upper directory or
    // link and carries its opaque marker. When a lower layer has the new
    // path and the source directory is not opaque, marks the source opaque
    // before the move. The mark hides the lower children at the new path.
    // A failed mark fails the rename with its status, and nothing moves. A
    // Link source is never marked. When the move fails after a mark, the
    // source keeps the mark, and the lower children at the old path stay
    // hidden. The moved entry gets its name in the case of paths.newPath.
    // When a lower layer has the old path, the caller writes the whiteout
    // there. ReplaceExisting::No fails with STATUS_OBJECT_NAME_COLLISION
    // when the destination exists in the merged view.
    NTSTATUS RenameUpperDirectory(const RenameCallerPaths& paths,
                                  EntryKind sourceKind,
                                  ReplaceExisting replace);

private:
    // One name of the renamed directory: its normalized path and its path
    // in the upper layer.
    struct RenamedName {
        std::wstring norm;
        std::wstring upperPath;
    };

    bool DestinationExistsInMerged(const std::wstring& normalizedPath) const;

    // Fails with STATUS_OBJECT_NAME_COLLISION when replace is
    // ReplaceExisting::No and the destination exists in the merged view.
    // Then makes the parent of newCallerPath exist in the upper.
    NTSTATUS PrepareRenameDestination(const CallerPath& newCallerPath,
                                      ReplaceExisting replace);

    // Copies the merged view of oldName to stagedPath, a new path in the
    // work directory, as a plain directory. lowerSource is the first lower
    // that holds oldName, and it is not a link: the caller clones a link
    // with CloneReparsePointThroughWorkDir. A lower directory reparse point
    // that is not a link also copies as a plain directory. The copy gets
    // the layout, streams, attributes, times and security of the upper
    // directory of oldName, or of lowerSource when the upper has none, and
    // the copy-up record of lowerSource, and is marked opaque. A lower
    // directory that the merge cannot list fails the copy. On failure,
    // stagedPath can hold a partial tree, and the caller removes it.
    NTSTATUS CopyMergedDirectory(const ResolvedPath& lowerSource,
                                 const RenamedName& oldName,
                                 const std::wstring& stagedPath);

    // Copies each entry of oldDir, a merge of a directory, into the
    // directory at dstPath, at every depth. The merge applies the whiteouts
    // and opaque markers of every layer, so the copy holds no marker files.
    // The caller marks the root of the copy opaque. A directory that the
    // merge cannot list fails the copy. On failure, dstPath holds a partial
    // tree, and the caller removes it.
    NTSTATUS CopyMergedChildren(const MergedDirectoryWithAncestry& oldDir,
                                const std::wstring& dstPath);

    // Copies entry, which the merge oldParent lists, from the layer that
    // gives it into dstParentPath. The copy keeps the name that layer
    // gives the entry. An upper entry keeps its copy-up record, and a lower
    // entry gets a new one. An entry that ClonesReparsePoint selects copies
    // as a link, and any other directory copies as a plain directory.
    NTSTATUS CopyMergedEntry(const MergedDirectoryWithAncestry& oldParent,
                             const MergedEntry& entry,
                             const std::wstring& dstParentPath);

    const LayerConfig& config_;
    PathResolver& pathResolver_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
    CopyUp& copyUp_;
};

}
