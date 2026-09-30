// HRESULT -> .NET exception translation layer.
//
// Subclass choice is deliberately narrow -- one per category that .NET
// callers already have a conventional catch for (NotFound, AccessDenied,
// InvalidHandle) plus a capability-missing bucket for the overlay-specific
// degradation paths.

using System;
using LayerMount.Interop;

namespace LayerMount;

/// <summary>
/// Base class for failures raised from the LayerMount native ABI.
/// </summary>
public class LayerMountException : Exception
{
    /// <summary>Native entry-point name or user-supplied call-site tag.</summary>
    public string Context { get; }

    /// <summary>
    /// True when the call failed while it filled a metacopy shell. An open
    /// for data, a write, or a change of size on a metacopy shell copies the
    /// lower file's data into the shell first. A failed fill fails the call
    /// with the status of the step that failed, so <see cref="Exception.HResult"/>
    /// alone does not tell it apart from a refusal of the call. A host adapter
    /// that falls back to a create when an open reports not found checks this
    /// property first, because a fill that cannot find its lower file also
    /// reports not found.
    /// </summary>
    public bool FromMetacopyFill { get; }

    internal LayerMountException(NativeFailure failure)
        : base(BuildMessage(failure))
    {
        HResult = failure.Hr;
        Context = failure.Context;
        FromMetacopyFill = failure.FromMetacopyFill;
    }

    private static string BuildMessage(NativeFailure failure)
    {
        if (!string.IsNullOrEmpty(failure.Message))
        {
            return $"{failure.Context}: {failure.Message} (HRESULT=0x{failure.Hr:X8})";
        }
        return $"{failure.Context}: LayerMount operation failed (HRESULT=0x{failure.Hr:X8})";
    }
}

/// <summary>
/// A path or layer entry was not found. Corresponds to
/// HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) and
/// HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND).
/// </summary>
public sealed class LayerMountNotFoundException : LayerMountException
{
    internal LayerMountNotFoundException(NativeFailure failure)
        : base(failure) { }
}

/// <summary>
/// The overlay refused access. Corresponds to E_ACCESSDENIED and
/// HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED).
/// </summary>
public sealed class LayerMountAccessDeniedException : LayerMountException
{
    internal LayerMountAccessDeniedException(NativeFailure failure)
        : base(failure) { }
}

/// <summary>
/// An opaque handle was invalid (freed, wrong kind, or never valid).
/// Corresponds to E_HANDLE.
/// </summary>
public sealed class LayerMountInvalidHandleException : LayerMountException
{
    internal LayerMountInvalidHandleException(NativeFailure failure)
        : base(failure) { }
}

/// <summary>
/// An operation was refused because the host cleared the required
/// capability bit and the degradation path rejected the request.
/// Identified via E_NOTIMPL plus an explanatory message mentioning
/// "capability" from LayerMountGetLastErrorMessage.
/// </summary>
public sealed class LayerMountCapabilityMissingException : LayerMountException
{
    internal LayerMountCapabilityMissingException(NativeFailure failure)
        : base(failure) { }
}

/// <summary>
/// The facts about one failed native call that every exception carries.
/// </summary>
internal readonly record struct NativeFailure(
    int Hr, string Context, string? Message, bool FromMetacopyFill);

/// <summary>
/// Translates HRESULTs returned by the native ABI into LayerMountException
/// hierarchy instances. Every managed wrapper method routes through
/// <see cref="ThrowIfFailed"/> so the exception shape is uniform.
/// </summary>
internal static class HResultGuard
{
    // LayerMount.dll returns two HRESULT flavors: Win32-facility values
    // (HRESULT_FROM_WIN32, 0x8007xxxx) and NT-facility values
    // (HRESULT_FROM_NT, 0xDxxxxxxx). Each category maps both.
    private const int E_ACCESSDENIED       = unchecked((int)0x80070005u);
    internal const int E_HANDLE            = unchecked((int)0x80070006u);
    private const int E_NOTIMPL            = unchecked((int)0x80004001u);
    internal const int E_ABORT             = unchecked((int)0x80004004u);
    internal const int E_FAIL              = unchecked((int)0x80004005u);
    private const int HR_FILE_NOT_FOUND    = unchecked((int)0x80070002u);
    private const int HR_PATH_NOT_FOUND    = unchecked((int)0x80070003u);
    private const int HR_NT_NOT_FOUND      = unchecked((int)0xD0000034u); // STATUS_OBJECT_NAME_NOT_FOUND
    private const int HR_NT_PATH_NOT_FOUND = unchecked((int)0xD000003Au); // STATUS_OBJECT_PATH_NOT_FOUND
    private const int HR_NT_ACCESS_DENIED  = unchecked((int)0xD0000022u); // STATUS_ACCESS_DENIED
    private const int HR_NT_INVALID_HANDLE = unchecked((int)0xD0000008u); // STATUS_INVALID_HANDLE

    public static void ThrowIfFailed(int hr, string context)
    {
        if (hr >= 0)
        {
            return;
        }
        string? message = ReadLastErrorMessage(hr);
        // The next native call clears the fill mark, so read it before any
        // other call on this thread.
        bool fromMetacopyFill = ReadLastFailureWasFill();
        throw MapException(new NativeFailure(hr, context, message, fromMetacopyFill));
    }

    private static unsafe string? ReadLastErrorMessage(int hr)
    {
        // LayerMountGetLastErrorMessage is itself HRESULT-returning; failure to
        // retrieve the message just leaves the exception without the TLS
        // detail, which is acceptable -- the HRESULT alone still identifies
        // the failure.
        int probeHr = BufferHelpers.TryReadString(
            (char* buffer, nuint capacity, nuint* required) =>
                NativeMethods.LayerMountGetLastErrorMessage(hr, buffer, capacity, required),
            out string? message);
        return probeHr >= 0 ? message : null;
    }

    private static unsafe bool ReadLastFailureWasFill()
    {
        int wasFill = 0;
        int hr = NativeMethods.LayerMountGetLastFailureWasFill(&wasFill);
        return hr >= 0 && wasFill != 0;
    }

    private static LayerMountException MapException(NativeFailure failure)
    {
        switch (failure.Hr)
        {
            case HR_FILE_NOT_FOUND:
            case HR_PATH_NOT_FOUND:
            case HR_NT_NOT_FOUND:
            case HR_NT_PATH_NOT_FOUND:
                return new LayerMountNotFoundException(failure);

            case E_ACCESSDENIED:
            case HR_NT_ACCESS_DENIED:
                return new LayerMountAccessDeniedException(failure);

            case E_HANDLE:
            case HR_NT_INVALID_HANDLE:
                return new LayerMountInvalidHandleException(failure);

            case E_NOTIMPL
                when failure.Message is not null
                     && failure.Message.Contains("capability", StringComparison.OrdinalIgnoreCase):
                return new LayerMountCapabilityMissingException(failure);

            default:
                return new LayerMountException(failure);
        }
    }
}
