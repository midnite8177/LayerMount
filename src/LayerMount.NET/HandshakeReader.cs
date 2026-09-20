using System;
using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Threading;

namespace LayerMount;

/// <summary>
/// Parent-side reader for the mount handshake JSON line that a host-adapter
/// child process emits when started under <c>--handshake-child</c>.
/// </summary>
public static class HandshakeReader
{
    /// <summary>
    /// Reads one line from <paramref name="reader"/>, bounded by
    /// <paramref name="timeout"/>, and parses it.
    /// </summary>
    /// <remarks>
    /// The read runs on a background thread so the timeout holds without the
    /// child closing the pipe. After a timeout that thread stays blocked inside
    /// <see cref="StreamReader.ReadLine"/> and <paramref name="reader"/> is
    /// still in use, so the caller must not read from <paramref name="reader"/>
    /// again.
    /// </remarks>
    /// <returns>The parsed <see cref="HandshakeResult"/>, or <c>null</c> if no
    /// line arrived before <paramref name="timeout"/> elapsed or the stream
    /// ended first; the caller then kills the child.</returns>
    /// <exception cref="JsonException">The line is not valid JSON.</exception>
    public static HandshakeResult? ReadLine(StreamReader reader, TimeSpan timeout)
    {
        string? line = null;
        Exception? caught = null;
        var done = new ManualResetEventSlim(false);

        var t = new Thread(() =>
        {
            try { line = reader.ReadLine(); }
            catch (Exception ex) { caught = ex; }
            finally { done.Set(); }
        })
        {
            IsBackground = true,
        };
        t.Start();

        if (!done.Wait(timeout)) return null;
        if (caught is not null) throw caught;
        if (line is null) return null;
        return Parse(line);
    }

    /// <summary>
    /// Parses one handshake JSON line into a <see cref="MountedHandshake"/>,
    /// a <see cref="FailedHandshake"/> or an <see cref="UnknownHandshake"/>
    /// by its <c>status</c> field. Field getters tolerate a missing or
    /// mistyped property.
    /// </summary>
    /// <exception cref="JsonException">The line is not valid JSON.</exception>
    public static HandshakeResult Parse(string line)
    {
        using var doc = JsonDocument.Parse(line);
        var root = doc.RootElement;
        string status = GetStr(root, "status");

        return status switch
        {
            "mounted" => new MountedHandshake
            {
                StatusRaw = status,
                RawLine = line,
                Host = GetStr(root, "host"),
                InstanceId = GetStr(root, "instanceId"),
                MountPoint = GetStr(root, "mountPoint"),
                ControlPipe = GetStr(root, "controlPipe"),
                Pid = GetInt(root, "pid"),
                ProcessCreationTimeFiletime = GetLong(root, "processCreationTimeFiletime"),
                ProcessCreationTimeUtc = GetTimestampOrNull(root, "processCreationTimeUtc"),
                StartedAtUtc = GetTimestampOrNull(root, "startedAtUtc"),
            },
            "failed" => new FailedHandshake
            {
                StatusRaw = status,
                RawLine = line,
                Host = GetStr(root, "host"),
                ErrorCode = GetStrOrNull(root, "error"),
                Message = GetStrOrNull(root, "message"),
            },
            _ => new UnknownHandshake
            {
                StatusRaw = status,
                RawLine = line,
                Host = GetStr(root, "host"),
            },
        };
    }

    private static string GetStr(JsonElement root, string name)
        => root.TryGetProperty(name, out var el) && el.ValueKind == JsonValueKind.String
           ? (el.GetString() ?? string.Empty) : string.Empty;

    private static string? GetStrOrNull(JsonElement root, string name)
        => root.TryGetProperty(name, out var el) && el.ValueKind == JsonValueKind.String
           ? el.GetString() : null;

    private static int GetInt(JsonElement root, string name)
        => root.TryGetProperty(name, out var el) && el.ValueKind == JsonValueKind.Number
           && el.TryGetInt32(out int v) ? v : 0;

    private static long GetLong(JsonElement root, string name)
        => root.TryGetProperty(name, out var el) && el.ValueKind == JsonValueKind.Number
           && el.TryGetInt64(out long v) ? v : 0;

    private static DateTimeOffset? GetTimestampOrNull(JsonElement root, string name)
    {
        string? value = GetStrOrNull(root, name);
        if (value is null) return null;
        return DateTimeOffset.TryParse(
                   value,
                   CultureInfo.InvariantCulture,
                   DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal,
                   out DateTimeOffset parsed)
               ? parsed : null;
    }
}

/// <summary>Parsed handshake payload.</summary>
public abstract record HandshakeResult
{
    /// <summary>The <c>status</c> field as emitted, or empty when absent.</summary>
    public string StatusRaw { get; init; } = string.Empty;

    /// <summary>The JSON line that produced this result.</summary>
    public string RawLine { get; init; } = string.Empty;

    /// <summary>
    /// Host-adapter-supplied host identifier, read from every line. Empty
    /// when the line omits the field or gives it a non-string value.
    /// Consumers treat the value as opaque and let the host adapter define it.
    /// </summary>
    public string Host { get; init; } = string.Empty;
}

/// <summary>A line whose status is <c>mounted</c>.</summary>
public sealed record MountedHandshake : HandshakeResult
{
    /// <summary>Id of the mounted overlay, as the host adapter assigned it.</summary>
    public string InstanceId { get; init; } = string.Empty;

    /// <summary>Path the child mounted at.</summary>
    public string MountPoint { get; init; } = string.Empty;

    /// <summary>Full name of the child's control pipe.</summary>
    public string ControlPipe { get; init; } = string.Empty;

    /// <summary>Child process id.</summary>
    public int Pid { get; init; }

    /// <summary>Child process creation time as a Windows FILETIME.</summary>
    public long ProcessCreationTimeFiletime { get; init; }

    /// <summary>Child process creation time; null when the line omits it or it is not a timestamp.</summary>
    public DateTimeOffset? ProcessCreationTimeUtc { get; init; }

    /// <summary>
    /// The time the child emitted the handshake, after the mount succeeded;
    /// null when the line omits it or it is not a timestamp.
    /// </summary>
    public DateTimeOffset? StartedAtUtc { get; init; }
}

/// <summary>A line whose status is <c>failed</c>.</summary>
public sealed record FailedHandshake : HandshakeResult
{
    /// <summary>The error code the child reported.</summary>
    public string? ErrorCode { get; init; }

    /// <summary>Human-readable failure message.</summary>
    public string? Message { get; init; }
}

/// <summary>A line whose status is absent or not a known value.</summary>
public sealed record UnknownHandshake : HandshakeResult
{
}
