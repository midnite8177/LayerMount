// Two-call buffer-pattern helpers for the LayerMount C ABI.
//
// The native DLL exposes several PWSTR out-buffer entry points that share a
// uniform contract: pass buffer=null / bufferChars=0 to learn the required
// size in *requiredChars, then allocate and call again. ERROR_MORE_DATA
// (as HRESULT_FROM_WIN32) is returned if the caller-provided buffer was
// short; *requiredChars is always written on that path.
//
// This file isolates the dance so every public-API surface method can
// express the intent as a one-liner.

using System;

namespace LayerMount.Interop;

internal static class BufferHelpers
{
    // Chars (not bytes) at or below which we stack-allocate; above, we heap-
    // allocate. 256 WCHARs = 512 bytes, well under the 1 KiB stack-use
    // soft-limit most callers tolerate.
    private const int StackThreshold = 256;

    // Maximum fill-call attempts before surfacing ERROR_MORE_DATA as a real
    // failure. Live-growing exports (e.g. process-tracker JSON while
    // callbacks still fire) can legitimately grow between sizing and fill,
    // but we cap retries so a pathological grow-faster-than-we-allocate
    // scenario surfaces as a proper error instead of looping forever.
    private const int MaxFillRetries = 5;

    // HRESULT_FROM_WIN32(ERROR_MORE_DATA) -- the ABI returns this on the
    // fill call when the live payload grew past our allocated capacity.
    private const int HRESULT_E_MORE_DATA = unchecked((int)0x800700EA);

    // Shape of a native entry point that implements the two-call pattern
    // for a single PWSTR output. Returns HRESULT; always writes
    // *requiredChars (including NUL) on failure + success.
    internal unsafe delegate int TwoCallStringFunc(
        char* buffer, nuint bufferChars, nuint* requiredChars);

    /// <summary>
    /// Runs the two-call buffer pattern. Returns the resulting managed
    /// string (NUL-terminator trimmed) and the final HRESULT. On failure
    /// the string is null and the caller should route the HRESULT through
    /// <c>HResultGuard.ThrowIfFailed</c>.
    /// </summary>
    /// <remarks>
    /// Retries the fill call on ERROR_MORE_DATA up to <see cref="MaxFillRetries"/>
    /// times. Live-backed ABIs (e.g.
    /// <c>LayerMountProcessTrackerExportJson</c> / <c>ExportCsv</c>) keep
    /// mutating the underlying string while the tracker still receives
    /// events, so the required size measured on the sizing call can
    /// legitimately grow by the time we make the fill call. Without the
    /// retry, live exports surface spurious
    /// <c>LayerMountException(ERROR_MORE_DATA)</c> to managed callers.
    /// </remarks>
    internal static unsafe int TryReadString(
        TwoCallStringFunc call, out string? result)
    {
        nuint required = 0;
        int hr = call(null, 0, &required);
        if (hr < 0)
        {
            result = null;
            return hr;
        }

        // requiredChars includes the NUL. 0 or 1 means "empty string".
        if (required <= 1)
        {
            result = string.Empty;
            return 0;
        }

        for (int attempt = 0; attempt < MaxFillRetries; attempt++)
        {
            int charCount = checked((int)required);
            Span<char> buffer = charCount <= StackThreshold
                ? stackalloc char[StackThreshold]
                : new char[charCount];
            buffer = buffer[..charCount];

            nuint actualRequired;
            fixed (char* p = buffer)
            {
                actualRequired = 0;
                hr = call(p, required, &actualRequired);
            }

            if (hr == HRESULT_E_MORE_DATA && actualRequired > required)
            {
                // Source grew between probe and fill. Adopt the new size
                // and retry with a larger buffer.
                required = actualRequired;
                continue;
            }
            if (hr < 0)
            {
                result = null;
                return hr;
            }

            // Strip the trailing NUL -- the native ABI always NUL-terminates.
            result = new string(buffer[..(charCount - 1)]);
            return 0;
        }

        // Exhausted retries: the source is growing faster than we can
        // allocate. Surface as a real ERROR_MORE_DATA so callers see a
        // distinct failure rather than a silent truncation.
        result = null;
        return HRESULT_E_MORE_DATA;
    }
}
