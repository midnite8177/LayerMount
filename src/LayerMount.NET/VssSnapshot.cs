// VssSnapshot -- managed receipt returned from
// <see cref="LayerMount.Vss.CreateSnapshot"/>.
//
// The native ABI has no LayerMountVssClose export -- the handle is a
// reference, not a resource owner. OS-side lifetime is driven by
// <see cref="LayerMount.Vss.DeleteSnapshot"/> (non-persistent) or by the
// backup administrator (persistent). Disposing this object is a no-op
// with respect to the OS snapshot; it only releases the slot in the
// native handle table.

using System;

namespace LayerMount;

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

    /// <summary>OS device path of the shadow copy
    /// (e.g. <c>\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy123</c>).</summary>
    public string DevicePath { get; }

    /// <summary>
    /// True when the snapshot survives process exit. Non-persistent
    /// snapshots are torn down automatically when all handles in the
    /// creating process close.
    /// </summary>
    public bool Persistent { get; }

    public void Dispose() => _handle.Dispose();
}
