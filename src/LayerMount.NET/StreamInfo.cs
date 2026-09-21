// StreamInfo -- a single named-data-stream entry reported by
// <see cref="LayerMount.EnumerateStreams"/>. Names carry NTFS's native
// `:foo:$DATA` form; the main unnamed stream (`::$DATA`) and LayerMount's
// reserved metadata streams are filtered out by the engine before this
// type is constructed, so callers see only user-visible ADS.

namespace LayerMount;

/// <summary>
/// One named data stream as reported by
/// <see cref="LayerMount.EnumerateStreams"/>.
/// </summary>
/// <param name="Name">
/// NTFS-native stream name, e.g. <c>:foo:$DATA</c>. Callers that want
/// just the user-supplied stream identifier should strip the leading
/// <c>:</c> and trailing <c>:$DATA</c>.
/// </param>
/// <param name="StreamSize">Logical end-of-file in bytes.</param>
/// <param name="AllocationSize">
/// <paramref name="StreamSize"/> rounded up to 4 KiB, never below it.
/// The same rule gives the allocation of a file.
/// </param>
public readonly record struct StreamInfo(
    string Name,
    ulong  StreamSize,
    ulong  AllocationSize);
