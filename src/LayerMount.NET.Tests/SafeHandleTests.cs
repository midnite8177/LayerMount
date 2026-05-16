using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

public sealed class SafeHandleTests
{
    private const uint FILE_ATTRIBUTE_NORMAL = 0x00000080u;
    private const uint GENERIC_READ          = 0x80000000u;
    private const uint GENERIC_WRITE         = 0x40000000u;

    [Fact]
    public void LayerMount_Dispose_IsIdempotent()
    {
        using var env = new TempLayerEnvironment(0);
        var mount = LayerMount.Create(env.BuildConfig());
        mount.Dispose();
        mount.Dispose();
        mount.Dispose(); // third time still safe
    }

    [Fact]
    public void LayerMountFile_Dispose_IsIdempotent()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        var file = mount.CreateFile(@"\dispose.txt", 0u,
            GENERIC_READ | GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL);
        file.Dispose();
        file.Dispose();
    }

    [Fact]
    public void LayerImage_Dispose_IsIdempotent()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        string src = System.IO.Path.Combine(env.Root, "src");
        System.IO.Directory.CreateDirectory(src);
        System.IO.File.WriteAllText(System.IO.Path.Combine(src, "only.txt"), "x");

        string imagePath = System.IO.Path.Combine(env.Root, "out.lmnt");
        var img = mount.Images.Pack(src, imagePath, compressionLevel: 1);
        img.Dispose();
        img.Dispose();
    }

    // Sanity check for the diagnostic counter that the dangling-child
    // tests below rely on: opening bumps ActiveHandles and disposing
    // returns it to baseline.
    [Fact]
    public void ActiveHandles_TracksOpenAndClose()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        ulong baseline = mount.GetStats().ActiveHandles;

        var files = new System.Collections.Generic.List<LayerMountFile>();
        for (int i = 0; i < 4; i++)
        {
            files.Add(mount.CreateFile($@"\count_{i}.txt", 0u,
                GENERIC_READ | GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL));
        }
        Assert.Equal(baseline + 4, mount.GetStats().ActiveHandles);

        foreach (var f in files)
        {
            f.Dispose();
        }
        Assert.Equal(baseline, mount.GetStats().ActiveHandles);
    }

    // Primary positive signal for Finding 26 follow-up: an LayerMountFile the
    // caller held onto (without Dispose) is closed on mount.Dispose() --
    // proves the registry walk ran. Idempotency on the held wrapper after
    // the walk must also hold.
    [Fact]
    public void LayerMount_Dispose_ClosesHeldFileChildren()
    {
        using var env = new TempLayerEnvironment(0);
        var mount = LayerMount.Create(env.BuildConfig());

        var f1 = mount.CreateFile(@"\held_1.txt", 0u,
            GENERIC_READ | GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL);
        var f2 = mount.CreateFile(@"\held_2.txt", 0u,
            GENERIC_READ | GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL);

        Assert.False(f1.IsClosed);
        Assert.False(f2.IsClosed);

        mount.Dispose();

        Assert.True(f1.IsClosed);
        Assert.True(f2.IsClosed);

        // Double-dispose after the walk closed the child must remain safe.
        f1.Dispose();
        f2.Dispose();
    }

    // Parallel test covering the ImageHandle branch of the type-ordered
    // walk. Uses Images.Pack so the test does not need admin.
    [Fact]
    public void LayerMount_Dispose_ClosesHeldImageChildren()
    {
        using var env = new TempLayerEnvironment(0);
        var mount = LayerMount.Create(env.BuildConfig());

        string src = System.IO.Path.Combine(env.Root, "held_img_src");
        System.IO.Directory.CreateDirectory(src);
        System.IO.File.WriteAllText(
            System.IO.Path.Combine(src, "only.txt"), "x");

        string imagePath = System.IO.Path.Combine(env.Root, "held.lmnt");
        var img = mount.Images.Pack(src, imagePath, compressionLevel: 1);
        Assert.False(img.IsClosed);

        mount.Dispose();

        Assert.True(img.IsClosed);
        img.Dispose();
    }
}
