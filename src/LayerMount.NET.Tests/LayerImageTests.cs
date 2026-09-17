using System;
using System.Buffers.Binary;
using System.IO;
using System.Security.Cryptography;
using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

public sealed class LayerImageTests
{
    [Fact]
    public void Pack_WritesHeaderFieldsAtDocumentedOffsets()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        string src = Path.Combine(env.Root, "src");
        Directory.CreateDirectory(src);
        File.WriteAllText(Path.Combine(src, "a.txt"), "alpha");

        string imagePath = Path.Combine(env.Root, "header.lmnt");
        using (mount.Images.Pack(src, imagePath, new ImageStampOptions(CompressionLevel: 3))) { }

        byte[] bytes = File.ReadAllBytes(imagePath);

        byte[] expectedMagic = { (byte)'O', (byte)'V', (byte)'L', (byte)'Y', (byte)'I', (byte)'M', (byte)'G', 0 };
        Assert.Equal(expectedMagic, bytes.AsSpan(0, 8).ToArray());

        Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(8, 4)));
        Assert.Equal(1u, BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(12, 4)));
        Assert.Equal(128ul, BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(16, 8)));

        ulong metadataSize = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(24, 8));
        ulong dataOffset = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(32, 8));
        ulong dataSize = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(40, 8));
        byte[] storedChecksum = bytes.AsSpan(48, 32).ToArray();

        Assert.Equal(128ul + metadataSize, dataOffset);

        byte[] dataSection = bytes.AsSpan((int)dataOffset, (int)dataSize).ToArray();
        byte[] computedChecksum = SHA256.HashData(dataSection);

        Assert.Equal(computedChecksum, storedChecksum);
    }

    [Fact]
    public void Pack_Validate_Unpack_RoundTrip()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        string src = Path.Combine(env.Root, "src");
        Directory.CreateDirectory(src);
        File.WriteAllText(Path.Combine(src, "a.txt"),   "alpha");
        File.WriteAllBytes(Path.Combine(src, "b.bin"), new byte[] { 1, 2, 3, 4 });

        string imagePath = Path.Combine(env.Root, "out.lmnt");
        using (var img = mount.Images.Pack(src, imagePath, new ImageStampOptions(CompressionLevel: 3)))
        {
            Assert.Equal(imagePath, img.Path);
        }

        mount.Images.Validate(imagePath);

        string dst = Path.Combine(env.Root, "dst");
        Directory.CreateDirectory(dst);
        mount.Images.Unpack(imagePath, dst);

        Assert.Equal(
            File.ReadAllBytes(Path.Combine(src, "a.txt")),
            File.ReadAllBytes(Path.Combine(dst, "a.txt")));
        Assert.Equal(
            File.ReadAllBytes(Path.Combine(src, "b.bin")),
            File.ReadAllBytes(Path.Combine(dst, "b.bin")));
    }

    [Fact]
    public void Unpack_MismatchedChecksum_ThrowsButUnpackUncheckedExtractsAnyway()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        string src = Path.Combine(env.Root, "src");
        Directory.CreateDirectory(src);
        File.WriteAllText(Path.Combine(src, "a.txt"), "alpha");

        string imagePath = Path.Combine(env.Root, "mismatched.lmnt");
        using (mount.Images.Pack(src, imagePath, new ImageStampOptions(CompressionLevel: 1))) { }

        const int ChecksumOffset = 48;
        byte[] bytes = File.ReadAllBytes(imagePath);
        bytes[ChecksumOffset] ^= 0xFF;
        File.WriteAllBytes(imagePath, bytes);

        string dst = Path.Combine(env.Root, "dst");
        Directory.CreateDirectory(dst);
        Assert.Throws<LayerMountException>(() => mount.Images.Unpack(imagePath, dst));

        string dstUnchecked = Path.Combine(env.Root, "dst-unchecked");
        Directory.CreateDirectory(dstUnchecked);
        mount.Images.UnpackUnchecked(imagePath, dstUnchecked);

        Assert.Equal(
            File.ReadAllBytes(Path.Combine(src, "a.txt")),
            File.ReadAllBytes(Path.Combine(dstUnchecked, "a.txt")));
    }

    [Fact]
    public void Validate_TruncatedImage_Throws()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        string src = Path.Combine(env.Root, "src");
        Directory.CreateDirectory(src);
        File.WriteAllText(Path.Combine(src, "only.txt"), "contents");

        string imagePath = Path.Combine(env.Root, "broken.lmnt");
        using (var img = mount.Images.Pack(src, imagePath, new ImageStampOptions(CompressionLevel: 1))) { }

        // Truncate the image so the checksum fails.
        using (var fs = new FileStream(imagePath, FileMode.Open, FileAccess.Write))
        {
            fs.SetLength(16);
        }

        Assert.Throws<LayerMountException>(() =>
            mount.Images.Validate(imagePath));
    }

    [Fact]
    public void Pack_StampOptions_AuthorAndDescriptionAreStamped()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        string src = Path.Combine(env.Root, "src");
        Directory.CreateDirectory(src);
        File.WriteAllText(Path.Combine(src, "a.txt"), "alpha");

        string imagePath = Path.Combine(env.Root, "stamped.lmnt");
        using (mount.Images.Pack(src, imagePath,
            new ImageStampOptions(Author: "a", Description: "d"))) { }

        ImageMetadata meta = mount.Images.GetMetadata(imagePath);
        Assert.Equal("a", meta.Author);
        Assert.Equal("d", meta.Description);
    }

    [Fact]
    public void PackDifferential_WithStampOptions_ProducesLoadableImage()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        string baseDir = Path.Combine(env.Root, "base");
        Directory.CreateDirectory(baseDir);
        File.WriteAllText(Path.Combine(baseDir, "a.txt"), "alpha");

        string src = Path.Combine(env.Root, "src");
        Directory.CreateDirectory(src);
        File.WriteAllText(Path.Combine(src, "a.txt"), "alpha");
        File.WriteAllText(Path.Combine(src, "b.txt"), "beta");

        string imagePath = Path.Combine(env.Root, "diff.lmnt");
        using (mount.Images.PackDifferential(src, baseDir, imagePath,
            new ImageStampOptions(Author: "a", Description: "d"))) { }

        mount.Images.Validate(imagePath);
        ImageMetadata meta = mount.Images.GetMetadata(imagePath);
        Assert.Equal("a", meta.Author);
        Assert.Equal("d", meta.Description);
    }
}
