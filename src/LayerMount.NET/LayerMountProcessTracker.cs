// LayerMount.ProcessTracker -- managed facade for the process-tracking
// subsystem. Obtained via <see cref="LayerMount.ProcessTracker"/>.

using System;
using LayerMount.Interop;

namespace LayerMount;

/// <summary>
/// Facade for the process-tracker subsystem. A caller gets an instance
/// from <see cref="LayerMount.ProcessTracker"/>.
/// </summary>
public sealed class ProcessTrackerApi
{
    private readonly LayerMount _owner;

    internal ProcessTrackerApi(LayerMount owner) => _owner = owner;

    /// <summary>
    /// Turns process tracking on or off for the overlay. Enabling fails
    /// if a configured rules file cannot be loaded, since an empty rule
    /// set would silently allow every operation.
    /// </summary>
    /// <exception cref="LayerMountException">
    /// The native call returns a non-success HRESULT, including when a
    /// configured rules file cannot be loaded.
    /// </exception>
    public void Enable(bool enable)
    {
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountProcessTrackerEnable(
            lease.Handle, enable ? 1 : 0);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountProcessTrackerEnable));
    }

    /// <summary>
    /// (Re-)loads the process-tracker rules JSON at
    /// <paramref name="rulesPath"/>.
    /// </summary>
    /// <exception cref="ArgumentNullException">
    /// <paramref name="rulesPath"/> is null.
    /// </exception>
    /// <exception cref="LayerMountException">
    /// The native call returns a non-success HRESULT, including an empty
    /// <paramref name="rulesPath"/>, the tracker not enabled
    /// (<see cref="Enable"/>), or the file could not be read or parsed.
    /// </exception>
    public void SetRules(string rulesPath)
    {
        ArgumentNullException.ThrowIfNull(rulesPath);
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountProcessTrackerSetRules(
            lease.Handle, rulesPath);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountProcessTrackerSetRules));
    }

    /// <summary>
    /// Exports the process-tracker access log as a JSON string.
    /// </summary>
    /// <exception cref="LayerMountException">
    /// The native call returns a non-success HRESULT, including when the
    /// tracker is not enabled (<see cref="Enable"/>).
    /// </exception>
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

    /// <summary>
    /// Exports the process-tracker access log as CSV.
    /// </summary>
    /// <exception cref="LayerMountException">
    /// The native call returns a non-success HRESULT, including when the
    /// tracker is not enabled (<see cref="Enable"/>).
    /// </exception>
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
