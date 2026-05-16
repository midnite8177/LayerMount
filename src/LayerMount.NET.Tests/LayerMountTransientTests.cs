// Round-trip tests for LayerMount.CreateTransient. A short-lived
// LM_HANDLE rooted at a single workDir, usable by
// vhd/vss/layer CLI subcommands to reach LayerMountVhd* / LayerMountVss* /
// LayerMountImage* primitives without mounting a filesystem.
//
// None of these tests require Administrator -- a transient overlay only
// reserves an in-process handle table slot and creates workDir on disk.

using System;
using System.IO;
using Xunit;

namespace LayerMount.Tests;

public sealed class LayerMountTransientTests
{
    [Fact]
    public void CreateTransient_FreshWorkDir_ReturnsUsableLayerMount()
    {
        using var scratch = new TempScratchDir();
        string workDir = scratch.Sub("transient");

        using var mount = LayerMount.CreateTransient(workDir);

        Assert.NotNull(mount);
        Assert.NotNull(mount.Vhd);
        Assert.NotNull(mount.Vss);
        Assert.NotNull(mount.Images);
        Assert.True(Directory.Exists(workDir),
            "CreateTransient must create workDir on demand (matches native best-effort semantics)");
    }

    [Fact]
    public void CreateTransient_ExistingWorkDir_Succeeds()
    {
        using var scratch = new TempScratchDir();
        string workDir = scratch.Sub("already-exists");
        Directory.CreateDirectory(workDir);

        using var mount = LayerMount.CreateTransient(workDir);

        Assert.NotNull(mount);
    }

    [Fact]
    public void CreateTransient_NullWorkDir_Throws()
    {
        Assert.Throws<ArgumentNullException>(
            () => LayerMount.CreateTransient(null!));
    }

    [Fact]
    public void CreateTransient_EmptyWorkDir_Throws()
    {
        Assert.Throws<ArgumentException>(
            () => LayerMount.CreateTransient(""));
    }

    [Fact]
    public void CreateTransient_DoubleDispose_IsIdempotent()
    {
        using var scratch = new TempScratchDir();
        string workDir = scratch.Sub("dispose-twice");

        var mount = LayerMount.CreateTransient(workDir);
        mount.Dispose();
        mount.Dispose();
    }

    [Fact]
    public void CreateTransient_VssList_ReturnsSnapshotCollection()
    {
        using var scratch = new TempScratchDir();
        string workDir = scratch.Sub("vss-probe");

        using var mount = LayerMount.CreateTransient(workDir);

        try
        {
            var snapshots = mount.Vss.ListSnapshots();
            Assert.NotNull(snapshots);
        }
        catch (LayerMountException ex) when (Support.VssEnvironment.IsEnvironmentalHResult(ex.HResult))
        {
            // VSS service is absent / blocked / caller not elevated on
            // this host; the wrapper still round-tripped the failure
            // correctly, which is what this test really verifies.
        }
    }

    private sealed class TempScratchDir : IDisposable
    {
        public string Root { get; }

        public TempScratchDir()
        {
            Root = Path.Combine(Path.GetTempPath(),
                "LayerMountTr_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(Root);
        }

        public string Sub(string name) => Path.Combine(Root, name);

        public void Dispose()
        {
            try { Directory.Delete(Root, recursive: true); }
            catch { /* best-effort */ }
        }
    }
}
