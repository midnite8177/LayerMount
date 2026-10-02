#pragma once

#include "LayerMount.h"

namespace LayerMount {

// LayerMount metadata accessor. Dispatches between two stores under the
// hood:
//   * NTFS Alternate Data Streams (`:overlay`, `:overlay.opaque`) when
//     the host advertises LM_CAP_ADS (the optimized path).
//   * SidecarMetadata JSON files under `<upper>\.overlay\` when ADS is
//     unavailable (the !LM_CAP_ADS fallback).
//
// Each method takes a `const LayerConfig*`. A null config means
// ADS-only, so the method touches the `:overlay` streams and nothing else.
// With a config, a read falls through to the sidecar when ADS has
// nothing, and a write picks the store from `LM_CAP_ADS`. A remove
// cleans both stores so a host can change capabilities and leave no
// stale entry behind.
class MetadataStore {
public:
    // Read `:overlay` (then sidecar if config present and ADS empty).
    // Returns default `LayerMountMetadata` if neither store has it, and
    // also when the stream exists but the read or the parse fails.
    // A read does not change the file's last access time, unless the
    // volume or an ACL refuses the marked open and the read falls back
    // to a plain open.
    static LayerMountMetadata ReadLayerMountMetadata(
        const std::wstring& filePath,
        const LayerConfig* config);

    // Write the metadata. With `config` and `!LM_CAP_ADS`: writes the
    // sidecar. Otherwise: writes the `:overlay` ADS.
    static bool WriteLayerMountMetadata(
        const std::wstring& filePath,
        const LayerMountMetadata& metadata,
        const LayerConfig* config);

    // Delete from both stores when `config` is provided (best-effort);
    // ADS-only otherwise.
    static bool RemoveLayerMountMetadata(
        const std::wstring& filePath,
        const LayerConfig* config);

    // True when the `:overlay.opaque` stream exists, or, with a config, the
    // sidecar `.opaque` file. Ignores the `.wh..wh..opq` marker file.
    static bool HasOpaqueMetadata(
        const std::wstring& directoryPath,
        const LayerConfig* config);

    // Writes the sidecar `.opaque` file when a config is given and the host
    // lacks LM_CAP_ADS, otherwise the `:overlay.opaque` stream. Does not
    // create the `.wh..wh..opq` marker file.
    static bool SetOpaqueMetadata(
        const std::wstring& directoryPath,
        const LayerConfig* config);

    // Deletes the `:overlay.opaque` stream, and the sidecar `.opaque` file
    // too when a config is given. Leaves the `.wh..wh..opq` marker file alone.
    static bool RemoveOpaqueMetadata(
        const std::wstring& directoryPath,
        const LayerConfig* config);
};

}
