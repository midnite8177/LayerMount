using System.IO;
using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

/// <summary>
/// Drives each HRESULT category through the managed wrapper and asserts
/// the mapped exception subclass, and whether the failure came from a
/// metacopy fill.
/// </summary>
public sealed class LayerMountExceptionTests
{
    private const uint GENERIC_READ            = 0x80000000u;
    private const uint FILE_READ_ATTRIBUTES    = 0x00000080u;
    private const uint FILE_WRITE_ATTRIBUTES   = 0x00000100u;
    private const uint FILE_ATTRIBUTE_SPARSE   = 0x00000200u;
    private const int  HR_NT_SHARING_VIOLATION = unchecked((int)0xD0000043u);

    private const int AboveMetacopyThreshold = 2 * 1024 * 1024;

    [Fact]
    public void NotFound_OpenMissingFile_ThrowsNotFound()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        var ex = Assert.Throws<LayerMountNotFoundException>(() =>
            mount.OpenFile(@"\absent.txt", grantedAccess: 0x80000000u));
        Assert.True(
            ex.HResult == unchecked((int)0x80070002u) ||
            ex.HResult == unchecked((int)0x80070003u) ||
            ex.HResult == unchecked((int)0xD0000034u) ||
            ex.HResult == unchecked((int)0xD000003Au),
            $"Unexpected HRESULT 0x{ex.HResult:X8} for missing-file error");
        Assert.False(string.IsNullOrEmpty(ex.Context));
    }

    [Fact]
    public void OpenForRead_MetacopyFillFailsOnHeldLower_ReportsFromMetacopyFill()
    {
        using var env = new TempLayerEnvironment(1);
        string lowerPath = Path.Combine(env.Lower(0), "big.bin");
        File.WriteAllBytes(lowerPath, CreateBytes(AboveMetacopyThreshold));
        using var mount = LayerMount.Create(env.BuildConfig());
        using (var staged = mount.OpenFile(@"\big.bin", FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES))
        {
            Assert.NotEqual(0u, staged.Info.FileAttributes & FILE_ATTRIBUTE_SPARSE);
        }
        using var writer = new FileStream(lowerPath, FileMode.Open, FileAccess.Write, FileShare.ReadWrite);

        var ex = Assert.ThrowsAny<LayerMountException>(() =>
            mount.OpenFile(@"\big.bin", GENERIC_READ));

        Assert.Equal(HR_NT_SHARING_VIOLATION, ex.HResult);
        Assert.True(ex.FromMetacopyFill);
        Assert.Contains("fill", ex.Message);
    }

    [Fact]
    public void OpenForRead_PlainFileHeldWithoutSharing_SameStatusIsNotFromMetacopyFill()
    {
        using var env = new TempLayerEnvironment(0);
        string upperPath = Path.Combine(env.Upper, "plain.txt");
        File.WriteAllText(upperPath, "plain bytes");
        using var mount = LayerMount.Create(env.BuildConfig());
        using var exclusive = new FileStream(upperPath, FileMode.Open, FileAccess.Read, FileShare.None);

        var ex = Assert.ThrowsAny<LayerMountException>(() =>
            mount.OpenFile(@"\plain.txt", GENERIC_READ));

        Assert.Equal(HR_NT_SHARING_VIOLATION, ex.HResult);
        Assert.False(ex.FromMetacopyFill);
    }

    [Fact]
    public void InvalidHandle_UseOfDisposedLayerMount_ThrowsInvalidHandle()
    {
        using var env = new TempLayerEnvironment(0);
        var mount = LayerMount.Create(env.BuildConfig());
        mount.Dispose();

        Assert.Throws<LayerMountInvalidHandleException>(() =>
            mount.GetStats());
    }

    [Fact]
    public void BuildMessage_IncludesHResultAndContext()
    {
        using var env = new TempLayerEnvironment(0);
        var mount = LayerMount.Create(env.BuildConfig());
        mount.Dispose();

        var ex = Assert.Throws<LayerMountInvalidHandleException>(() =>
            mount.GetStats());
        Assert.Contains("HRESULT=0x", ex.Message);
        Assert.False(string.IsNullOrEmpty(ex.Context));
    }

    private static byte[] CreateBytes(int length)
    {
        var bytes = new byte[length];
        for (int i = 0; i < length; ++i)
        {
            bytes[i] = (byte)('A' + (i % 26));
        }
        return bytes;
    }
}
