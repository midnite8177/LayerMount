using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Versioning;
using Xunit;

namespace LayerMount.Tests;

[SupportedOSPlatform("windows")]
public sealed class MergeDirectoryTests
{
    [Fact]
    public void MergeDirectory_CallbackReturnsFalse_StopsWithoutThrowing()
    {
        using var env = new TempLayerEnvironment(0);
        env.WriteUpperFile(@"dir\a.txt", "x");
        env.WriteUpperFile(@"dir\b.txt", "x");
        env.WriteUpperFile(@"dir\c.txt", "x");
        using var mount = LayerMount.Create(env.BuildConfig());

        int calls = 0;
        mount.MergeDirectory(@"\dir", (name, info) =>
        {
            calls++;
            return false;
        });

        Assert.Equal(1, calls);
    }

    [Fact]
    public void MergeDirectory_CallbackReturnsTrue_SeesEveryEntry()
    {
        using var env = new TempLayerEnvironment(0);
        env.WriteUpperFile(@"dir\a.txt", "x");
        env.WriteUpperFile(@"dir\b.txt", "x");
        env.WriteUpperFile(@"dir\c.txt", "x");
        using var mount = LayerMount.Create(env.BuildConfig());

        var names = new List<string>();
        mount.MergeDirectory(@"\dir", (name, info) =>
        {
            names.Add(name);
            return true;
        });

        Assert.Equal(new[] { "a.txt", "b.txt", "c.txt" }, names);
    }

    [Fact]
    public void MergeDirectory_EmptyCheckOnDirectoryWithEntries_ReportsNotEmpty()
    {
        using var env = new TempLayerEnvironment(1);
        env.WriteLowerFile(0, @"dir\a.txt", "x");
        env.WriteUpperFile(@"dir\b.txt", "x");
        Directory.CreateDirectory(Path.Combine(env.Upper, "empty"));
        using var mount = LayerMount.Create(env.BuildConfig());

        Assert.False(IsEmpty(mount, @"\dir"));
        Assert.True(IsEmpty(mount, @"\empty"));
    }

    [Fact]
    public void MergeDirectory_CallbackThrows_ThrowsLayerMountException()
    {
        using var env = new TempLayerEnvironment(0);
        env.WriteUpperFile(@"dir\a.txt", "x");
        using var mount = LayerMount.Create(env.BuildConfig());

        Assert.ThrowsAny<LayerMountException>(() =>
            mount.MergeDirectory(@"\dir", (name, info) =>
                throw new InvalidOperationException("callback failed")));
    }

    [Fact]
    public void MergeDirectory_UpperScanFails_ThrowsWithoutCallingBack()
    {
        using var env = new TempLayerEnvironment(1);
        env.WriteUpperFile(@"dir\up.txt", "x");
        env.WriteLowerFile(0, @"dir\below.txt", "x");
        using var denied = new DirectoryListingDenied(Path.Combine(env.Upper, "dir"));
        using var mount = LayerMount.Create(env.BuildConfig());
        using var noBackupPrivilege = new BackupPrivilegeDisabledOnThread();
        Assert.Throws<UnauthorizedAccessException>(() =>
            Directory.EnumerateFileSystemEntries(Path.Combine(env.Upper, "dir")).ToList());

        int calls = 0;
        Assert.ThrowsAny<LayerMountException>(() =>
            mount.MergeDirectory(@"\dir", (name, info) =>
            {
                calls++;
                return false;
            }));

        Assert.Equal(0, calls);
    }

    private static bool IsEmpty(LayerMount mount, string dir)
    {
        bool empty = true;
        mount.MergeDirectory(dir, (name, info) =>
        {
            if (name == "." || name == "..")
            {
                return true;
            }
            empty = false;
            return false;
        });
        return empty;
    }
}
