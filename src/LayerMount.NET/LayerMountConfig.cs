using System.Collections.Generic;

namespace LayerMount;

/// <summary>
/// Construction parameters for <see cref="LayerMount.Create"/>.
/// </summary>
/// <remarks>
/// Mirrors the native <c>LM_CONFIG</c> field set while projecting
/// pointer-based fields into idiomatic managed types, forward-compatible
/// through the native <c>structSize</c> mechanism: a field added here in
/// a later version projects onto a larger native struct without breaking
/// callers built against an older header. Every path here is read only
/// for the duration of the <c>Create</c> call. The native DLL copies all
/// referenced strings into internal storage before returning, so the
/// caller owns nothing past that call.
/// </remarks>
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

    /// <summary>
    /// Whether the process tracker starts enabled. See
    /// <see cref="ProcessRulesPath"/> for the rules file it loads.
    /// </summary>
    public bool EnableProcessTracking { get; init; }

    /// <summary>Optional path to a JSON rules file for the process tracker.</summary>
    public string? ProcessRulesPath { get; init; }

    /// <summary>Process-tracker access-log circular buffer capacity.</summary>
    public uint AccessLogCapacity { get; init; } = 1024;

    /// <summary>Path-resolver cache capacity.</summary>
    public uint PathCacheCapacity { get; init; } = 1024;
}
