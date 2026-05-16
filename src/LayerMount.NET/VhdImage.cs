// VhdImage -- managed wrapper over an <c>LM_VHD_HANDLE</c>.
//
// Obtained from <see cref="LayerMount.Vhd.Create"/> or
// <see cref="LayerMount.Vhd.Open"/>. Exposes the handle-bound VHD
// operations: Attach / Detach / Merge / GetVolumeGuid.

using System;
using LayerMount.Interop;

namespace LayerMount;

public sealed class VhdImage : IDisposable
{
    private readonly VhdHandle _handle;

    internal VhdImage(VhdHandle handle, string path)
    {
        _handle = handle;
        Path = path;
    }

    /// <summary>Source path of the VHD file on disk.</summary>
    public string Path { get; }

    public bool IsClosed => _handle.IsClosed;

    /// <summary>
    /// Attaches the VHD and returns the physical device path
    /// (e.g. <c>\\.\PhysicalDrive3</c>). Idempotent -- a subsequent call
    /// on the same handle returns the cached path without re-attaching.
    /// </summary>
    public unsafe string Attach()
    {
        using var lease = new SafeHandleLease(_handle);
        IntPtr h = lease.Handle;
        int hr = BufferHelpers.TryReadString(
            (char* buffer, nuint capacity, nuint* required) =>
                NativeMethods.LayerMountVhdAttach(h, buffer, capacity, required),
            out string? physicalPath);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVhdAttach));
        return physicalPath ?? string.Empty;
    }

    /// <summary>Detaches the VHD from the OS.</summary>
    public void Detach()
    {
        using var lease = new SafeHandleLease(_handle);
        int hr = NativeMethods.LayerMountVhdDetach(lease.Handle);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVhdDetach));
    }

    /// <summary>
    /// Merges this (differencing) VHD into its parent. The VHD must not
    /// be attached at the time of the call.
    /// </summary>
    public void Merge()
    {
        using var lease = new SafeHandleLease(_handle);
        int hr = NativeMethods.LayerMountVhdMerge(lease.Handle);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVhdMerge));
    }

    /// <summary>
    /// Resolves the volume GUID path (<c>\\?\Volume{...}\</c>) for an
    /// attached VHD. Must be called after <see cref="Attach"/>; PnP can
    /// lag the attach, so callers should retry on empty results.
    /// </summary>
    public unsafe string GetVolumeGuid()
    {
        using var lease = new SafeHandleLease(_handle);
        IntPtr h = lease.Handle;
        int hr = BufferHelpers.TryReadString(
            (char* buffer, nuint capacity, nuint* required) =>
                NativeMethods.LayerMountVhdGetVolumeGuid(h, buffer, capacity, required),
            out string? guid);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVhdGetVolumeGuid));
        return guid ?? string.Empty;
    }

    public void Dispose() => _handle.Dispose();
}
