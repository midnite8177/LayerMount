#pragma once

// Sidecar JSON metadata store -- the !LM_CAP_ADS fallback for overlay
// per-file metadata (opaque marker, metacopy state, redirect target,
// copy-up timestamp) on filesystems that don't support NTFS Alternate
// Data Streams.
//
// Layout under the upper layer:
//
//     <upper>\.overlay\<sha1(relativePath)>.meta.json     (per-file metadata)
//     <upper>\.overlay\<sha1(relativePath)>.opaque        (opaque dir marker)
//
// Path keying uses SHA-1 of the lowercased absolute upper path so the
// scheme survives filenames containing characters NTFS would reject in a
// flat namespace (`<`, `>`, `:`, etc.) and tolerates very long paths.
//
// Stateless: every method takes the file/dir path under the upper layer
// plus the upper root so we can compute the sidecar location. The
// dispatcher in MetadataADS picks between ADS and sidecar; this module
// just owns the sidecar I/O shape.

#include "LayerMount.h"

namespace LayerMount {

class SidecarMetadata {
public:
    // Read the sidecar JSON for `filePath` (an absolute path under
    // `upperRoot`). Returns default-constructed `LayerMountMetadata` if the
    // sidecar is absent or unreadable; never throws.
    //
    // If `corrupted` is non-null it is set to true when a sidecar appeared
    // to exist but could not be read or parsed. Callers whose correctness
    // depends on metadata fidelity should treat `*corrupted == true` as a
    // failure instead of accepting the default return value.
    static LayerMountMetadata Read(const std::wstring& filePath,
                                const std::wstring& upperRoot,
                                bool* corrupted = nullptr);

    // Write the sidecar JSON. Creates `<upper>\.overlay\` if it does not
    // exist. Returns false on I/O failure.
    static bool Write(const std::wstring& filePath,
                      const LayerMountMetadata& metadata,
                      const std::wstring& upperRoot);

    // Delete the per-file sidecar. Returns true on success or absent.
    static bool Remove(const std::wstring& filePath,
                       const std::wstring& upperRoot);

    // Opaque-marker variants. Marker is a separate zero-byte file
    // (`<sha1(dirPath)>.opaque`) so detection is a single GetFileAttributes.
    static bool HasOpaque(const std::wstring& dirPath,
                          const std::wstring& upperRoot);

    static bool SetOpaque(const std::wstring& dirPath,
                          const std::wstring& upperRoot);

    static bool RemoveOpaque(const std::wstring& dirPath,
                             const std::wstring& upperRoot);
};

} // namespace LayerMount
