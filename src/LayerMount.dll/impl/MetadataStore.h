#pragma once

#include "LayerMount.h"

#include <string>
#include <vector>

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
// nothing, and a write picks the store from `LM_CAP_ADS`.
// RemoveLayerMountMetadata cleans both stores.
//
// The streams go on the entry at the path, a link included, and no stream
// open follows a link to its target. A stream moves with its entry on a
// rename and goes with it on a delete. The sidecar store keys a record by
// the entry's path, so a rename of an upper entry calls MoveSidecarRecords
// after the move, and a delete calls ListSidecarKeyedEntries before it and
// RemoveSidecarRecordsOfGoneEntries after it.
class MetadataStore {
public:
    // Read `:overlay` (then sidecar if config present and ADS empty).
    // Returns default `LayerMountMetadata` if neither store has it, and
    // also when the stream exists but the read or the parse fails. When the
    // config's host lacks LM_CAP_ADS, such a failure falls through to the
    // sidecar.
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

    // The sidecar record and opaque marker of one entry that
    // MoveSidecarRecords moved from the key of fromPath to the key of toPath.
    struct MovedSidecarRecord {
        std::wstring fromPath;
        std::wstring toPath;
    };

    // Call after a rename moved the entry at `from` to `to`. When the
    // config's host lacks LM_CAP_ADS, moves the sidecar record and opaque
    // marker of the entry, and of each entry below it, without a walk into
    // a junction or a directory symbolic link, to the new path. The move
    // deletes a record already at a new path, so the moved entry never
    // takes the record of the entry it replaced. Adds each entry whose
    // records moved to *moved, in order. Stops at the first entry whose
    // records cannot move and returns that error. A directory that the walk
    // cannot list does not stop the move, but the call then returns the
    // first listing error. On such a host, the move of a directory walks
    // its whole tree.
    static NTSTATUS MoveSidecarRecords(
        const std::wstring& from,
        const std::wstring& to,
        const LayerConfig& config,
        std::vector<MovedSidecarRecord>* moved);

    // Moves the records as MoveSidecarRecords does, but an entry whose
    // records cannot move does not stop the move. Its records stay at the
    // key of its old path, and the records at the key of its new path go,
    // so the entry does not take the records of an entry it replaced.
    // Returns the first error of a record move or of a listing.
    static NTSTATUS MoveSidecarRecordsLeavingStuckOnes(
        const std::wstring& from,
        const std::wstring& to,
        const LayerConfig& config);

    // Moves the records in moved back to their old paths, in reverse order.
    // A record that cannot move back stays at its new path, and the call
    // continues with the next one. Returns the first error.
    static NTSTATUS MoveSidecarRecordsBack(
        const std::vector<MovedSidecarRecord>& moved,
        const LayerConfig& config);

    // Call before a delete of the entry at `path`. When the config's host
    // lacks LM_CAP_ADS, returns the path of the entry and of each entry below
    // it, without a walk into a junction or a directory symbolic link.
    // Otherwise returns nothing.
    static std::vector<std::wstring> ListSidecarKeyedEntries(
        const std::wstring& path,
        const LayerConfig& config);

    // Call after the delete with the list from ListSidecarKeyedEntries, or
    // with the paths that the delete removed. Removes the sidecar record and
    // opaque marker of each listed entry that the delete removed, so a new
    // entry at its path starts with none. Does nothing when the sidecar
    // store is not in use.
    static void RemoveSidecarRecordsOfGoneEntries(
        const std::vector<std::wstring>& entries,
        const LayerConfig& config);
};

}
