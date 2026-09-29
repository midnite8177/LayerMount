// Two-call buffer-pattern helpers for the LayerMount C ABI.
//
// Native entry points with a string or byte out-buffer share one contract:
// pass buffer=null and a capacity of 0 to get the required size, then
// allocate and call again. TwoCallFunc states the full contract.

using System;

namespace LayerMount.Interop;

internal static class BufferHelpers
{
    // Elements at or below which we stack-allocate; above, we heap-
    // allocate. 256 WCHARs = 512 bytes, well under the 1 KiB stack-use
    // soft-limit most callers tolerate.
    internal const int StackThreshold = 256;

    // Maximum fill-call attempts before surfacing ERROR_MORE_DATA as a real
    // failure. Live-growing exports (e.g. process-tracker JSON while
    // callbacks still fire) can legitimately grow between sizing and fill,
    // but we cap retries so a pathological grow-faster-than-we-allocate
    // scenario surfaces as a proper error instead of looping forever.
    internal const int MaxFillRetries = 5;

    // HRESULT_FROM_WIN32(ERROR_MORE_DATA) -- the ABI returns this on the
    // fill call when the live payload grew past our allocated capacity.
    internal const int HRESULT_E_MORE_DATA = unchecked((int)0x800700EA);

    /// <summary>
    /// Shape of a native entry point that implements the two-call pattern
    /// for one out-buffer of <typeparamref name="T"/> elements. Returns
    /// HRESULT and always writes *required, on failure and on success.
    /// After a successful fill, *required holds the count of elements
    /// written; for a string that count includes the NUL.
    /// </summary>
    internal unsafe delegate int TwoCallFunc<T>(
        T* buffer, nuint bufferLength, nuint* required)
        where T : unmanaged;

    private delegate TResult WrittenProjection<T, TResult>(ReadOnlySpan<T> written);

    /// <summary>
    /// Runs the two-call buffer pattern. Returns the resulting managed
    /// string (NUL-terminator trimmed) and the final HRESULT. The string
    /// holds only the chars the fill call reports, so a source that shrank
    /// after the sizing call leaves no trailing NULs. On failure
    /// the string is null and the caller must route the HRESULT through
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
    internal static int TryReadString(
        TwoCallFunc<char> call, out string? result)
        => TryRead(
            call,
            static written => written.Length <= 1
                ? string.Empty
                : new string(written[..^1]),
            out result);

    /// <summary>
    /// Runs the two-call buffer pattern for a byte result. Returns the
    /// bytes the fill call wrote and the final HRESULT. On failure the
    /// array is null and the caller must route the HRESULT through
    /// <c>HResultGuard.ThrowIfFailed</c>.
    /// </summary>
    /// <remarks>
    /// Retries the fill call on ERROR_MORE_DATA up to <see cref="MaxFillRetries"/>
    /// times, for a source that grew after the sizing call. When the source
    /// shrank, the array holds only the bytes the fill call reports.
    /// </remarks>
    internal static int TryReadBytes(
        TwoCallFunc<byte> call, out byte[]? result)
        => TryRead(call, static written => written.ToArray(), out result);

    private static unsafe int TryRead<T, TResult>(
        TwoCallFunc<T> call, WrittenProjection<T, TResult> project, out TResult? result)
        where T : unmanaged
    {
        nuint required = 0;
        int hr = call(null, 0, &required);
        if (hr < 0)
        {
            result = default;
            return hr;
        }

        if (required == 0)
        {
            result = project(ReadOnlySpan<T>.Empty);
            return 0;
        }

        for (int attempt = 0; attempt < MaxFillRetries; attempt++)
        {
            int length = checked((int)required);
            Span<T> buffer = length <= StackThreshold
                ? stackalloc T[StackThreshold]
                : new T[length];
            buffer = buffer[..length];

            nuint actual = 0;
            fixed (T* p = buffer)
            {
                hr = call(p, required, &actual);
            }

            if (hr == HRESULT_E_MORE_DATA && actual > required)
            {
                required = actual;
                continue;
            }
            if (hr < 0)
            {
                result = default;
                return hr;
            }

            // Without the clamp, a fill that reports more than the buffer
            // holds throws ArgumentOutOfRangeException from the slice.
            result = project(buffer[..(int)Math.Min(actual, required)]);
            return 0;
        }

        // Exhausted retries: the source is growing faster than we can
        // allocate. Surface as a real ERROR_MORE_DATA so callers see a
        // distinct failure rather than a silent truncation.
        result = default;
        return HRESULT_E_MORE_DATA;
    }
}
