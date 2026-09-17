// Event marshalling from the native LM_EVENT_CALLBACK to managed
// EventHandler<LayerMountEventArgs>.
//
// The native engine calls LM_EVENT_CALLBACK on arbitrary internal threads.
// To stay AOT-safe (no Marshal.GetFunctionPointerForDelegate), we register
// a [UnmanagedCallersOnly] static trampoline as the callback, and thread
// the LayerMount identity through via userContext = GCHandle.ToIntPtr(handle)
// where the handle holds a strong reference to the LayerMount instance.
//
// Because a late callback can race with LayerMount.Dispose (the native Set
// primitive does not serialize against in-flight Emit invocations), the
// GCHandle is Normal (strong). Forgetting to Dispose the LayerMount leaks the
// instance until process exit -- standard .NET IDisposable contract.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using LayerMount.Interop;

namespace LayerMount;

/// <summary>
/// Managed payload of an LayerMount native event. A new instance is created
/// per event fan-out; retaining the args after the event handler returns
/// is safe (unlike the native <c>LM_EVENT*</c> whose string pointers are
/// only valid for the duration of the callback).
/// </summary>
public sealed class LayerMountEventArgs : EventArgs
{
    /// <summary>The event's category.</summary>
    public LayerMountEventType Type { get; }

    /// <summary>The event's HRESULT. <c>S_OK</c> for an informational event.</summary>
    public int HResult { get; }

    /// <summary>
    /// The path the event concerns, relative to the overlay root. Null
    /// when the event has no associated path.
    /// </summary>
    public string? RelativePath { get; }

    /// <summary>
    /// A human-readable message for the event, when the engine supplies
    /// one. Null otherwise.
    /// </summary>
    public string? Message { get; }

    /// <summary>
    /// When the event occurred, in UTC. A native timestamp outside the
    /// range <see cref="DateTime"/> can represent is clamped to
    /// <see cref="DateTime.MinValue"/> instead of throwing.
    /// </summary>
    public DateTime TimestampUtc { get; }

    /// <summary>The process id associated with the event, when known. 0 if not applicable.</summary>
    public int Pid { get; }

    internal LayerMountEventArgs(
        LayerMountEventType type,
        int hResult,
        string? relativePath,
        string? message,
        DateTime timestampUtc,
        int pid)
    {
        Type = type;
        HResult = hResult;
        RelativePath = relativePath;
        Message = message;
        TimestampUtc = timestampUtc;
        Pid = pid;
    }
}

internal static unsafe class EventTrampoline
{
    /// <summary>
    /// Static entry point registered via <c>LayerMountSetEventCallback</c>. The
    /// <paramref name="userContext"/> is a <see cref="GCHandle"/> IntPtr
    /// pointing at the owning <see cref="LayerMount"/>.
    /// </summary>
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    public static void NativeEventCallback(LM_EVENT* evt, void* userContext)
    {
        // The callback contract forbids unwinding into native code; swallow
        // everything and proceed.
        try
        {
            if (userContext == null || evt == null) return;

            GCHandle handle = GCHandle.FromIntPtr((IntPtr)userContext);
            if (!handle.IsAllocated) return;
            if (handle.Target is not LayerMount mount) return;

            var args = new LayerMountEventArgs(
                type:         (LayerMountEventType)evt->type,
                hResult:      evt->hr,
                relativePath: PtrToOptionalString(evt->relativePath),
                message:      PtrToOptionalString(evt->message),
                timestampUtc: SafeFromFileTimeUtc((long)evt->timestamp),
                pid:          (int)evt->pid);

            mount.RaiseEvent(args);
        }
        catch
        {
            // Must never throw back into native.
        }
    }

    private static string? PtrToOptionalString(IntPtr p)
        => p == IntPtr.Zero ? null : Marshal.PtrToStringUni(p);

    // DateTime.FromFileTimeUtc throws ArgumentOutOfRangeException for
    // FILETIME values outside [0, 2650467743999999999]. A bad timestamp
    // from the native side (uninitialized memory, future-dated event,
    // sentinel zero) would otherwise propagate out of NativeEventCallback
    // -- and per the unmanaged-callback contract this method must not
    // throw back into native. Clamp to DateTime.MinValue so the event
    // still fires with a benign timestamp rather than being dropped
    // silently or aborting the process.
    private static DateTime SafeFromFileTimeUtc(long ft)
    {
        try
        {
            return DateTime.FromFileTimeUtc(ft);
        }
        catch (ArgumentOutOfRangeException)
        {
            return DateTime.MinValue;
        }
    }
}
