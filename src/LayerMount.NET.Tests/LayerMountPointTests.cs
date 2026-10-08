// Round-trip tests for the LayerMount.MountPoint.* managed wrappers.
//
// None of these tests require Administrator -- mount-point preparation
// only manipulates a temp directory under %TEMP%.

using System;
using System.IO;
using Xunit;

namespace LayerMount.Tests;

public sealed class LayerMountPointTests
{
    [Theory]
    [InlineData("C:",   true)]
    [InlineData("C:\\", true)]
    [InlineData("z:",   true)]
    [InlineData("z:\\", true)]
    [InlineData("C:\\foo",          false)]
    [InlineData("\\\\?\\C:",        false)]
    [InlineData("foo",              false)]
    [InlineData("",                 false)]
    [InlineData("1:",               false)]
    [InlineData(":",                false)]
    public void IsDriveLetter_RecognizesDriveLetterForms(string mountPoint, bool expected)
    {
        Assert.Equal(expected, LayerMount.MountPoint.IsDriveLetter(mountPoint));
    }

    [Fact]
    public void IsDriveLetter_NullPath_Throws()
    {
        // Native accepts NULL and returns FALSE; the managed wrapper enforces
        // the .NET convention of ArgumentNullException at the boundary.
        Assert.Throws<ArgumentNullException>(
            () => LayerMount.MountPoint.IsDriveLetter(null!));
    }

    [Fact]
    public void PrepareDirectory_FreshPath_ValidatesWithoutClaimingOwnership()
    {
        using var scratch = new TempScratchDir("MP");
        string path = scratch.Sub("mnt");

        var prep = LayerMount.MountPoint.PrepareDirectory(path);

        Assert.False(prep.DirectoryCreatedByUs,
            "PrepareDirectory must not claim ownership before the host adapter's mount call");
        Assert.False(Directory.Exists(path),
            "PrepareDirectory does not create the directory; the host adapter's mount call does");
    }

    [Fact]
    public void PrepareDirectory_PathExists_ThrowsCollision()
    {
        using var scratch = new TempScratchDir("MP");
        string path = scratch.Sub("already-here");
        Directory.CreateDirectory(path);

        // Native returns HRESULT_FROM_NT(STATUS_OBJECT_NAME_COLLISION); we only
        // assert that a non-success HRESULT comes back as an LayerMountException.
        var ex = Assert.Throws<LayerMountException>(
            () => LayerMount.MountPoint.PrepareDirectory(path));
        Assert.True(ex.HResult < 0, $"Expected failure HRESULT, got 0x{ex.HResult:X8}");
    }

    [Fact]
    public void PrepareDirectory_EmptyPath_Throws()
    {
        // Native returns HRESULT_FROM_NT(STATUS_INVALID_PARAMETER) for empty
        // path; managed wrapper surfaces that as LayerMountException.
        var ex = Assert.Throws<LayerMountException>(
            () => LayerMount.MountPoint.PrepareDirectory(""));
        Assert.True(ex.HResult < 0);
    }

    [Fact]
    public void RoundTrip_CaptureIdentityWithoutOwnership_LeavesDirectory()
    {
        using var scratch = new TempScratchDir("MP");
        string path = scratch.Sub("mnt");

        var prep = LayerMount.MountPoint.PrepareDirectory(path);
        Assert.False(prep.DirectoryCreatedByUs,
            "PrepareDirectory does not claim ownership");

        SimulateMountCreatingDirectory(path);
        prep = LayerMount.MountPoint.CaptureIdentity(path);
        Assert.NotEqual(0UL, prep.VolumeSerial);
        Assert.False(prep.DirectoryCreatedByUs,
            "CaptureIdentity is side-effect-free on ownership by design");

        // Without an explicit ownership claim from the host adapter,
        // ReleaseIfSafe must refuse to remove the directory.
        LayerMount.MountPoint.ReleaseIfSafe(path, prep);
        Assert.True(Directory.Exists(path),
            "ReleaseIfSafe without createdByUs must leave the directory");
    }

    [Fact]
    public void RoundTrip_NotOwnedByUs_LeavesDirectory()
    {
        using var scratch = new TempScratchDir("MP");
        string path = scratch.Sub("foreign");
        Directory.CreateDirectory(path);

        // Default prep: directoryCreatedByUs=FALSE. Release must no-op.
        var prep = LayerMount.MountPoint.CaptureIdentity(path);
        Assert.False(prep.DirectoryCreatedByUs);

        LayerMount.MountPoint.ReleaseIfSafe(path, prep);

        Assert.True(Directory.Exists(path),
            "ReleaseIfSafe must refuse to remove a directory it does not own");
    }

    [Fact]
    public void RoundTrip_ClaimAfterMount_ReleaseRemovesEmptyDirectory()
    {
        using var scratch = new TempScratchDir("MP");
        string path = scratch.Sub("mnt");

        LayerMount.MountPoint.PrepareDirectory(path);

        SimulateMountCreatingDirectory(path);
        var prep = LayerMount.MountPoint.ClaimAfterMount(path);
        Assert.NotEqual(0UL, prep.VolumeSerial);
        Assert.True(prep.DirectoryCreatedByUs,
            "ClaimAfterMount claims a directory whose identity it captured");

        LayerMount.MountPoint.ReleaseIfSafe(path, prep);
        Assert.False(Directory.Exists(path),
            "ReleaseIfSafe removes the empty directory the host adapter claimed");
    }

    [Fact]
    public void ClaimAfterMount_ZeroIdentity_LeavesDirectory()
    {
        using var scratch = new TempScratchDir("MP");
        string path = scratch.Sub("mnt");

        LayerMount.MountPoint.PrepareDirectory(path);

        var prep = LayerMount.MountPoint.ClaimAfterMount(path);
        Assert.Equal(0UL, prep.VolumeSerial);
        Assert.False(prep.DirectoryCreatedByUs,
            "ClaimAfterMount does not claim a directory it could not identify");

        Directory.CreateDirectory(path);
        LayerMount.MountPoint.ReleaseIfSafe(path, prep);
        Assert.True(Directory.Exists(path),
            "ReleaseIfSafe leaves a directory that was never claimed");
    }

    [Theory]
    [InlineData("C:")]
    [InlineData("C:\\")]
    public void ClaimAfterMount_DriveLetter_DoesNotClaim(string mountPoint)
    {
        var prep = LayerMount.MountPoint.ClaimAfterMount(mountPoint);
        Assert.Equal(0UL, prep.VolumeSerial);
        Assert.False(prep.DirectoryCreatedByUs,
            "ClaimAfterMount does not claim a drive-letter mount point");
    }

    [Fact]
    public void CaptureIdentity_MissingDirectory_LeavesIdentityZero()
    {
        using var scratch = new TempScratchDir("MP");
        string path = scratch.Sub("never-created");

        // Native: no-op, identity stays zero, returns S_OK.
        var prep = LayerMount.MountPoint.CaptureIdentity(path);
        Assert.False(prep.DirectoryCreatedByUs);
        Assert.Equal(0UL, prep.VolumeSerial);
    }

    private static void SimulateMountCreatingDirectory(string path) =>
        Directory.CreateDirectory(path);
}
