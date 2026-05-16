// HRESULT -> .NET exception translation layer.
//
// Every non-success HRESULT returned by the native ABI is funnelled through
// HResultGuard.ThrowIfFailed, which:
//   1. Pulls the thread-local error message via LayerMountGetLastErrorMessage.
//   2. Maps well-known HRESULT values to category-specific subclasses.
//   3. Stashes the HRESULT on Exception.HResult (already an int field) and
//      the caller-supplied context string (typically the native entry-point
//      name) on the Context property so exceptions pinpoint the call site.
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

    internal LayerMountException(int hr, string context, string? message)
        : base(BuildMessage(hr, context, message))
    {
        HResult = hr;
        Context = context;
    }

    private static string BuildMessage(int hr, string context, string? message)
    {
        if (!string.IsNullOrEmpty(message))
        {
            return $"{context}: {message} (HRESULT=0x{hr:X8})";
        }
        return $"{context}: LayerMount operation failed (HRESULT=0x{hr:X8})";
    }
}

/// <summary>
/// A path or layer entry was not found. Corresponds to
/// HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) and
/// HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND).
/// </summary>
public sealed class LayerMountNotFoundException : LayerMountException
{
    internal LayerMountNotFoundException(int hr, string context, string? message)
        : base(hr, context, message) { }
}

/// <summary>
/// The overlay refused access. Corresponds to E_ACCESSDENIED and
/// HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED).
/// </summary>
public sealed class LayerMountAccessDeniedException : LayerMountException
{
    internal LayerMountAccessDeniedException(int hr, string context, string? message)
        : base(hr, context, message) { }
}

/// <summary>
/// An opaque handle was invalid (freed, wrong kind, or never valid).
/// Corresponds to E_HANDLE.
/// </summary>
public sealed class LayerMountInvalidHandleException : LayerMountException
{
    internal LayerMountInvalidHandleException(int hr, string context, string? message)
        : base(hr, context, message) { }
}

/// <summary>
/// An operation was refused because the host cleared the required
/// capability bit and the degradation path rejected the request.
/// Identified via E_NOTIMPL plus an explanatory message mentioning
/// "capability" from LayerMountGetLastErrorMessage.
/// </summary>
public sealed class LayerMountCapabilityMissingException : LayerMountException
{
    internal LayerMountCapabilityMissingException(int hr, string context, string? message)
        : base(hr, context, message) { }
}

/// <summary>
/// Translates HRESULTs returned by the native ABI into LayerMountException
/// hierarchy instances. Every managed wrapper method routes through
/// <see cref="ThrowIfFailed"/> so the exception shape is uniform.
/// </summary>
internal static class HResultGuard
{
    // Well-known HRESULT constants (unchecked cast so high bit is preserved).
    //
    // LayerMount.dll surfaces two HRESULT flavors: Win32-facility wrappers
    // (HRESULT_FROM_WIN32, bits 0x80070000) and NT-facility wrappers
    // (HRESULT_FROM_NT, bits 0xD0000000 — the FACILITY_NT_BIT 0x10000000
    // layered over the raw NTSTATUS). The ABI contract documented in
    // LayerMount.h:526-527 explicitly includes both. Mapping only the Win32
    // set caused `new LayerMount().OpenFile("missing")` to throw the base
    // LayerMountException, which defeated every `catch (LayerMountNotFoundException)`
    // in downstream host adapters.
    private const int E_ACCESSDENIED       = unchecked((int)0x80070005u);
    private const int E_HANDLE             = unchecked((int)0x80070006u);
    private const int E_NOTIMPL            = unchecked((int)0x80004001u);
    private const int E_FAIL               = unchecked((int)0x80004005u);
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
        throw MapException(hr, context, message);
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

    private static LayerMountException MapException(int hr, string context, string? message)
    {
        switch (hr)
        {
            case HR_FILE_NOT_FOUND:
            case HR_PATH_NOT_FOUND:
            case HR_NT_NOT_FOUND:
            case HR_NT_PATH_NOT_FOUND:
                return new LayerMountNotFoundException(hr, context, message);

            case E_ACCESSDENIED:
            case HR_NT_ACCESS_DENIED:
                return new LayerMountAccessDeniedException(hr, context, message);

            case E_HANDLE:
            case HR_NT_INVALID_HANDLE:
                return new LayerMountInvalidHandleException(hr, context, message);

            case E_NOTIMPL
                when message is not null
                     && message.Contains("capability", StringComparison.OrdinalIgnoreCase):
                return new LayerMountCapabilityMissingException(hr, context, message);

            default:
                return new LayerMountException(hr, context, message);
        }
    }
}
