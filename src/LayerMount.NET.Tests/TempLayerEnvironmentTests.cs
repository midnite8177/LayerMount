using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.Versioning;
using Xunit;

namespace LayerMount.Tests;

[SupportedOSPlatform("windows")]
public sealed class TempLayerEnvironmentTests
{
    private const string KeepFileName = "keep.txt";

    [Fact]
    public void Dispose_WithAJunctionOutOfTheRoot_LeavesTheTargetUntouched()
    {
        using var outside = new TempScratchDir("JunctionTarget");
        var keep = outside.Sub(KeepFileName);
        File.WriteAllText(keep, "keep me");
        File.SetAttributes(keep, FileAttributes.ReadOnly);

        string root;
        using (var env = new TempLayerEnvironment(0))
        {
            root = env.Root;
            CreateRealFilesystemJunction(
                Path.Combine(env.Upper, "escape"), outside.Root);
        }

        Assert.True(
            File.Exists(keep),
            $"Dispose must not delete {keep}. That path is outside the fixture root.");
        Assert.True(
            (File.GetAttributes(keep) & FileAttributes.ReadOnly) != 0,
            $"Dispose must not clear the read-only attribute on {keep}. "
                + "That path is outside the fixture root.");
        Assert.False(Directory.Exists(root), $"Dispose must delete {root}.");
    }

    [Theory]
    [InlineData(FileAttributes.ReadOnly)]
    [InlineData(FileAttributes.ReadOnly | FileAttributes.Hidden)]
    [InlineData(FileAttributes.ReadOnly | FileAttributes.System)]
    public void Dispose_WithAReadOnlyFileInASubdirectory_RemovesTheRoot(
        FileAttributes attributes)
    {
        string root;
        using (var env = new TempLayerEnvironment(0))
        {
            root = env.Root;
            var directory = Path.Combine(env.Upper, "a", "b");
            Directory.CreateDirectory(directory);
            var locked = Path.Combine(directory, "locked.txt");
            File.WriteAllText(locked, "locked");
            File.SetAttributes(locked, attributes);
        }

        Assert.False(
            Directory.Exists(root),
            $"Dispose must delete {root}. It holds one file with the attributes {attributes}.");
    }

    // A junction needs no SeCreateSymbolicLinkPrivilege, so this test needs
    // no elevation.
    private static void CreateRealFilesystemJunction(
        string junction, string target)
    {
        var info = new ProcessStartInfo
        {
            FileName = "cmd.exe",
            Arguments = $"/c mklink /J \"{junction}\" \"{target}\"",
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };

        using var process = Process.Start(info)
            ?? throw new InvalidOperationException("cmd.exe did not start");
        var output = process.StandardOutput.ReadToEnd();
        var error = process.StandardError.ReadToEnd();
        process.WaitForExit();

        Assert.True(
            process.ExitCode == 0,
            $"mklink /J \"{junction}\" \"{target}\" exited with "
                + $"{process.ExitCode}: {output}{error}");
        Assert.True(
            (new DirectoryInfo(junction).Attributes & FileAttributes.ReparsePoint) != 0,
            $"{junction} is not a reparse point.");
    }
}
