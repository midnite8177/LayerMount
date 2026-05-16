// Round-trip tests for the LayerMount.MountPoint.* managed wrappers.
// Mirrors the assertions in the native AbiTests suite
// (src/LayerMount.AbiTests/AbiHostMountPointTests.cpp), proving the
// .NET wrappers reach LayerMount.dll without losing layout or semantics.
//
// Lifecycle the native helpers model:
//   1. PrepareDirectory  -- validate path and create parent directories.
//                            Does NOT create the leaf and does NOT claim
//                            ownership; host adapters that fail on a
//                            pre-existing leaf would be pre-empted, and
//                            claiming ownership before the mount commits
//                            would open a TOCTOU.
//   2. (host materialises the directory by mounting onto it.)
//   3. CaptureIdentity   -- snap volumeSerial + fileId now that the
//                            directory exists. Side-effect-free on the
//                            ownership flag by design.
//   4. Host adapter sets prep.DirectoryCreatedByUs = true if the mount
//      call succeeded (which implies the leaf is newly owned).
//   5. (host runs.)
//   6. ReleaseIfSafe     -- remove the directory iff createdByUs AND
//                            identity still matches AND empty; otherwise
//                            leave it in place.
//
// None of these tests require Administrator -- mount-point preparation
// only manipulates a temp directory under %TEMP%.

using System;
using System.IO;
using Xunit;

namespace LayerMount.Tests;

public sealed class LayerMountPointTests
{
    // ------------------------------------------------------------------
    // IsDriveLetter
    // ------------------------------------------------------------------

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

    // ------------------------------------------------------------------
    // PrepareDirectory
    // ------------------------------------------------------------------

    [Fact]
    public void PrepareDirectory_FreshPath_ValidatesWithoutClaimingOwnership()
    {
        using var scratch = new TempScratchDir();
        string path = scratch.Sub("mnt");

        var prep = LayerMount.MountPoint.PrepareDirectory(path);

        // PrepareDirectory is validation-only under the revised contract.
        // It must NOT create the leaf (the host adapter's mount call
        // materializes it) and must NOT claim ownership until the host's
        // mount call actually completes.
        Assert.False(prep.DirectoryCreatedByUs,
            "Prepare must not claim ownership before the host's mount call");
        Assert.False(Directory.Exists(path),
            "Prepare does not materialize the directory; the host does");
    }

    [Fact]
    public void PrepareDirectory_PathExists_ThrowsCollision()
    {
        using var scratch = new TempScratchDir();
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

    // ------------------------------------------------------------------
    // ReleaseIfSafe round-trip
    // ------------------------------------------------------------------

    [Fact]
    public void RoundTrip_CaptureIdentityWithoutOwnership_LeavesDirectory()
    {
        using var scratch = new TempScratchDir();
        string path = scratch.Sub("mnt");

        var prep = LayerMount.MountPoint.PrepareDirectory(path);
        Assert.False(prep.DirectoryCreatedByUs,
            "Prepare does not claim ownership under the revised contract");

        // Simulate the host mount call materializing the directory.
        Directory.CreateDirectory(path);
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
        using var scratch = new TempScratchDir();
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
    public void CaptureIdentity_MissingDirectory_LeavesIdentityZero()
    {
        using var scratch = new TempScratchDir();
        string path = scratch.Sub("never-created");

        // Native: no-op, identity stays zero, returns S_OK.
        var prep = LayerMount.MountPoint.CaptureIdentity(path);
        Assert.False(prep.DirectoryCreatedByUs);
        Assert.Equal(0UL, prep.VolumeSerial);
    }

    // ------------------------------------------------------------------
    // Inline scratch helper -- no project-wide fixture needed for these
    // self-contained mount-point tests.
    // ------------------------------------------------------------------

    private sealed class TempScratchDir : IDisposable
    {
        public string Root { get; }

        public TempScratchDir()
        {
            Root = Path.Combine(Path.GetTempPath(),
                "LayerMountMP_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(Root);
        }

        public string Sub(string name) => Path.Combine(Root, name);

        public void Dispose()
        {
            try { Directory.Delete(Root, recursive: true); }
            catch { /* best-effort cleanup */ }
        }
    }
}
