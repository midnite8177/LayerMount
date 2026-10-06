#pragma once

#include "LayerMount.h"

#include <functional>
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

// Returns name in lowercase. The merged view keys its entries and its
// whiteout names with this fold.
std::wstring CaseFoldedName(const std::wstring& name);

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

// Sets *kind to the kind of the entry at path: File for a non-directory,
// Link for a link as IsDirectoryLink defines it, and Directory for any other
// directory. attributes are the entry's own, as IsDirectoryLink takes them.
// When the reparse tag cannot be read, returns that error and leaves *kind
// unchanged.
NTSTATUS EntryKindOf(const std::wstring& path, DWORD attributes, EntryKind* kind);

// Sets *clones to whether a copy of the entry at path clones its reparse
// point. An entry whose reparse tag is a name surrogate clones, such as a
// file symbolic link or a link as IsDirectoryLink defines it. A file with
// the tag of a WSL special file or of an app execution alias also clones.
// Any other entry, such as a cloud placeholder file or a deduplicated file,
// copies as a plain file with its data or as a plain directory. attributes
// are the entry's own, as IsDirectoryLink takes them. Reads the tag only
// for a reparse point. When the tag cannot be read, returns that error and
// leaves *clones false. INVALID_FILE_ATTRIBUTES returns
// STATUS_INVALID_PARAMETER.
NTSTATUS ClonesReparsePoint(const std::wstring& path, DWORD attributes, bool* clones);

// What the reparse point of an entry is to a lookup in the merged view.
// RelativeSymlink is a symbolic link with SYMLINK_FLAG_RELATIVE, which
// resolves in the view. OtherLink is any other name surrogate, such as a
// junction or an absolute symbolic link, which Win32 follows on the
// physical path. None is a reparse point that is not a link.
enum class ReparseLink { None, RelativeSymlink, OtherLink };

// Reads the reparse data of the entry at path, not of its target. For a
// RelativeSymlink, sets *relativeTarget to the link's substitute name. A
// relative target that holds a colon gives OtherLink, because a
// drive-relative target or a stream name has no view path. A reparse
// point whose data cannot be read also gives OtherLink. The lookup leaves
// both to Win32.
ReparseLink ReadReparseLink(const std::wstring& path, std::wstring* relativeTarget);

enum class ReplaceExisting { No, Yes };

// The result of MoveUpperEntry. status is the result of the move.
// recordLeftBehind is the error of the first sidecar record that the call
// left at the key of a path where the entry is not, or STATUS_SUCCESS.
struct UpperEntryMove {
    NTSTATUS status;
    NTSTATUS recordLeftBehind;
};

// Renames an upper file or directory within its volume. A junction or a
// symbolic link moves as a link, and its target stays. With
// ReplaceExisting::No, an entry at `to` fails the rename with
// STATUS_OBJECT_NAME_COLLISION and stays as it was. An inherited deny-write
// ACE on the upper parent fails the rename with STATUS_ACCESS_DENIED unless
// the process holds SE_RESTORE_NAME. The sidecar records of the moved
// entries follow them; see MetadataStore::MoveSidecarRecords. When a record
// cannot move, the entry moves back to `from`, the records that moved go
// back, and the status is the record error. If the entry cannot move back,
// it stays at `to`, the records that can move follow it, and the status is
// STATUS_SUCCESS, because a failed status tells the caller that the entry
// is still at `from`. In both cases, a record that stays at the key of the
// other path sets recordLeftBehind.
// Warning: the undo cannot restore an entry that a move with
// ReplaceExisting::Yes replaced. With ReplaceExisting::Yes, `to` must hold
// no entry.
UpperEntryMove MoveUpperEntry(const std::wstring& from,
                              const std::wstring& to,
                              ReplaceExisting replace,
                              const LayerConfig& config);

// Moves the upper entry as MoveUpperEntry does, but a sidecar record that
// cannot move neither stops nor undoes the move. That record stays at the
// key of its old path, and the other records move. The status is the error
// of the move on disk only. The first record that stays sets
// recordLeftBehind. With ReplaceExisting::Yes, the move replaces a file at
// `to` in one step.
UpperEntryMove MoveUpperEntryLeavingStuckRecords(const std::wstring& from,
                                                 const std::wstring& to,
                                                 ReplaceExisting replace,
                                                 const LayerConfig& config);

