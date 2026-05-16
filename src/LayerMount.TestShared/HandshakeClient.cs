using System;
using System.IO;
using System.Text.Json;
using System.Threading;

namespace LayerMount.TestShared;

/// <summary>
/// Parent-side reader for the mount handshake JSON line emitted by
/// a host-adapter child process started under <c>--handshake-child</c>.
/// Shared across consumer test projects so they all deserialize the
/// schema through one code path.
/// </summary>
public static class HandshakeClient
{
    /// <summary>
    /// Reads exactly one newline-terminated JSON line from <paramref name="reader"/>,
    /// bounded by <paramref name="timeout"/>. The read runs on a background thread
    /// so the timeout is enforced without relying on the child to close the pipe.
    /// </summary>
    /// <returns>The parsed <see cref="HandshakeResult"/> on success, or
    /// <c>null</c> if the line was not received before <paramref name="timeout"/>
    /// elapsed (in which case the caller should kill the child).</returns>
    public static HandshakeResult? ReadFromStdout(StreamReader reader, TimeSpan timeout)
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
    /// Parses a single handshake JSON line. Uses defensive field getters so a
    /// missing <c>host</c> on legacy emissions falls through as empty string
    /// rather than throwing -- callers decide whether empty is fatal.
    /// </summary>
    public static HandshakeResult Parse(string line)
    {
        using var doc = JsonDocument.Parse(line);
        var root = doc.RootElement;
        string status = GetStr(root, "status");

        return status switch
        {
            "mounted" => new HandshakeResult
            {
                Kind = HandshakeKind.Mounted,
                StatusRaw = status,
                RawLine = line,
                InstanceId = GetStr(root, "instanceId"),
                MountPoint = GetStr(root, "mountPoint"),
                ControlPipe = GetStr(root, "controlPipe"),
                Pid = GetInt(root, "pid"),
                ProcessCreationTimeFiletime = GetLong(root, "processCreationTimeFiletime"),
                ProcessCreationTimeUtc = GetStrOrNull(root, "processCreationTimeUtc"),
                StartedAtUtc = GetStrOrNull(root, "startedAtUtc"),
                Host = GetStr(root, "host"),
            },
            "failed" => new HandshakeResult
            {
                Kind = HandshakeKind.Failed,
                StatusRaw = status,
                RawLine = line,
                Host = GetStr(root, "host"),
                ErrorCode = GetStrOrNull(root, "error"),
                Message = GetStrOrNull(root, "message"),
            },
            _ => new HandshakeResult
            {
                Kind = HandshakeKind.Unknown,
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
}

/// <summary>
/// Classification of a parsed handshake line, derived from the
/// <c>status</c> field of the handshake schema.
/// </summary>
public enum HandshakeKind
{
    /// <summary>Status field absent or not one of the known values.</summary>
    Unknown,
    /// <summary>Child reported a successful mount.</summary>
    Mounted,
    /// <summary>Child reported a failure before the mount completed.</summary>
    Failed,
}

/// <summary>
/// Parsed handshake payload. Any host adapter's emission is
/// assertable via this single shape.
///
/// <see cref="Host"/> carries the opaque adapter-supplied identifier
/// from the handshake. Empty only on a schema violation; tests should
/// treat empty as a failure, not a missing-field tolerance. Do not
/// enumerate or hard-code expected values -- consumers should treat the
/// field as opaque and let the adapter define it.
/// </summary>
public sealed record HandshakeResult
{
    public HandshakeKind Kind { get; init; }
    public string StatusRaw { get; init; } = string.Empty;
    public string RawLine { get; init; } = string.Empty;

    // mounted fields
    public string InstanceId { get; init; } = string.Empty;
    public string MountPoint { get; init; } = string.Empty;
    public string ControlPipe { get; init; } = string.Empty;
    public int Pid { get; init; }
    public long ProcessCreationTimeFiletime { get; init; }
    public string? ProcessCreationTimeUtc { get; init; }
    public string? StartedAtUtc { get; init; }

    // failed fields
    public string? ErrorCode { get; init; }
    public string? Message { get; init; }

    // present on both mounted + failed
    public string Host { get; init; } = string.Empty;
}
