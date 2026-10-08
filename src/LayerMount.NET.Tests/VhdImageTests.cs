using System.IO;
using System.Runtime.Versioning;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Threading;
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

        vhd.Detach();
    }

    [SkippableFact]
    [SupportedOSPlatform("windows")]
    public void Export_FileThatDeniesRead_ThrowsAccessDeniedNamingTheFile()
    {
        ElevationHelper.SkipIfNotElevated("VHD attach requires admin");

        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        string source = Path.Combine(env.Root, "source");
        Directory.CreateDirectory(source);
        File.WriteAllText(Path.Combine(source, "keep.txt"), "keep");
        string vhdPath = Path.Combine(env.Root, "denied.vhdx");
        mount.Vhd.Import(source, vhdPath, 0);

        using (VhdImage vhd = mount.Vhd.Open(vhdPath, readOnly: false,
                   suppressDriveLetter: true, lifetime: VhdAttachLifetime.ProcessScoped))
        {
            vhd.Attach();
            string secret = VolumeRootOf(vhd) + "secret.txt";
            File.WriteAllText(secret, "secret");
            var file = new FileInfo(secret);
            FileSecurity security = file.GetAccessControl(AccessControlSections.Access);
            security.AddAccessRule(new FileSystemAccessRule(
                new SecurityIdentifier(WellKnownSidType.WorldSid, null),
                FileSystemRights.ReadData, AccessControlType.Deny));
            file.SetAccessControl(security);
        }

        var thrown = Assert.Throws<LayerMountAccessDeniedException>(() =>
            mount.Vhd.Export(vhdPath, Path.Combine(env.Root, "dst")));
        Assert.Contains(@"'\secret.txt'", thrown.Message);
    }

    // The volume can show up after the attach returns.
    private static string VolumeRootOf(VhdImage vhd)
    {
        for (int attempt = 0; ; ++attempt)
        {
            try
            {
                string root = vhd.GetVolumeGuid();
                return root.EndsWith('\\') ? root : root + '\\';
            }
            catch (LayerMountException) when (attempt < 40)
            {
                Thread.Sleep(250);
            }
        }
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