// Moves the upper entry at path, as MoveUpperEntry does, to the new path
// that newAsidePath gives, and sets *asidePath to that path. The new path
// must be on the volume of the entry. When no entry is at path, does not
// call newAsidePath, sets *asidePath empty and returns STATUS_SUCCESS. A
// failed probe or move returns its error, leaves the entry at path, and
// sets *asidePath empty.
UpperEntryMove MoveUpperEntryAside(const std::wstring& path,
                                   const std::function<std::wstring()>& newAsidePath,
                                   const LayerConfig& config,
                                   std::wstring* asidePath);

// Sets *kind to the kind of the upper entry at path, as EntryKindOf reads
// it, and sets *exists to true. When no entry is at path, also when the
// entry goes between the attribute read and the tag read, sets *exists to
// false and returns success. Returns the error of a failed attribute or
// tag read.
NTSTATUS ProbeUpperEntry(const std::wstring& path, bool* exists, EntryKind* kind);

// Removes the upper entry at path, whose kind ProbeUpperEntry read. A
// directory goes with its whole tree, also when it is a reparse point that
// is not a link, such as a cloud placeholder. A junction or a symbolic link
// goes, and its target stays. An entry that is already gone is success.
// The sidecar records of the removed entries also go.
NTSTATUS RemoveUpperEntryOfKind(const std::wstring& path,
                                EntryKind kind,
                                const LayerConfig& config);

// Probes the upper entry at path with ProbeUpperEntry, then removes it with
// RemoveUpperEntryOfKind. A path with no entry is success. A failed probe
// returns its error and removes nothing.
NTSTATUS RemoveUpperEntry(const std::wstring& path, const LayerConfig& config);

// Creates a container directory at containerPath, a new path in the work
// directory, and calls build with the path of an entry in the container.
// The container gets the ACEs of the parent of upperPath through
// WriteSecurityToInheritAs. A move does not recompute inherited ACEs, so
// the entry that build makes inherits there as it does at upperPath. Then the entry moves to upperPath with
// ReplaceExisting::No. An entry at upperPath fails the move with
// STATUS_OBJECT_NAME_COLLISION and stays as it was. The call removes the
// container after a success and after a failure. Returns the first failure.
NTSTATUS BuildInContainerAndMove(const std::wstring& containerPath,
                                 const std::wstring& upperPath,
                                 const LayerConfig& config,
                                 const std::function<NTSTATUS(const std::wstring&)>& build);

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

// Where a walk of a path from the layer root meets a link. Unreadable is a
// component whose attributes or reparse tag the walk cannot read, so it can
// be a link.
enum class LinkOnPath { None, Self, Ancestor, Unreadable };

// Walks dirRelativePath from the layer root as
// HasNonDirectoryOrLinkSelfOrAncestorInLayer does. A walk that stops at a
// non-directory, or at a missing component, returns LinkOnPath::None.
// dirRelativePath must be normalized, so that a link at its last component
// returns LinkOnPath::Self.
LinkOnPath FindLinkOnPath(const std::wstring& layerPath, const std::wstring& dirRelativePath);

// Where a walk of a path from the layer root stops: at the first component
// that is a file, a link or an unreadable entry. None is a walk that meets
// only directories, or that ends at a missing component.
enum class WalkStop { None, File, Link, Unreadable };

// What a walk says about links on a path. Unreadable is a component whose
// attributes or reparse tag the walk cannot read, so it can be a link. Each
// caller decides what Unreadable means for its own operation.
enum class LinkStop { None, Link, Unreadable };

// The LinkStop of a walk that stopped at stop. A walk that stops at a file,
// or at no component, gives LinkStop::None.
LinkStop LinkStopOf(WalkStop stop);

// True when LinkStopOf(stop) is not LinkStop::None, so the walk met a link
// or a component that can be one.
bool EndsAtLinkOrUnreadable(WalkStop stop);

// What a layer holds at one path. Unreadable is an entry whose attributes
// or reparse tag the engine cannot read, for a reason other than a missing
// path.
enum class ComponentKind { Missing, Directory, File, Link, Unreadable };

// Reads what the layer at layerPath holds at relativePath. A link is a link
// as IsDirectoryLink defines it. Costs one GetFileAttributesW, and one more
// open for a directory reparse point.
ComponentKind ComponentKindInLayer(const std::wstring& layerPath,
                                   const std::wstring& relativePath);

