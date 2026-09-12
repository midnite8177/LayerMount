using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Runtime.Versioning;
using System.Text;
using System.Threading;
using Xunit;

namespace LayerMount.Tests;

[SupportedOSPlatform("windows")]
public sealed class ReparsePointTests
{
    private const uint FILE_ATTRIBUTE_NORMAL = 0x00000080u;
    private const uint GENERIC_READ          = 0x80000000u;
    private const uint GENERIC_WRITE         = 0x40000000u;

    private const uint IO_REPARSE_TAG_MOUNT_POINT = 0xA0000003u;

    // REPARSE_DATA_BUFFER: tag (4), ReparseDataLength (2), Reserved (2).
    private const int ReparseHeaderBytes = 8;

    // MountPointReparseBuffer: SubstituteNameOffset, SubstituteNameLength,
    // PrintNameOffset, PrintNameLength.
    private const int MountPointFieldBytes = 8;

    private const int PathBufferOffset = ReparseHeaderBytes + MountPointFieldBytes;

    // HRESULT_FROM_NT(STATUS_NOT_A_REPARSE_POINT).
    private const int HRESULT_FROM_NT_NOT_A_REPARSE_POINT = unchecked((int)0xD0000275u);

    private const int ConcurrentReadCount = 300;

    private const int LongTargetLength = 2000;

    [Fact]
    public void GetReparsePoint_OnJunction_ReturnsExactlyWhatSetReparsePointWrote()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        byte[] written = CreateJunction(env, mount, @"\link", Path.Combine(env.Root, "target"));

        byte[] read = mount.GetReparsePoint(@"\link");

