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
/// On-disk allocation in bytes. The underlying <c>FindFirstStreamW</c>
/// query does not distinguish logical and allocated size, so this
/// currently mirrors <paramref name="StreamSize"/>; precise allocation
/// accounting would require opening the stream and querying
/// <c>FILE_STANDARD_INFORMATION</c>.
/// </param>
public readonly record struct StreamInfo(
    string Name,
    ulong  StreamSize,
    ulong  AllocationSize);
