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

    // Production callers may hand LayerMount the
    // already-expanded NT access mask — GENERIC_* bits are mapped to
    // FILE_GENERIC_* by the kernel before reaching the host. Tests that
    // need to exercise the write-intent paths (HasWriteAccess gate,
    // CoW trigger) must use the expanded form.
    private const uint SYNCHRONIZE              = 0x00100000u;
    private const uint READ_CONTROL             = 0x00020000u;
    private const uint FILE_READ_DATA           = 0x00000001u;
    private const uint FILE_WRITE_DATA          = 0x00000002u;
    private const uint FILE_APPEND_DATA         = 0x00000004u;
    private const uint FILE_READ_EA             = 0x00000008u;
    private const uint FILE_WRITE_EA            = 0x00000010u;
    private const uint FILE_READ_ATTRIBUTES     = 0x00000080u;
    private const uint FILE_WRITE_ATTRIBUTES    = 0x00000100u;
    private const uint FILE_GENERIC_READ =
        SYNCHRONIZE | READ_CONTROL | FILE_READ_DATA | FILE_READ_EA | FILE_READ_ATTRIBUTES;
    private const uint FILE_GENERIC_WRITE =
        SYNCHRONIZE | READ_CONTROL | FILE_WRITE_DATA | FILE_APPEND_DATA |
        FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;

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

    [Fact]
    public void SetFileInfo_TruncateExistingUpperFile_Succeeds()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        using (var file = mount.CreateFile(
                   @"\truncate.txt",
                   createOptions: 0u,
                   grantedAccess: GENERIC_READ | GENERIC_WRITE,
                   fileAttributes: FILE_ATTRIBUTE_NORMAL))
        {
            byte[] payload = Encoding.UTF8.GetBytes("twelve-chars");
            file.Write(offset: 0, payload);
            FileInfoSnapshot info = file.GetFileInfo();
            Assert.Equal((ulong)payload.Length, info.FileSize);

            file.SetFileInfo(fileSize: 0);
            info = file.GetFileInfo();
            Assert.Equal(0UL, info.FileSize);
        }
    }

    [Fact]
    public void Overwrite_OnExistingUpperFile_TruncatesAndSucceeds()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        using (var file = mount.CreateFile(
                   @"\overwrite.txt",
                   createOptions: 0u,
                   grantedAccess: GENERIC_READ | GENERIC_WRITE,
                   fileAttributes: FILE_ATTRIBUTE_NORMAL))
        {
            file.Write(offset: 0, Encoding.UTF8.GetBytes("initial-content"));
            file.Overwrite(
                fileAttributes: FILE_ATTRIBUTE_NORMAL,
                replaceAttributes: true,
                allocationSize: 0,
                originatorPid: 0);
            FileInfoSnapshot info = file.GetFileInfo();
            Assert.Equal(0UL, info.FileSize);
        }
    }

    [Fact]
    public void SetFileInfo_TruncateLowerOnlyFile_CoWsAndSucceeds()
    {
        using var env = new TempLayerEnvironment(1);
        env.WriteLowerFile(0, "seeded.txt", "lower-content");
        using var mount = LayerMount.Create(env.BuildConfig());

        using (var file = mount.OpenFile(
                   @"\seeded.txt",
                   grantedAccess: FILE_GENERIC_READ | FILE_GENERIC_WRITE,
                   createOptions: 0u))
        {
            file.SetFileInfo(fileSize: 0);
            FileInfoSnapshot info = file.GetFileInfo();
            Assert.Equal(0UL, info.FileSize);
        }
    }

    [Fact]
    public void Overwrite_LowerOnlyFile_CoWsAndTruncates()
    {
        using var env = new TempLayerEnvironment(1);
        env.WriteLowerFile(0, "lower-doc.txt", "lower-original");
        using var mount = LayerMount.Create(env.BuildConfig());

        using (var file = mount.OpenFile(
                   @"\lower-doc.txt",
                   grantedAccess: FILE_GENERIC_READ | FILE_GENERIC_WRITE,
                   createOptions: 0u))
        {
            file.Overwrite(
                fileAttributes: FILE_ATTRIBUTE_NORMAL,
                replaceAttributes: true,
                allocationSize: 0,
                originatorPid: 0);
            FileInfoSnapshot info = file.GetFileInfo();
            Assert.Equal(0UL, info.FileSize);
        }
    }
}