        Assert.Equal(written, read);
    }

    [Fact]
    public void GetReparsePoint_OnJunctionWithLongTarget_ReturnsTheWholeDescriptor()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        string target = BuildLongTarget(env.Root, LongTargetLength);
        byte[] written = CreateJunction(env, mount, @"\longlink", target);

        byte[] read = mount.GetReparsePoint(@"\longlink");

        Assert.Equal(written.Length, read.Length);
        AssertWellFormedMountPoint(read);
        Assert.Equal(SubstituteNameOf(target), ReadSubstituteName(read));
    }

    [Fact]
    public void GetReparsePoint_AfterEachRewrite_ReturnsTheCurrentDescriptor()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        string shortTarget = Path.Combine(env.Root, "short");
        string longTarget = BuildLongTarget(env.Root, LongTargetLength);

        byte[] written = CreateJunction(env, mount, @"\rewritten", shortTarget);
        Assert.Equal(written, mount.GetReparsePoint(@"\rewritten"));

        written = BuildMountPointBuffer(longTarget);
        mount.SetReparsePoint(@"\rewritten", written);
        Assert.Equal(written, mount.GetReparsePoint(@"\rewritten"));

        written = BuildMountPointBuffer(shortTarget);
        mount.SetReparsePoint(@"\rewritten", written);
        Assert.Equal(written, mount.GetReparsePoint(@"\rewritten"));
    }

    [Fact]
    public void GetReparsePoint_OnPlainFile_Throws()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        using (mount.CreateFile(
            @"\plain.txt", createOptions: 0u,
            grantedAccess: GENERIC_READ | GENERIC_WRITE,
            fileAttributes: FILE_ATTRIBUTE_NORMAL))
        {
        }

        var ex = Assert.Throws<LayerMountException>(() => mount.GetReparsePoint(@"\plain.txt"));

        Assert.Equal(HRESULT_FROM_NT_NOT_A_REPARSE_POINT, ex.HResult);
    }

    [Fact]
    public void GetReparsePoint_OnMissingPath_ThrowsNotFound()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        Assert.Throws<LayerMountNotFoundException>(() => mount.GetReparsePoint(@"\nothing-here"));
    }

    // Exercises the window between the size probe and the fill call. It
    // cannot force a rewrite to land inside that window, so it proves only
    // that a reader running concurrently with a rewrite keeps returning one
    // of the two descriptors the writer alternates between. If this test
    // flakes, that is a real finding; do not loosen the assertions.
    [Fact]
    public void GetReparsePoint_WhileAnotherThreadRewritesTheTarget_NeverFails()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        string shortTarget = Path.Combine(env.Root, "short");
        string longTarget = BuildLongTarget(env.Root, LongTargetLength);
        byte[] shortDescriptor = BuildMountPointBuffer(shortTarget);
        byte[] longDescriptor = BuildMountPointBuffer(longTarget);
        CreateJunction(env, mount, @"\raced", shortTarget);

        using var stopWriter = new CancellationTokenSource();
        Exception? writerFailure = null;
        var writer = new Thread(() =>
        {
            try
            {
                for (int i = 0; !stopWriter.IsCancellationRequested; i++)
                {
                    mount.SetReparsePoint(
                        @"\raced", (i & 1) == 0 ? longDescriptor : shortDescriptor);
                }
            }
            catch (Exception ex)
            {
                writerFailure = ex;
            }
        })
        {
            IsBackground = true,
        };

        var observed = new List<string>(ConcurrentReadCount);
        writer.Start();
        try
        {
            for (int i = 0; i < ConcurrentReadCount; i++)
            {
                byte[] descriptor = mount.GetReparsePoint(@"\raced");
                AssertWellFormedMountPoint(descriptor);
                observed.Add(ReadSubstituteName(descriptor));
            }
        }
        finally
        {
            stopWriter.Cancel();
            writer.Join();
        }

        Assert.True(writerFailure is null, writerFailure?.ToString() ?? string.Empty);
        string shortName = SubstituteNameOf(shortTarget);
        string longName = SubstituteNameOf(longTarget);
        foreach (string name in observed)
        {
            Assert.True(
                name == shortName || name == longName,
                $"read back a substitute name of {name.Length} chars that was never written");
        }
    }

    private static string SubstituteNameOf(string target) => @"\??\" + target;

    private static string BuildLongTarget(string root, int minimumLength)
    {
        var target = new StringBuilder(root);
        while (target.Length < minimumLength)
        {
            target.Append('\\').Append(new string('s', 40));
        }
        return target.ToString();
    }

    // An IO_REPARSE_TAG_MOUNT_POINT descriptor naming <target> as its
    // substitute name, with an empty print name. The path buffer holds the
    // substitute name, its terminator, and the terminator of the print name.
    private static byte[] BuildMountPointBuffer(string target)
    {
        string substituteName = SubstituteNameOf(target);
        int nameBytes = substituteName.Length * sizeof(char);
        int pathBytes = nameBytes + (2 * sizeof(char));
        int reparseDataLength = MountPointFieldBytes + pathBytes;
        byte[] buffer = new byte[ReparseHeaderBytes + reparseDataLength];

        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(0), IO_REPARSE_TAG_MOUNT_POINT);
        BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(4), (ushort)reparseDataLength);
        BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(6), 0);
        BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(8), 0);
        BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(10), (ushort)nameBytes);
        BinaryPrimitives.WriteUInt16LittleEndian(
            buffer.AsSpan(12), (ushort)(nameBytes + sizeof(char)));
        BinaryPrimitives.WriteUInt16LittleEndian(buffer.AsSpan(14), 0);
        Encoding.Unicode.GetBytes(substituteName, buffer.AsSpan(PathBufferOffset));
        return buffer;
    }

    // The array holds one whole mount-point descriptor and nothing else: the
    // tag is the mount-point tag, and the descriptor's own ReparseDataLength
    // accounts for every byte past the header.
    private static void AssertWellFormedMountPoint(byte[] descriptor)
    {
        Assert.True(
            descriptor.Length > PathBufferOffset,
            $"a mount-point descriptor exceeds {PathBufferOffset} bytes, got {descriptor.Length}");
        Assert.Equal(
            IO_REPARSE_TAG_MOUNT_POINT,
            BinaryPrimitives.ReadUInt32LittleEndian(descriptor.AsSpan(0)));
        Assert.Equal(
            descriptor.Length - ReparseHeaderBytes,
            BinaryPrimitives.ReadUInt16LittleEndian(descriptor.AsSpan(4)));
    }

    private static string ReadSubstituteName(byte[] descriptor)
    {
        int nameOffset = BinaryPrimitives.ReadUInt16LittleEndian(descriptor.AsSpan(8));
        int nameBytes = BinaryPrimitives.ReadUInt16LittleEndian(descriptor.AsSpan(10));
        return Encoding.Unicode.GetString(descriptor, PathBufferOffset + nameOffset, nameBytes);
    }

    private static byte[] CreateJunction(
        TempLayerEnvironment env, LayerMount mount, string relativePath, string target)
    {
        Directory.CreateDirectory(Path.Combine(env.Upper, relativePath.TrimStart('\\')));
        byte[] descriptor = BuildMountPointBuffer(target);
        mount.SetReparsePoint(relativePath, descriptor);
        return descriptor;
    }
}
