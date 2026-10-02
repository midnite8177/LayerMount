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

// Sets *isLink to whether the entry at path is a junction or a directory
// symbolic link. Such a link is a directory reparse point whose reparse
// tag is a name surrogate. Any other directory reparse point is not a
// link. attributes are the entry's own, as GetFileAttributesW reports them
// without following a link. Reads the tag only for a directory reparse
// point. When the tag cannot be read, returns that error and leaves
// *isLink false. INVALID_FILE_ATTRIBUTES returns STATUS_INVALID_PARAMETER.
NTSTATUS IsDirectoryLink(const std::wstring& path, DWORD attributes, bool* isLink);

enum class ReplaceExisting { No, Yes };

enum class CopyAcrossVolumes { No, Yes };

// Renames an upper file or directory. A junction or a symbolic link moves
// as a link, and its target stays. With CopyAcrossVolumes::Yes, a file
// whose destination is on another volume moves as a copy and a delete. A
// directory, a junction or a directory symbolic link cannot move to
// another volume. An inherited deny-write ACE on the upper parent fails
// the rename with STATUS_ACCESS_DENIED unless the process holds
// SE_RESTORE_NAME.
NTSTATUS MoveUpperEntry(const std::wstring& from,
                        const std::wstring& to,
                        ReplaceExisting replace,
                        CopyAcrossVolumes copy);

// Removes the upper entry at path. A directory goes with its whole tree.
// A junction or a symbolic link goes, and its target stays. A path that
// does not exist is success.
NTSTATUS RemoveUpperEntry(const std::wstring& path);

// Creates a directory at path. A directory already there is success. Any
// other entry there returns STATUS_OBJECT_NAME_COLLISION, so no caller
// goes on to treat a file as a directory.
NTSTATUS CreateDirectoryOrUseExisting(const std::wstring& path);

// Returns the FindFirstFileW search pattern for dirRelativePath in the
// layer at layerPath. The pattern never holds a doubled separator.
std::wstring JoinLayerScanPath(const std::wstring& layerPath,
                               const std::wstring& dirRelativePath);

// Walks dirRelativePath from the layer root and returns true at the first
// component that is a non-directory. A component that does not exist ends
// the walk with false. Any other failure to read a component's attributes
// returns true, so a layer that the walk cannot read hides what is below
// it. Costs one GetFileAttributesW per component.
bool HasNonDirectorySelfOrAncestorInLayer(const std::wstring& layerPath,
                                          const std::wstring& dirRelativePath);

// A directory in one layer. dirNorm is normalized and relative to the layer
// root; an empty dirNorm is the root.
struct LayerDirectory {
    const WhiteoutManager& whiteoutMgr;
    const std::wstring& layerPath;
    const std::wstring& dirNorm;
};

// Whether a probe or a scan of the layer found the directory as a directory.
enum class DirectoryProbe {
    Found,
    Missed,
};

enum class LowerVisibility {
    Visible,
    HiddenByOpaqueMarker,
    HiddenByNonDirectory,
};

// Whether the lowers below the layer can hold entries under the directory.
// An opaque marker at the directory or at an ancestor in the layer, the
// layer root included, hides them, and so does a non-directory there. A
// directory the layer was found to hold has no non-directory at its path or
// above, so only a miss walks the path.
LowerVisibility LowersBelow(const LayerDirectory& dir, DirectoryProbe probe);

}
