using System.IO;
using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

/// <summary>
/// Drives each HRESULT category through the managed wrapper and asserts
/// the mapped exception subclass (FR-40).
/// </summary>
public sealed class LayerMountExceptionTests
{
    [Fact]
    public void NotFound_OpenMissingFile_ThrowsNotFound()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        // The engine returns file-not-found as HRESULT_FROM_NT(STATUS_OBJECT_NAME_NOT_FOUND)
        // (0xD0000034). HResultGuard maps both the Win32 and NT-facility
        // flavors to LayerMountNotFoundException -- prior to that mapping,
        // GetFileInfo probes in downstream host adapters fell through to
        // STATUS_UNSUCCESSFUL and clients saw ERROR_GEN_FAILURE on every
        // create-disposition open.
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
        // Arg order flips from MSTest's StringAssert.Contains(value, substring)
        // to xUnit's Assert.Contains(substring, value).
        Assert.Contains("HRESULT=0x", ex.Message);
        Assert.False(string.IsNullOrEmpty(ex.Context));
    }
}
