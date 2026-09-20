using System;
using System.Security.Principal;
using Xunit;

namespace LayerMount.TestShared;

/// <summary>
/// Skips a test that needs an elevated process. A test that needs
/// elevation calls <see cref="SkipIfNotElevated"/> first. On a
/// non-Windows OS the process never counts as elevated.
/// </summary>
public static class ElevationHelper
{
    public static bool IsElevated()
    {
        if (!OperatingSystem.IsWindows()) return false;
        using var identity = WindowsIdentity.GetCurrent();
        var principal = new WindowsPrincipal(identity);
        return principal.IsInRole(WindowsBuiltInRole.Administrator);
    }

    /// <summary>
    /// Throws <see cref="SkipException"/> when the process is not elevated,
    /// with <paramref name="why"/> in the skip message. Returns otherwise.
    /// </summary>
    public static void SkipIfNotElevated(string why)
    {
        Skip.IfNot(IsElevated(), $"Skipping: requires elevation ({why})");
    }
}
