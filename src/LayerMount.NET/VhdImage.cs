using System;
using LayerMount.Interop;

namespace LayerMount;

/// <summary>
/// Managed wrapper over an <c>LM_VHD_HANDLE</c>, returned from
/// <see cref="VhdApi.Create"/> or <see cref="VhdApi.Open"/>. Exposes the
/// handle-bound VHD operations: <see cref="Attach"/>, <see cref="Detach"/>,
/// <see cref="Merge"/>, and <see cref="GetVolumeGuid"/>.
/// </summary>
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

    /// <summary>
    /// True once <see cref="Dispose"/> has closed the underlying handle.
    /// A VHD operation called after that throws
    /// <see cref="LayerMountInvalidHandleException"/>.
    /// </summary>
    public bool IsClosed => _handle.IsClosed;

    /// <summary>
    /// Attaches the VHD and returns the physical device path
    /// (e.g. <c>\\.\PhysicalDrive3</c>). Idempotent -- a subsequent call
    /// on the same handle returns the cached path without re-attaching.
    /// </summary>
    /// <exception cref="LayerMountException">
    /// The native call returns a non-success HRESULT.
    /// </exception>
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

    /// <summary>
    /// Detaches the VHD from the OS and clears the cached physical
    /// device path, so a later <see cref="Attach"/> call attaches fresh.
    /// </summary>
    /// <exception cref="LayerMountException">
    /// The native call returns a non-success HRESULT.
    /// </exception>
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
    /// <exception cref="LayerMountException">
    /// The VHD is attached, or the native call returns another
    /// non-success HRESULT.
    /// </exception>
    public void Merge()
    {
        using var lease = new SafeHandleLease(_handle);
        int hr = NativeMethods.LayerMountVhdMerge(lease.Handle);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVhdMerge));
    }

    /// <summary>
    /// Resolves the volume GUID path (<c>\\?\Volume{...}\</c>) for an
    /// attached VHD. Must be called after <see cref="Attach"/>; PnP can
    /// lag the attach, so a caller can retry on an empty result.
    /// </summary>
    /// <exception cref="LayerMountException">
    /// <see cref="Attach"/> has not populated the cached open handle
    /// yet, or the native call returns another non-success HRESULT.
    /// </exception>
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

    /// <summary>
    /// Closes the underlying handle, releasing its native handle-table
    /// slot. For a process-scoped attach, this also detaches the VHD; a
    /// permanent attach stays mounted until an explicit
    /// <see cref="Detach"/> call.
    /// </summary>
    public void Dispose() => _handle.Dispose();
}
