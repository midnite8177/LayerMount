using System;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace LayerMount.TestShared;

/// <summary>
/// Thin client for the per-instance control pipe at
/// <c>\\.\pipe\layermount-&lt;instanceId&gt;</c>.
///
/// Protocol is one newline-terminated JSON request + one newline-terminated
/// JSON response over a byte-mode pipe. Shared across consumer test
/// projects so they all talk to any host adapter through one code path.
/// </summary>
public static class ControlPipeClient
{
    /// <summary>
    /// Strips the <c>\\.\pipe\</c> or <c>\\?\pipe\</c> prefix off a fully-
    /// qualified pipe path. <see cref="NamedPipeClientStream"/> only takes
    /// the bare name, so the handshake's <c>controlPipe</c> field must be
    /// normalised before use.
    /// </summary>
    public static string StripPipePrefix(string path)
    {
        const string dotPrefix = @"\\.\pipe\";
        const string qmPrefix  = @"\\?\pipe\";
        if (path.StartsWith(dotPrefix, StringComparison.Ordinal))
            return path.Substring(dotPrefix.Length);
        if (path.StartsWith(qmPrefix, StringComparison.Ordinal))
            return path.Substring(qmPrefix.Length);
        return path;
    }

    /// <summary>
    /// Sends <c>{"cmd":"shutdown"}</c> over the control pipe and drains the
    /// acknowledgement. Returns <c>true</c> if the request was written and a
    /// non-empty response was received before <paramref name="timeout"/>
    /// elapsed. Host's shutdown delegate fires after the ack is sent, so a
    /// <c>true</c> return means "shutdown requested; child is unwinding".
    /// Callers must still <c>WaitForExit</c> on the child process.
    /// </summary>
    /// <param name="pipeNameOrFullPath">Either the bare pipe name
    /// (<c>layermount-&lt;id&gt;</c>) or the fully-qualified form
    /// (<c>\\.\pipe\layermount-&lt;id&gt;</c>). Prefix is stripped
    /// automatically.</param>
    /// <param name="timeout">Upper bound on connect + request + response.</param>
    public static bool Shutdown(string pipeNameOrFullPath, TimeSpan timeout)
    {
        string? response = SendRequest(pipeNameOrFullPath, "{\"cmd\":\"shutdown\"}", timeout);
        return !string.IsNullOrEmpty(response);
    }

    /// <summary>
    /// Sends <c>{"cmd":"identity"}</c> and returns the parsed JSON document.
    /// Used by tests that want to assert the <c>host</c> field or pid match
    /// what the handshake advertised. Returns <c>null</c> on any pipe /
    /// parse failure; callers decide severity.
    /// </summary>
    public static JsonDocument? Identity(string pipeNameOrFullPath, TimeSpan timeout)
    {
        string? response = SendRequest(pipeNameOrFullPath, "{\"cmd\":\"identity\"}", timeout);
        if (string.IsNullOrWhiteSpace(response)) return null;
        try { return JsonDocument.Parse(response); }
        catch (JsonException) { return null; }
    }

    /// <summary>
    /// Connect, write a single newline-terminated JSON request, read the
    /// response, and close. Swallows all exceptions on failure and returns
    /// <c>null</c> so individual callers don't have to repeat catch blocks.
    /// </summary>
    private static string? SendRequest(string pipeNameOrFullPath, string requestJson, TimeSpan timeout)
    {
        string name = StripPipePrefix(pipeNameOrFullPath);
        try
        {
            using var client = new NamedPipeClientStream(
                ".", name, PipeDirection.InOut, PipeOptions.None);
            client.Connect((int)timeout.TotalMilliseconds);

            byte[] req = Encoding.UTF8.GetBytes(requestJson + "\n");
            client.Write(req, 0, req.Length);
            client.Flush();

            // Single read is enough because responses are a few hundred bytes
            // at most and byte-mode pipes deliver in whatever chunks arrive.
            // 8 KiB is well above any legitimate control response.
            byte[] buffer = new byte[8192];
            int n = client.Read(buffer, 0, buffer.Length);
            if (n <= 0) return null;
            return Encoding.UTF8.GetString(buffer, 0, n).TrimEnd('\n', '\r');
        }
        catch
        {
            return null;
        }
    }
}
