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
// Each method takes an optional `const LayerConfig*`. When nullptr, the
// methods behave as ADS-only -- this preserves the test surface and any
// caller that hasn't been taught about the gate. When provided, reads
// transparently fall through to the sidecar if ADS yields nothing, and
// writes pick the store based on `LM_CAP_ADS`. Removes clean both
// stores so a host can flip
// capabilities without leaving stale entries behind.
class MetadataADS {
public:
    // Read `:overlay` (then sidecar if config present and ADS empty).
    // Returns default `LayerMountMetadata` if neither store has it.
    //
    // If `corrupted` is non-null it is set to true when a metadata stream
    // appeared to exist but could not be read or parsed (sharing/ACL/JSON
    // failure). Callers whose resolution correctness depends on metadata
    // fidelity should treat `*corrupted == true` as a failure rather than
    // accepting the default return value. A legitimately-absent stream
    // leaves `*corrupted == false`.
    static LayerMountMetadata ReadLayerMountMetadata(
        const std::wstring& filePath,
        const LayerConfig* config = nullptr,
        bool* corrupted = nullptr);

    // Write the metadata. With `config` and `!LM_CAP_ADS`: writes the
    // sidecar. Otherwise: writes the `:overlay` ADS.
    static bool WriteLayerMountMetadata(
        const std::wstring& filePath,
        const LayerMountMetadata& metadata,
        const LayerConfig* config = nullptr);

    // Delete from both stores when `config` is provided (best-effort);
    // ADS-only otherwise.
    static bool RemoveLayerMountMetadata(
        const std::wstring& filePath,
        const LayerConfig* config = nullptr);

    // Check `:overlay.opaque` (then sidecar opaque marker if config given).
    static bool HasOpaqueADS(
        const std::wstring& directoryPath,
        const LayerConfig* config = nullptr);

    // Set the opaque marker. Routing same as Write.
    static bool SetOpaqueADS(
        const std::wstring& directoryPath,
        const LayerConfig* config = nullptr);

    // Remove the opaque marker from both stores when config is given.
    static bool RemoveOpaqueADS(
        const std::wstring& directoryPath,
        const LayerConfig* config = nullptr);
};

} // namespace LayerMount
