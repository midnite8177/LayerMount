using System.IO;
using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

public sealed class LayerImageTests
{
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
        using (var img = mount.Images.Pack(src, imagePath, compressionLevel: 3))
        {
            Assert.Equal(imagePath, img.Path);
        }

        mount.Images.Validate(imagePath);

        string dst = Path.Combine(env.Root, "dst");
        Directory.CreateDirectory(dst);
        mount.Images.Unpack(imagePath, dst, verifyChecksum: true);

        Assert.Equal(
            File.ReadAllBytes(Path.Combine(src, "a.txt")),
            File.ReadAllBytes(Path.Combine(dst, "a.txt")));
        Assert.Equal(
            File.ReadAllBytes(Path.Combine(src, "b.bin")),
            File.ReadAllBytes(Path.Combine(dst, "b.bin")));
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
        using (var img = mount.Images.Pack(src, imagePath, compressionLevel: 1)) { }

        // Truncate the image so the checksum fails.
        using (var fs = new FileStream(imagePath, FileMode.Open, FileAccess.Write))
        {
            fs.SetLength(16);
        }

        Assert.Throws<LayerMountException>(() =>
            mount.Images.Validate(imagePath));
    }
}
