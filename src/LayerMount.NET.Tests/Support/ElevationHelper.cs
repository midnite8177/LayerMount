using System.Runtime.InteropServices;
using System.Security.Principal;
using Xunit;

namespace LayerMount.Tests.Support;

/// <summary>
/// Helpers for admin-gated test paths. Mounting, VSS snapshots, and VHD
/// attach all require elevation; tests that exercise those paths call
/// <see cref="SkipIfNotElevated"/> first.
/// </summary>
internal static class ElevationHelper
{
    public static bool IsElevated()
    {
        if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows)) return false;
        using var identity = WindowsIdentity.GetCurrent();
        var principal = new WindowsPrincipal(identity);
        return principal.IsInRole(WindowsBuiltInRole.Administrator);
    }

    public static void SkipIfNotElevated(string why)
    {
        Skip.IfNot(IsElevated(), $"Skipping: requires elevation ({why})");
    }
}
