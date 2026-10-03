#pragma once

#include "LayerMount.h"
#include "DirectoryMerge.h"
#include "LayerPath.h"
#include "EntryCopy.h"
#include "../abi/CapabilityGate.h"

#include <string>

namespace LayerMount {

class PathResolver;
class WhiteoutManager;
class Cache;
class CopyUp;

// Renames a directory or a directory link in the merged view. Copies a
// lower source to the new upper path, and moves an upper source there.
class DirectoryRename {
public:
    DirectoryRename(ConfigRef config,
                    PathResolver& pathResolver,
                    WhiteoutManager& whiteoutMgr,
                    Cache& cache,
                    CopyUp& copyUp,
                    ::LayerMount::abi::CapabilityGate capabilities);

    // Copies the merged view of the old directory to the new upper path,
    // marks it opaque, and removes the old upper entry. The caller writes the
    // whiteout at the old path. sourceKind is Directory or Link. A Link
    // source is copied as a link, without its upper shadow and without
    // opacity.
    // ReplaceExisting::No fails with STATUS_OBJECT_NAME_COLLISION when the
    // destination exists in the merged view. The new upper entry gets its
    // name in newCallerPath's case. A failed copy leaves the old tree as it
    // was and tries to remove the new upper entry.
    NTSTATUS RenameLowerDirectory(const CallerPath& oldCallerPath,
                                  const CallerPath& newCallerPath,
                                  RenameEntryKind sourceKind,
                                  ReplaceExisting replace);

    // Fails with STATUS_OBJECT_PATH_NOT_FOUND and writes nothing when
    // CopyUp::EnsureUpperParent refuses the new parent. Moves the upper
    // directory and carries its opaque marker. Marks the moved entry opaque
    // when a lower layer has its new path, so lower children of a replaced
    // destination stay hidden. sourceKind is Directory or Link. A moved
    // Link never becomes opaque, because the marker would go into its
    // target. The moved entry
    // gets its name in newCallerPath's case. When a lower layer has the old
    // path, the caller writes the whiteout there.
    // ReplaceExisting::No fails with STATUS_OBJECT_NAME_COLLISION when the
    // destination exists in the merged view.
    NTSTATUS RenameUpperDirectory(const CallerPath& oldCallerPath,
                                  const CallerPath& newCallerPath,
                                  RenameEntryKind sourceKind,
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

    // Copies the merged view of oldName to the upper path of newName and
    // marks newName opaque. lowerSource is the first lower that holds
    // oldName. For a directory root, the copy then gets the copy-up record
    // of lowerSource, and the attributes, layout, times and security of the
    // upper directory of oldName, or of lowerSource when the upper has
    // none. A reparse-point root copies as a link. On failure it tries to
    // remove the upper path of newName.
    NTSTATUS CopyMergedDirectory(const ResolvedPath& lowerSource,
                                 const RenamedName& oldName,
                                 const RenamedName& newName);

    NTSTATUS CopyMergedLink(const ResolvedPath& lowerSource,
                            const RenamedName& oldName,
                            const RenamedName& newName);

    NTSTATUS CopyMergedDirectoryTree(const ResolvedPath& lowerSource,
                                     const RenamedName& oldName,
                                     const RenamedName& newName);

    // Copies each entry of oldDir, a merge of a directory, into the
    // directory at newUpperPath, at every depth. The merge applies the
    // whiteouts and opaque markers of every layer, so the copy holds no
    // marker files, and newUpperPath or a directory above it must be opaque.
    // A directory that the merge cannot list fails the copy. On failure,
    // newUpperPath holds a partial tree, and the caller removes it.
    NTSTATUS CopyMergedChildren(const MergedDirectoryWithAncestry& oldDir,
                                const std::wstring& newUpperPath);

    // Copies entry, which the merge oldParent lists, from the layer that
    // gives it into newParentUpperPath. The copy keeps the name that layer
    // gives the entry. An upper entry keeps its copy-up record, and a lower
    // entry gets a new one. A link copies as a link.
    NTSTATUS CopyMergedEntry(const MergedDirectoryWithAncestry& oldParent,
                             const MergedEntry& entry,
                             const std::wstring& newParentUpperPath);

    // Copies the upper entries of the directory at oldUpperPath into the
    // copy of a lower root that is a directory reparse point but not a link,
    // at newUpperPath. A whiteout removes the entry it hides, unless the old
    // upper also holds an entry of that name. oldRelativePath is
    // oldUpperPath relative to the upper root. On failure, newUpperPath
    // holds a partial tree, and the caller removes it.
    NTSTATUS OverlayUpperShadow(const std::wstring& oldRelativePath,
                                const std::wstring& oldUpperPath,
                                const std::wstring& newUpperPath);

    // Removes from the copy at newUpperPath the entry that the upper
    // whiteout whiteoutName hides, unless the old upper at oldUpperPath
    // also holds an entry of that name.
    NTSTATUS RemoveEntryHiddenByUpperWhiteout(const std::wstring& oldUpperPath,
                                              const std::wstring& newUpperPath,
                                              const std::wstring& whiteoutName);

    // Whether the upper entry at relativePath, with attributes upperAttrs,
    // merges into the copy at copyPath. It does when both are directories
    // that are not links, the upper one is not opaque, and no upper
    // whiteout hides relativePath. Otherwise the upper entry replaces the
    // copy's entry.
    bool UpperEntryMergesIntoCopy(const std::wstring& relativePath,
                                  DWORD upperAttrs,
                                  const std::wstring& copyPath) const;

    // Copies the upper entry at srcAbs, whose attributes are srcAttrs, to
    // dstAbs: a link as a link, a file with its data and streams, and a
    // directory with its whole tree. Each copy keeps its copy-up record. The
    // copy drops whiteout and opaque marker files, so dstAbs or a directory
    // above it must be opaque.
    NTSTATUS CopyTreeWithoutMarkers(const std::wstring& srcAbs,
                                    DWORD srcAttrs,
                                    const std::wstring& dstAbs);

    const LayerConfig& config_;
    PathResolver& pathResolver_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
    CopyUp& copyUp_;
    ::LayerMount::abi::CapabilityGate capabilities_;
};

}
