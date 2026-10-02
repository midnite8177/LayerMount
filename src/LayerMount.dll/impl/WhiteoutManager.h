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

    // Both functions change the upper layer only.
    bool CreateWhiteout(const std::wstring& relativePath, WhiteoutType type);
    bool RemoveWhiteout(const std::wstring& relativePath);

    // Check if directory is opaque in the upper layer (ADS marker or .wh..wh..opq)
    bool IsOpaque(const std::wstring& dirRelativePath) const;

    bool IsOpaqueInLayer(const std::wstring& dirRelativePath,
                         const std::wstring& layerPath) const;

    bool SetOpaque(const std::wstring& dirRelativePath);
    bool RemoveOpaque(const std::wstring& dirRelativePath);

    // Whether the directory, an ancestor of it, or the layer root is opaque in
    // the layer. dirRelativePath is relative to the layer root, and an empty
    // dirRelativePath is the root.
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
    const LayerConfig& config_;
    Cache* cache_;
    ::LayerMount::abi::EventEmitter* events_ = nullptr;
};

}
