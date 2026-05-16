using System;
using System.IO;
using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

public sealed class LayerMountTests
{
    [Fact]
    public void GetVersion_ReturnsAbiVersionOne()
    {
        var (_, _, _, abi) = LayerMount.GetVersion();
        Assert.Equal(1u, abi);
    }

    [Fact]
    public void Create_ValidConfig_Succeeds()
    {
        using var env = new TempLayerEnvironment(lowerCount: 1);
        using var mount = LayerMount.Create(env.BuildConfig());
        Assert.NotNull(mount);

        var stats = mount.GetStats();
        Assert.Equal(0ul, stats.ReadCount);
        Assert.Equal(0ul, stats.WriteCount);
    }

    [Fact]
    public void Create_MissingUpper_Throws()
    {
        // UpperPath empty/null -> ArgumentException at the managed boundary.
        var cfg = new LayerMountConfig
        {
            UpperPath   = string.Empty,
            WorkDirPath = Path.GetTempPath(),
        };
        Assert.Throws<ArgumentException>(() => LayerMount.Create(cfg));
    }

    [Fact]
    public void Create_NonexistentUpper_ThrowsLayerMountException()
    {
        using var env = new TempLayerEnvironment(0);
        var cfg = new LayerMountConfig
        {
            UpperPath   = Path.Combine(env.Root, "does_not_exist"),
            WorkDirPath = env.Work,
        };
        var ex = Assert.Throws<LayerMountException>(() => LayerMount.Create(cfg));
        // E_INVALIDARG = 0x80070057 per HResultGuard mapping default.
        Assert.Equal(unchecked((int)0x80070057u), ex.HResult);
        Assert.False(string.IsNullOrEmpty(ex.Context));
    }

    [Fact]
    public void Dispose_Twice_NoThrow()
    {
        using var env = new TempLayerEnvironment(0);
        var mount = LayerMount.Create(env.BuildConfig());
        mount.Dispose();
        mount.Dispose(); // idempotent
    }

    [Fact]
    public void GetVolumeInfo_ReturnslayermountLabel()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        var vi = mount.GetVolumeInfo();
        Assert.Equal("LayerMount", vi.VolumeLabel);
    }

    [Fact]
    public void ResolvePath_LowerFile_MatchesLowerLayer()
    {
        using var env = new TempLayerEnvironment(1);
        env.WriteLowerFile(0, "data.txt", "hi");
        using var mount = LayerMount.Create(env.BuildConfig());

        var rp = mount.ResolvePath(@"\data.txt");
        Assert.Equal(LayerSource.Lower, rp.Source);
        Assert.Equal(0, rp.LowerIndex);
        Assert.EndsWith("data.txt", rp.AbsolutePath,
            StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void EnsureInUpperLayer_CopiesUp()
    {
        using var env = new TempLayerEnvironment(1);
        env.WriteLowerFile(0, "promote.txt", "from-lower");
        using var mount = LayerMount.Create(env.BuildConfig());

        mount.EnsureInUpperLayer(@"\promote.txt");
        string upperFile = Path.Combine(env.Upper, "promote.txt");
        Assert.True(File.Exists(upperFile));
    }
}
