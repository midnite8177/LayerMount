// LayerMountConfig -- managed construction parameters for an LayerMount.
//
// Mirrors the native LM_CONFIG field set while projecting
// pointer-based fields into idiomatic managed types. Forward-compatible
// through the native structSize mechanism; new fields added here project
// to new fields in the native struct without breaking compilers that
// built against an older header.

using System.Collections.Generic;

namespace LayerMount;

public sealed record LayerMountConfig
{
    /// <summary>Absolute path to the upper (writable) layer root.</summary>
    public required string UpperPath { get; init; }

    /// <summary>Absolute path to the atomic-ops staging directory.</summary>
    public required string WorkDirPath { get; init; }

    /// <summary>Ordered lower layers; index 0 = highest priority.</summary>
    public IReadOnlyList<string> LowerPaths { get; init; }
        = System.Array.Empty<string>();

    /// <summary>
    /// Host capability bitfield. Default covers every optimized path
    /// (ADS, reparse points, sparse files, multiple streams, NTFS ACLs).
    /// Clear a bit to trigger the matching fallback.
    /// </summary>
    public HostCapabilities Capabilities { get; init; }
        = HostCapabilities.Ads
        | HostCapabilities.ReparsePoints
        | HostCapabilities.SparseFiles
        | HostCapabilities.MultipleStreams
        | HostCapabilities.NtfsAcls;

    public bool EnableProcessTracking { get; init; }

    /// <summary>Optional path to a JSON rules file for the process tracker.</summary>
    public string? ProcessRulesPath { get; init; }

    /// <summary>Process-tracker access-log circular buffer capacity.</summary>
    public uint AccessLogCapacity { get; init; } = 1024;

    /// <summary>Path-resolver cache capacity.</summary>
    public uint PathCacheCapacity { get; init; } = 1024;
}
