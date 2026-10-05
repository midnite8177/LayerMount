#pragma once

#include "LayerMount.h"

#include <optional>
#include <string_view>

namespace LayerMount {

class Cache;

namespace abi { class EventEmitter; }

enum class WhiteoutType {
    None,
    File,
    Directory,
    Opaque
};

class WhiteoutManager {
public:
    // A null cache turns off cache invalidation.
    explicit WhiteoutManager(ConfigRef config, Cache* cache);

    // A null emitter turns off event emission.
    void SetEventEmitter(::LayerMount::abi::EventEmitter* events) noexcept {
        events_ = events;
    }

    // True when fileName starts with the .wh. prefix in any case, because
    // NTFS matches a marker name case-insensitively.
    static bool IsWhiteoutName(std::wstring_view fileName);

    // Returns the name that a whiteout file hides, which is whiteoutName
    // without its .wh. prefix. whiteoutName must pass IsWhiteoutName.
    static std::wstring GetWhitedOutName(const std::wstring& whiteoutName);

    // Returns the name that a directory entry hides when the entry is a
    // whiteout file. Returns nullopt for any other entry and for the opaque
    // marker.
    static std::optional<std::wstring> WhitedOutNameOfEntry(const std::wstring& entryName);

    // Does a .wh.<name> marker exist for relativePath in the given layer?
    bool HasWhiteout(const std::wstring& relativePath,
                     const std::wstring& layerPath) const;

    bool HasWhiteoutInAnyLayer(const std::wstring& relativePath) const;

    // Both functions change the upper layer only. CreateWhiteout returns
    // the error of the marker write that failed as an NTSTATUS.
    NTSTATUS CreateWhiteout(const std::wstring& relativePath, WhiteoutType type);
    bool RemoveWhiteout(const std::wstring& relativePath);

    // IsOpaqueInLayer for the upper layer.
    bool IsOpaque(const std::wstring& dirRelativePath) const;

    // Whether the directory holds an opaque marker in the layer: the opaque
    // metadata or the .wh..wh..opq file. A link, which is a junction or a
    // directory symbolic link, is never opaque, as overlayfs gives a symlink
    // no opaque xattr. A directory under a link, or on a path with a
    // component that the walk cannot read, is not opaque either. When the
    // directory holds a marker, the call walks the path from the layer root
    // as FindLinkOnPath does.
    bool IsOpaqueInLayer(const std::wstring& dirRelativePath,
                         const std::wstring& layerPath) const;

    // IsOpaqueInLayer for a directory whose path the caller walked from the
    // layer root and found only plain directories on. Reads the markers and
    // does not walk again.
    bool IsOpaqueWalkedDirectoryInLayer(const std::wstring& dirRelativePath,
                                        const std::wstring& layerPath) const;

    // Writes both opaque markers in the upper layer. Succeeds when either
    // write does. When both fail, returns the error of the marker file
    // create as an NTSTATUS. The directory keeps its times; SetOpaque
    // ignores a failure to restore them. Overlayfs keeps opacity in an
    // extended attribute, which does not change mtime. When the upper holds
    // a link at the directory or at an ancestor, SetOpaque returns
    // STATUS_NOT_A_DIRECTORY and writes nothing, so no marker, directory or
    // time write goes through the link into its target. When the walk
    // cannot read a component of the path, SetOpaque returns
    // STATUS_ACCESS_DENIED, as overlayfs fails a path it cannot search.
    NTSTATUS SetOpaque(const std::wstring& dirRelativePath);

    // Writes both opaque markers on the directory at dirFullPath, as
    // SetOpaque does. The directory must exist and must not be under a
    // link. Returns STATUS_NOT_A_DIRECTORY when the directory is a link, and
    // the error of the reparse tag read when it cannot read the tag. Makes no
    // directory and invalidates no cache entry.
    NTSTATUS SetOpaqueAtPath(const std::wstring& dirFullPath);

    // Removes both opaque markers in the upper layer. On a link, removes
    // only the link's own opaque metadata and keeps the marker file in its
    // target. Under a link, or when the walk cannot read a component of the
    // path, removes nothing and returns true.
    bool RemoveOpaque(const std::wstring& dirRelativePath);

    // Whether the directory, an ancestor of it, or the layer root is opaque in
    // the layer. A marker at a link or under one does not count, as
    // IsOpaqueInLayer reads it. dirRelativePath is relative to the layer root,
    // and an empty dirRelativePath is the root.
    bool HasOpaqueSelfOrAncestorInLayer(const std::wstring& dirRelativePath,
                                        const std::wstring& layerPath) const;

    // Whether an ancestor of relativePath, not counting the layer root, has a
    // whiteout marker in the layer. A whiteout at a directory path hides every
    // descendant from that layer downward.
    bool HasWhitedOutAncestorInLayer(const std::wstring& relativePath,
                                     const std::wstring& layerPath) const;

    // Build the whiteout marker filename for a relative path: parent\.wh.<name>
    static std::wstring GetWhiteoutFileName(const std::wstring& relativePath);

    static std::wstring GetWhiteoutFullPath(const std::wstring& layerPath,
                                            const std::wstring& relativePath);

private:
    bool HoldsOpaqueMarker(const std::wstring& dirFullPath) const;
    NTSTATUS WriteOpaqueMarkers(const std::wstring& dirFullPath);

    const LayerConfig& config_;
    Cache* cache_;
    ::LayerMount::abi::EventEmitter* events_ = nullptr;
};

}
