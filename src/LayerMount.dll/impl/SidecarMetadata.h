#pragma once

// Sidecar JSON metadata store -- the !LM_CAP_ADS fallback for overlay
// per-file metadata (opaque marker, metacopy state, redirect target,
// copy-up timestamp) on filesystems that don't support NTFS Alternate
// Data Streams.
//
// Layout under the upper layer:
//
//     <upper>\.overlay\<sha1(lowercased path)>.meta.json     (per-file metadata)
//     <upper>\.overlay\<sha1(lowercased path)>.opaque        (opaque dir marker)
//
// Path keying uses SHA-1 of the lowercased absolute upper path so the
// scheme survives filenames containing characters NTFS would reject in a
// flat namespace (`<`, `>`, `:`, etc.) and tolerates very long paths.
// A record is found only when filePath has the same form, plain or \\?\,
// at the write and at the read.

#include "HostPath.h"
#include "LayerMount.h"

#include <optional>

namespace LayerMount {

class SidecarMetadata {
public:
    explicit SidecarMetadata(const HostPath& upperRoot);

    // Read the sidecar JSON for `filePath` (an absolute path under the upper
    // root). Returns default-constructed `LayerMountMetadata` if the
    // sidecar is absent or unreadable; never throws.
    LayerMountMetadata Read(const std::wstring& filePath) const;

    // Write the sidecar JSON. Creates `<upper>\.overlay\` if it does not
    // exist. Returns false on I/O failure.
    bool Write(const std::wstring& filePath, const LayerMountMetadata& metadata) const;

    // Delete the per-file sidecar. Returns true on success or absent.
    bool Remove(const std::wstring& filePath) const;

    // Moves the per-file sidecar and the opaque marker of fromPath to the
    // names of toPath. For each kind that fromPath lacks, deletes toPath's,
    // so an entry moved onto toPath never takes the record of the entry that
    // was there. Two paths that differ only in case share a sidecar, which
    // stays. A failed move or delete returns its error. When the opaque
    // marker cannot move after the per-file sidecar moved, the sidecar moves
    // back to fromPath. If that move back fails, the sidecar stays at toPath.
    // A failed call can still have deleted the record at toPath.
    NTSTATUS Move(const std::wstring& fromPath, const std::wstring& toPath) const;

    bool HasRecord(const std::wstring& filePath) const;

    bool HasOpaque(const std::wstring& dirPath) const;

    bool SetOpaque(const std::wstring& dirPath) const;

    bool RemoveOpaque(const std::wstring& dirPath) const;

private:
    std::optional<std::wstring> BaseOf(const std::wstring& entryPath) const;
    std::optional<std::wstring> FileOf(const std::wstring& entryPath,
                                       const wchar_t* suffix) const;
    bool EnsureDirectory() const;

    std::wstring win32Directory_;
};

}
