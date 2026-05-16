// SafeHandle subclasses for the native LM_* opaque handle types.
//
// Each wraps one of the DLL's opaque handle types and releases it through
// the matching destroy/close export. Use from the high-level wrappers
// (LayerMount, LayerMountFile, VhdImage, LayerImage); consumers never see these
// directly.
//
// ReleaseHandle cannot throw -- it may run on the finalizer thread. A
// failure HRESULT from the underlying destroy/close entry point is logged
// (returning false from ReleaseHandle makes the CLR raise a
// SafeHandleCriticalFailure on managed debug builds but is otherwise
// harmless). The common failure mode is LayerMountDestroy returning
// E_ILLEGAL_METHOD_CALL when a host adapter is still attached -- callers
// must clear the host-attached flag (via the matching Unmount path) before
// disposing the overlay.

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using LayerMount.Interop;

namespace LayerMount;

// Internal helper used from every ReleaseHandle below. A failing HRESULT
// means the native slot is now stranded until process exit -- that leak is
// silent without diagnostic output, so route it to Debug.WriteLine so it
// at least surfaces when attached to a debugger or instrumented host.
// Must not throw; called from finalizer thread.
internal static class ReleaseDiagnostics
{
    public static bool Report(string handleKind, IntPtr raw, int hr)
    {
        if (hr >= 0) return true;
        try
        {
            Debug.WriteLine(
                $"[LayerMount.SafeHandle] {handleKind} release failed: " +
                $"handle=0x{raw.ToInt64():X} hr=0x{hr:X8}. " +
                "Native handle-table slot will remain allocated until process exit.");
        }
        catch
        {
            // Never propagate from finalizer.
        }
        return false;
    }
}

/// <summary>
/// RAII-style lease that pins a <see cref="SafeHandle"/> for the duration
/// of a native call so a concurrent <c>Dispose</c> or finalizer cannot
/// release the native handle while it is in use. Pair every call that
/// needs a raw <c>IntPtr</c> with a <c>using</c>-scoped lease:
/// <code>
/// using var lease = new SafeHandleLease(_handle);
/// int hr = NativeMethods.Foo(lease.Handle, ...);
/// </code>
/// </summary>
internal ref struct SafeHandleLease
{
    private readonly SafeHandle _h;
    private bool _added;

    /// <summary>Pinned raw handle. Valid until <see cref="Dispose"/>.</summary>
    public readonly IntPtr Handle;

    public SafeHandleLease(SafeHandle h)
    {
        _h = h;
        _added = false;
        // DangerousAddRef sets `_added = true` on success. If the handle
        // is already closed it throws ObjectDisposedException; translate
        // that into the library-typed LayerMountInvalidHandleException so
        // callers that catch LayerMountException observe the same shape as
        // a native E_HANDLE, not a raw framework exception.
        try
        {
            h.DangerousAddRef(ref _added);
        }
        catch (ObjectDisposedException ex)
        {
            throw new LayerMountInvalidHandleException(
                unchecked((int)0x80070006u) /* E_HANDLE */,
                "SafeHandleLease",
                ex.Message);
        }
        Handle = h.DangerousGetHandle();
    }

    public void Dispose()
    {
        if (_added)
        {
            _h.DangerousRelease();
            _added = false;
        }
    }
}

/// <summary>
/// Owns an <c>LM_HANDLE</c> (the top-level overlay instance). Released via
/// <c>LayerMountDestroy</c>.
/// </summary>
public sealed class LayerMountHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    internal LayerMountHandle() : base(ownsHandle: true) { }

    internal void SetRawHandle(IntPtr h) => SetHandle(h);

    protected override bool ReleaseHandle()
    {
        int hr = NativeMethods.LayerMountDestroy(handle);
        return ReleaseDiagnostics.Report(nameof(LayerMountHandle), handle, hr);
    }
}

/// <summary>
/// Owns an <c>LM_FILE_HANDLE</c>. Released via <c>LayerMountCloseFile</c>.
/// </summary>
public sealed class LayerMountFileHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    internal LayerMountFileHandle() : base(ownsHandle: true) { }

    internal void SetRawHandle(IntPtr h) => SetHandle(h);

    protected override bool ReleaseHandle()
    {
        int hr = NativeMethods.LayerMountCloseFile(handle);
        return ReleaseDiagnostics.Report(nameof(LayerMountFileHandle), handle, hr);
    }
}

/// <summary>
/// Owns an <c>LM_VHD_HANDLE</c>. Released via <c>LayerMountVhdClose</c>.
/// </summary>
public sealed class VhdHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    internal VhdHandle() : base(ownsHandle: true) { }

    internal void SetRawHandle(IntPtr h) => SetHandle(h);

    protected override bool ReleaseHandle()
    {
        int hr = NativeMethods.LayerMountVhdClose(handle);
        return ReleaseDiagnostics.Report(nameof(VhdHandle), handle, hr);
    }
}

/// <summary>
/// Owns an <c>LM_VSS_SNAPSHOT_HANDLE</c>. Released via
/// <c>LayerMountVssCloseSnapshot</c> -- the handle is a receipt for the
/// per-session handle-table slot, not a resource owner. OS-side
/// snapshot lifetime is driven explicitly via
/// <c>LayerMountVssDeleteSnapshot</c> (non-persistent) or left to the
/// backup admin (persistent). Closing the receipt does not alter the
/// snapshot's existence; skipping it would leak the handle-table slot
/// until overlay teardown.
/// </summary>
public sealed class VssSnapshotHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    internal VssSnapshotHandle() : base(ownsHandle: true) { }

    internal void SetRawHandle(IntPtr h) => SetHandle(h);

    protected override bool ReleaseHandle()
    {
        int hr = NativeMethods.LayerMountVssCloseSnapshot(handle);
        return ReleaseDiagnostics.Report(nameof(VssSnapshotHandle), handle, hr);
    }
}

/// <summary>
/// Owns an <c>LM_IMAGE_HANDLE</c>. Released via <c>LayerMountImageClose</c>.
/// </summary>
public sealed class ImageHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    internal ImageHandle() : base(ownsHandle: true) { }

    internal void SetRawHandle(IntPtr h) => SetHandle(h);

    protected override bool ReleaseHandle()
    {
        int hr = NativeMethods.LayerMountImageClose(handle);
        return ReleaseDiagnostics.Report(nameof(ImageHandle), handle, hr);
    }
}
