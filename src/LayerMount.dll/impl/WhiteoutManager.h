#pragma once

#include "LayerMount.h"

#include <optional>
#include <string_view>
#include <vector>

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
    explicit WhiteoutManager(const LayerConfig& config, Cache* cache);

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

    // Check all layers (upper first, then lowers in order)
    bool HasWhiteoutInAnyLayer(const std::wstring& relativePath) const;

    // Both functions change the upper layer only.
    bool CreateWhiteout(const std::wstring& relativePath, WhiteoutType type);
    bool RemoveWhiteout(const std::wstring& relativePath);

    // Check if directory is opaque in the upper layer (ADS marker or .wh..wh..opq)
    bool IsOpaque(const std::wstring& dirRelativePath) const;

    // Check if directory is opaque in a specific layer
    bool IsOpaqueInLayer(const std::wstring& dirRelativePath,
                         const std::wstring& layerPath) const;

    bool SetOpaque(const std::wstring& dirRelativePath);
    bool RemoveOpaque(const std::wstring& dirRelativePath);

    // Walk ancestors upward; return true if any ancestor is opaque in upper layer
    bool HasOpaqueAncestor(const std::wstring& relativePath) const;

    // Walk ancestors upward; return true if any ancestor is opaque in a specific layer
    bool HasOpaqueAncestorInLayer(const std::wstring& relativePath,
                                  const std::wstring& layerPath) const;

    bool HasOpaqueSelfOrAncestorInLayer(const std::wstring& dirRelativePath,
                                        const std::wstring& layerPath) const;

    // Walk ancestors upward; return true if any ancestor has a whiteout marker in
    // the given layer. A whiteout at a directory path hides every descendant from
    // that layer downward — callers in the resolver use this to stop surfacing
    // lower-layer content beneath a deleted directory.
    bool HasWhitedOutAncestorInLayer(const std::wstring& relativePath,
                                     const std::wstring& layerPath) const;

    // Returns the names that the whiteouts in one layer's directory hide,
    // without the .wh. prefix, in directory order. A directory missing from
    // the layer gives an empty list. A failed scan gives nullopt, because a
    // partial list would show entries that the layer deleted. The list does
    // not include the opaque marker.
    std::optional<std::vector<std::wstring>> ListWhitedOutNames(
        const std::wstring& dirRelativePath,
        const std::wstring& layerPath) const;

    // Build the whiteout marker filename for a relative path: parent\.wh.<name>
    static std::wstring GetWhiteoutFileName(const std::wstring& relativePath);

    // Build the full absolute whiteout path within a layer
    static std::wstring GetWhiteoutFullPath(const std::wstring& layerPath,
                                            const std::wstring& relativePath);

private:
    const LayerConfig& config_;
    Cache* cache_;
    ::LayerMount::abi::EventEmitter* events_ = nullptr;
};

// Returns the FindFirstFileW search pattern for dirRelativePath in the
// layer at layerPath. An empty dirRelativePath gets no extra separator,
// because FindFirstFileW refuses a doubled separator at the root of an
// extended-form (\\?\) path, though a plain path accepts one.
std::wstring JoinLayerScanPath(const std::wstring& layerPath,
                               const std::wstring& dirRelativePath);

} // namespace LayerMount
