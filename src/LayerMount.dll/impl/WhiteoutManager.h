#pragma once

#include "LayerMount.h"

namespace LayerMount {

// Forward declaration — full definition in Cache.h
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
    // cache may be nullptr (for isolated testing or when invalidation is not needed)
    explicit WhiteoutManager(const LayerConfig& config, Cache* cache = nullptr);

    // Event emitter wired post-construction by LayerMount. Unset ->
    // emission is a no-op (matches existing test callers that construct
    // WhiteoutManager directly).
    void SetEventEmitter(::LayerMount::abi::EventEmitter* events) noexcept {
        events_ = events;
    }

    // --- Whiteout detection ---

    // Does this filename have the .wh. prefix?
    static bool IsWhiteoutName(const std::wstring& fileName);

    // Does a .wh.<name> marker exist for relativePath in the given layer?
    bool HasWhiteout(const std::wstring& relativePath,
                     const std::wstring& layerPath) const;

    // Check all layers (upper first, then lowers in order)
    bool HasWhiteoutInAnyLayer(const std::wstring& relativePath) const;

    // --- Whiteout creation/removal (upper layer only) ---

    bool CreateWhiteout(const std::wstring& relativePath,
                        WhiteoutType type = WhiteoutType::File);
    bool RemoveWhiteout(const std::wstring& relativePath);

    // --- Opaque directory support ---

    // Check if directory is opaque in the upper layer (ADS marker or .wh..wh..opq)
    bool IsOpaque(const std::wstring& dirRelativePath) const;

    // Check if directory is opaque in a specific layer
    bool IsOpaqueInLayer(const std::wstring& dirRelativePath,
                         const std::wstring& layerPath) const;

    bool SetOpaque(const std::wstring& dirRelativePath);
    bool RemoveOpaque(const std::wstring& dirRelativePath);

    // --- Opaque inheritance ---

    // Walk ancestors upward; return true if any ancestor is opaque in upper layer
    bool HasOpaqueAncestor(const std::wstring& relativePath) const;

    // Walk ancestors upward; return true if any ancestor is opaque in a specific layer
    bool HasOpaqueAncestorInLayer(const std::wstring& relativePath,
                                  const std::wstring& layerPath) const;

    // Walk ancestors upward; return true if any ancestor has a whiteout marker in
    // the given layer. A whiteout at a directory path hides every descendant from
    // that layer downward — callers in the resolver use this to stop surfacing
    // lower-layer content beneath a deleted directory.
    bool HasWhitedOutAncestorInLayer(const std::wstring& relativePath,
                                     const std::wstring& layerPath) const;

    // --- Directory enumeration support ---

    // List all whiteout-hidden filenames in a directory within a given layer.
    // Returns original names (with .wh. prefix stripped).
    // On success (enumeration reached ERROR_NO_MORE_FILES) writes true to
    // *ok if ok != nullptr. A mid-enumeration FindNextFileW failure (sharing
    // violation, network error, etc.) leaves *ok == false so callers can
    // fail directory merging instead of using a partial whiteout list that
    // would expose already-deleted lower entries.
    std::vector<std::wstring> ListWhiteoutsInDirectory(
        const std::wstring& dirRelativePath,
        const std::wstring& layerPath,
        bool* ok = nullptr) const;

    // --- Utility ---

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

} // namespace LayerMount
