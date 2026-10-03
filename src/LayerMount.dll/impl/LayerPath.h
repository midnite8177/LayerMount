#pragma once

#include "LayerMount.h"

#include <string>
#include <string_view>
#include <utility>

namespace LayerMount {

class WhiteoutManager;

// Returns dirPath with one trailing separator. A dirPath that already ends
// in one, such as a drive root or a shadow-copy device root, gets no second
// one. Windows does not normalize an extended-form (\\?\) path, so
// FindFirstFileW, GetFileAttributesW and CreateFileW refuse a doubled
// separator in one, though a plain path accepts it.
std::wstring DirWithSeparator(const std::wstring& dirPath);

// Returns relativePath under dirPath, with one separator between them.
// relativePath must not start with a separator. An empty relativePath
// gives DirWithSeparator(dirPath).
std::wstring JoinDirPath(const std::wstring& dirPath,
                         const std::wstring& relativePath);

// A path in the case the caller wrote it. A normalized path is lowercase,
// so the constructor is explicit: a call site that holds only a normalized
// path has to name the conversion.
class CallerPath {
public:
    explicit CallerPath(std::wstring text) : text_(std::move(text)) {}

    const std::wstring& Text() const noexcept { return text_; }

private:
    std::wstring text_;
};

// Returns path with backslash separators and no leading or trailing
// separator. Unlike NormalizePath, it keeps the case of path.
std::wstring NormalizePathPreserveCase(const std::wstring& path);

// Whether pathNorm names an entry below dirNorm, at a separator boundary,
// so "a\b" is inside "a" and "ab" is not. Both paths are in the same
// NormalizePath or NormalizePathPreserveCase form. A path is not inside
// itself, and an empty dirNorm holds nothing.
bool IsInsideDirectory(std::wstring_view pathNorm, std::wstring_view dirNorm);

// An empty or all-separator relativePath returns upperRoot. An entry
// created at the result keeps the caller's case.
std::wstring BuildUpperPathPreserveCase(const std::wstring& upperRoot,
                                        const std::wstring& relativePath);

// Returns targetPath with its last component replaced by entryPath's name
// as stored on disk, the name FindFirstFileW reports. Returns targetPath
// unchanged when the lookup fails. FindFirstFileW needs list access on
// entryPath's parent, and a failed lookup must not fail a copy-up.
std::wstring WithStoredLeafName(const std::wstring& targetPath,
                                const std::wstring& entryPath);

// Returns true for a directory that is not a reparse point. A scan of a
// junction or a directory symbolic link lists the entries of its target.
bool IsEnumerableDirectory(DWORD attributes);

// Sets *isLink to whether the entry at path is a junction or a directory
// symbolic link. Such a link is a directory reparse point whose reparse
// tag is a name surrogate. Any other directory reparse point is not a
// link. attributes are the entry's own, as GetFileAttributesW reports them
// without following a link. Reads the tag only for a directory reparse
// point. When the tag cannot be read, returns that error and leaves
// *isLink false. INVALID_FILE_ATTRIBUTES returns STATUS_INVALID_PARAMETER.
NTSTATUS IsDirectoryLink(const std::wstring& path, DWORD attributes, bool* isLink);

// Sets *kind to the kind of the entry at path: File for a non-directory,
// Link for a link as IsDirectoryLink defines it, and Directory for any other
// directory. attributes are the entry's own, as IsDirectoryLink takes them.
// When the reparse tag cannot be read, returns that error and leaves *kind
// unchanged.
NTSTATUS EntryKindOf(const std::wstring& path, DWORD attributes, RenameEntryKind* kind);

enum class ReplaceExisting { No, Yes };

enum class CopyAcrossVolumes { No, Yes };

// Renames an upper file or directory. A junction or a symbolic link moves
// as a link, and its target stays. With CopyAcrossVolumes::Yes, a file
// whose destination is on another volume moves as a copy and a delete. A
// directory, a junction or a directory symbolic link cannot move to
// another volume. An inherited deny-write ACE on the upper parent fails
// the rename with STATUS_ACCESS_DENIED unless the process holds
// SE_RESTORE_NAME. The sidecar records of the moved entries follow them;
// see MetadataStore::MoveSidecarRecords.
NTSTATUS MoveUpperEntry(const std::wstring& from,
                        const std::wstring& to,
                        ReplaceExisting replace,
                        CopyAcrossVolumes copy,
                        const LayerConfig& config);

// Removes the upper entry at path. A directory goes with its whole tree.
// A junction or a symbolic link goes, and its target stays. A path that
// does not exist is success. The sidecar records of the removed entries
// also go.
NTSTATUS RemoveUpperEntry(const std::wstring& path, const LayerConfig& config);

// Creates a directory at path. A directory already there is success. Any
// other entry there returns STATUS_OBJECT_NAME_COLLISION, so no caller
// goes on to treat a file as a directory.
NTSTATUS CreateDirectoryOrUseExisting(const std::wstring& path);

// Returns the FindFirstFileW search pattern for dirRelativePath in the
// layer at layerPath. The pattern never holds a doubled separator.
std::wstring JoinLayerScanPath(const std::wstring& layerPath,
                               const std::wstring& dirRelativePath);

// Walks dirRelativePath from the layer root and returns true at the first
// component that is a non-directory or a link. A link is a junction or a
// directory symbolic link, which overlayfs would see as a symlink. A
// component that does not exist ends the walk with false. Any other failure
// to read a component's attributes or reparse tag returns true, so a layer
// that the walk cannot read hides what is below it. Costs one
// GetFileAttributesW per component, and one more open for each directory
// reparse point.
bool HasNonDirectoryOrLinkSelfOrAncestorInLayer(const std::wstring& layerPath,
                                                const std::wstring& dirRelativePath);

// Whether the lower at lowerIndex holds a link at dirRelativePath or at an
// ancestor, at a path that the upper or a higher lower also holds. Overlayfs
// follows a lower symlink only when no higher layer holds its name. So such
// a lower adds nothing under dirRelativePath, and no deeper lower does
// either. Costs the walk of HasNonDirectoryOrLinkSelfOrAncestorInLayer in
// the lower, and one GetFileAttributesW per higher layer when the walk stops
// at a link.
bool HasLinkUnderHigherLayerEntry(const LayerConfig& config,
                                  size_t lowerIndex,
                                  const std::wstring& dirRelativePath);

// A directory in one layer. dirNorm is normalized and relative to the layer
// root; an empty dirNorm is the root.
struct LayerDirectory {
    const WhiteoutManager& whiteoutMgr;
    const std::wstring& layerPath;
    const std::wstring& dirNorm;
};

enum class LowerVisibility {
    Visible,
    HiddenByOpaqueMarker,
    HiddenByNonDirectoryOrLink,
};

// Whether the lowers below the layer can hold entries under the directory.
// An opaque marker at the directory or at an ancestor in the layer, the
// layer root included, hides them, and so does a non-directory or a link
// there. A scan or a probe through a link finds the link target as a
// directory, so LowersBelow walks the path even when the layer holds the
// directory. Every call walks every component of the path, with one
// GetFileAttributesW per component and one more open per directory reparse
// point.
LowerVisibility LowersBelow(const LayerDirectory& dir);

}