// Whether the upper or a lower above lowerIndex holds an entry at
// relativePath. A failure to read an entry's attributes, other than a
// missing path, counts as holding it, so an unreadable higher layer hides
// a lower link.
bool HigherLayerHoldsEntry(const LayerConfig& config,
                           size_t lowerIndex,
                           const std::wstring& relativePath);

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
// point. Each directory on the path that holds an opaque marker adds one
// more walk from the layer root.
LowerVisibility LowersBelow(const LayerDirectory& dir);

// What one layer holds at a directory and at its ancestors. A directory
// merge reads it to find what the layer and the layers below it add to the
// directory. LayerAncestryOf reads a whole path. StepLayerAncestry reads
// one more component, so a walk down a tree reads each component once.
struct LayerAncestry {
    // A whiteout in the layer at the directory or at an ancestor, as
    // HasWhiteout reads it. StepLayerAncestry reads the marker only in a
    // parent whose firstNonDirectory is WalkStop::None.
    bool whitedOut;
    // An opaque marker at the directory, at an ancestor or at the layer
    // root, as LowersBelow reads it. StepLayerAncestry reads the marker only
    // at a plain directory whose parent's firstNonDirectory is
    // WalkStop::None.
    bool opaque;
    // Where a walk of the directory's path from the layer root stops: at the
    // first file, link or unreadable entry at an ancestor or at the
    // directory. HasNonDirectoryOrLinkSelfOrAncestorInLayer returns true
    // when this is not WalkStop::None.
    WalkStop firstNonDirectory;
    // For a lower: the first such entry is a link at a path that a higher
    // layer also holds, as HasLinkUnderHigherLayerEntry reads it. Always
    // false for the upper.
    bool linkUnderHigherEntry;
    // The layer holds no entry at the directory, or no entry at an ancestor.
    // Nothing below the directory can then hide a lower.
    bool absent;
};

// The ancestry of dir, read from the layer root. lowerIndex is the index
// of the lower at dir.layerPath, or -1 for the upper.
LayerAncestry LayerAncestryOf(const LayerConfig& config,
                              const LayerDirectory& dir,
                              int lowerIndex);

// The ancestry of dir, from parent, the ancestry of the parent directory
// of dir in the same layer. Reads only the last component of dir.dirNorm.
// lowerIndex is as LayerAncestryOf takes it.
LayerAncestry StepLayerAncestry(const LayerConfig& config,
                                const LayerDirectory& dir,
                                int lowerIndex,
                                const LayerAncestry& parent);

// The first link on a path in the merged view, as FindLinkInView reads it.
// stop is LinkStop::Link at a link, LinkStop::Unreadable at a component
// whose attributes or reparse tag the walk cannot read, and LinkStop::None
// when the view meets neither. source is the layer that holds that
// component, or LayerSource::None, and path is the component's path
// relative to the layer root.
struct LinkInView {
    LinkStop stop;
    LayerSource source;
    std::wstring path;
};

// Walks dirNorm through the merged view from the root and returns the first
// link there: a link in the upper, or a link in the first lower that the
// view reaches at that path when no higher layer holds the link's name. A
// name under a junction or an absolute symbolic link is in the link target,
// outside the overlay. dirNorm must be a view path that
// PathResolver::ViewPathThroughLinks has walked. A relative symbolic link
// still on it, which that walk kept at the last component, comes back as a
// link like any other. A whiteout, an opaque marker or a file that hides a
// lower from the path also hides that lower's links, as in a directory
// merge. dirNorm is normalized and relative to the layer root; an empty
// dirNorm is the root. Walks the path in the upper, and, when that walk
// stops at no file, link or unreadable component, in each lower. When a
// lower walk stops at a link or an unreadable component, adds a
// LayerAncestryOf read in the upper and in each lower that the view
// reaches.
LinkInView FindLinkInView(const LayerConfig& config,
                          const WhiteoutManager& whiteoutMgr,
                          const std::wstring& dirNorm);

// Returns FindLinkInView of the parent of normalizedPath, which is the
// first link in the merged view above the entry at normalizedPath. An
// entry at the root has none.
LinkInView FindLinkAbove(const LayerConfig& config,
                         const WhiteoutManager& whiteoutMgr,
                         const std::wstring& normalizedPath);

}
