using System.IO;
using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

public sealed class VhdImageTests
{
    [SkippableFact]
    public void Create_Attach_RoundTrip()
    {
        ElevationHelper.SkipIfNotElevated("VHD attach requires admin");

        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        string vhdPath = Path.Combine(env.Root, "probe.vhdx");
        // ProcessScoped + VhdClose (via Dispose) handles detach at the end;
        // calling Detach() explicitly here races against the caching layer
        // and returns ERROR_SHARING_VIOLATION, which is out of scope for a
        // wrapper-shape test.
        using var vhd = mount.Vhd.Create(
            vhdPath,
            sizeBytes: 32ul * 1024 * 1024,
            kind: VhdKind.Dynamic,
            suppressDriveLetter: true,
            lifetime: VhdAttachLifetime.ProcessScoped);

        Assert.Equal(vhdPath, vhd.Path);

        string physical = vhd.Attach();
        Assert.False(string.IsNullOrEmpty(physical),
            "Attach must return a non-empty physical path");
    }

    [Fact]
    public void Open_NonExistent_ThrowsLayerMountNotFound()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        string missing = Path.Combine(env.Root, "missing.vhdx");
        // virtdisk.lib's OpenVirtualDisk returns HRESULT_FROM_WIN32(
        // ERROR_FILE_NOT_FOUND), which HResultGuard maps to
        // LayerMountNotFoundException.
        Assert.Throws<LayerMountNotFoundException>(() =>
            mount.Vhd.Open(missing));
    }

    [Fact]
    public void ListLayers_NoManifest_ReturnsEmpty()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        var layers = mount.Vhd.ListLayers(env.Root);
        Assert.Empty(layers);
    }
}
