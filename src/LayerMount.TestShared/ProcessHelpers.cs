using System;
using System.Threading;

namespace LayerMount.TestShared;

/// <summary>
/// Small timing / polling utilities shared by both test projects' E2E
/// flows. Kept minimal -- anything more structural (retry with backoff,
/// async awaitables) belongs in a proper helper, not here.
/// </summary>
public static class ProcessHelpers
{
    /// <summary>
    /// Polls <paramref name="predicate"/> every 50 ms until it returns
    /// <c>true</c> or <paramref name="timeout"/> elapses. Returns the final
    /// predicate evaluation so callers can assert success.
    /// </summary>
    public static bool WaitUntil(Func<bool> predicate, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (predicate()) return true;
            Thread.Sleep(50);
        }
        return predicate();
    }
}
