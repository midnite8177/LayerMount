// LayerMount.ProcessTracker -- managed facade for the process-tracking
// subsystem. Obtained via <see cref="LayerMount.ProcessTracker"/>.

using System;
using LayerMount.Interop;

namespace LayerMount;

public sealed class ProcessTrackerApi
{
    private readonly LayerMount _owner;

    internal ProcessTrackerApi(LayerMount owner) => _owner = owner;

    public void Enable(bool enable)
    {
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountProcessTrackerEnable(
            lease.Handle, enable ? 1 : 0);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountProcessTrackerEnable));
    }

    public void SetRules(string rulesPath)
    {
        ArgumentNullException.ThrowIfNull(rulesPath);
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountProcessTrackerSetRules(
            lease.Handle, rulesPath);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountProcessTrackerSetRules));
    }

    public unsafe string ExportJson()
    {
        using var lease = new SafeHandleLease(_owner.Handle);
        IntPtr h = lease.Handle;
        int hr = BufferHelpers.TryReadString(
            (char* buffer, nuint capacity, nuint* required) =>
                NativeMethods.LayerMountProcessTrackerExportJson(h, buffer, capacity, required),
            out string? result);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountProcessTrackerExportJson));
        return result ?? string.Empty;
    }

    public unsafe string ExportCsv()
    {
        using var lease = new SafeHandleLease(_owner.Handle);
        IntPtr h = lease.Handle;
        int hr = BufferHelpers.TryReadString(
            (char* buffer, nuint capacity, nuint* required) =>
                NativeMethods.LayerMountProcessTrackerExportCsv(h, buffer, capacity, required),
            out string? result);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountProcessTrackerExportCsv));
        return result ?? string.Empty;
    }
}
