using System;

namespace LayerMount;

/// <summary>
/// Managed receipt for a VSS snapshot, returned from
/// <see cref="VssApi.CreateSnapshot"/>. The handle is a reference, not
/// an owner of a resource. Closing it releases only the slot in the
/// native handle table; it does not change the OS-side snapshot. The
/// lifetime of the OS-side snapshot depends on
/// <see cref="VssApi.DeleteSnapshot"/> (non-persistent) or on the
/// backup administrator (persistent).
/// </summary>
public sealed class VssSnapshot : IDisposable
{
    private readonly VssSnapshotHandle _handle;

    internal VssSnapshot(
        VssSnapshotHandle handle,
        string id,
        string devicePath,
        bool persistent)
    {
        _handle = handle;
        Id = id;
        DevicePath = devicePath;
        Persistent = persistent;
    }

    /// <summary>VSS snapshot id (GUID without braces).</summary>
    public string Id { get; }

    /// <summary>
    /// OS device path of the shadow copy
    /// (e.g. <c>\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy123</c>),
    /// without a trailing separator. The bare form names the device
    /// object, not a directory: append a backslash before a directory
    /// query such as GetFileAttributesW, or the query fails while the
    /// snapshot is live.
    /// </summary>
    public string DevicePath { get; }

    /// <summary>
    /// True when the snapshot survives process exit. Non-persistent
    /// snapshots are torn down automatically when all handles in the
    /// creating process close.
    /// </summary>
    public bool Persistent { get; }

    /// <summary>
    /// Releases the slot in the native handle table. Has no effect on
    /// the OS-side snapshot.
    /// </summary>
    public void Dispose() => _handle.Dispose();
}
