using System;
using System.Text;
using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

public sealed class LayerMountFileTests
{
    private const uint FILE_ATTRIBUTE_NORMAL = 0x00000080u;
    private const uint GENERIC_READ          = 0x80000000u;
    private const uint GENERIC_WRITE         = 0x40000000u;
    private const uint DELETE                = 0x00010000u;

    [Fact]
    public void CreateFile_Write_Read_Close_RoundTrip()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        using (var file = mount.CreateFile(
                   @"\roundtrip.txt",
                   createOptions: 0u,
                   grantedAccess: GENERIC_READ | GENERIC_WRITE,
                   fileAttributes: FILE_ATTRIBUTE_NORMAL))
        {
            byte[] payload = Encoding.UTF8.GetBytes("managed hello");
            uint written = file.Write(offset: 0, payload);
            Assert.Equal((uint)payload.Length, written);

            byte[] readback = new byte[32];
            uint readCount = file.Read(offset: 0, readback);
            Assert.Equal((uint)payload.Length, readCount);
            byte[] prefix = new byte[readCount];
            Array.Copy(readback, prefix, readCount);
            Assert.Equal("managed hello", Encoding.UTF8.GetString(prefix));
        }
    }

    [Fact]
    public void Dispose_Twice_NoThrow()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        var file = mount.CreateFile(@"\twice.txt", 0u,
            GENERIC_READ | GENERIC_WRITE | DELETE, FILE_ATTRIBUTE_NORMAL);
        file.Dispose();
        file.Dispose(); // idempotent
    }

    [Fact]
    public void Read_AfterClose_Throws()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        var file = mount.CreateFile(@"\closed.txt", 0u,
            GENERIC_READ | GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL);
        file.Dispose();
        Assert.True(file.IsClosed);

        byte[] buf = new byte[8];
        // The SafeHandle is closed; subsequent Read goes through a handle
        // the native side no longer knows about, so it must throw.
        Assert.Throws<LayerMountInvalidHandleException>(() =>
            file.Read(0, buf));
    }
}
