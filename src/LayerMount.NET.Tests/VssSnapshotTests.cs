using System;
using System.IO;
using System.Runtime.InteropServices;
using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

public sealed partial class VssSnapshotTests
{
    /// <summary>
    /// The same query <c>VSSManager::ValidateSnapshotPath</c> makes. Called
    /// directly rather than through <see cref="Directory.Exists"/>, which
    /// answers false for every spelling of a device-namespace path and so
    /// cannot tell the two forms apart.
    /// </summary>
    private static bool AttributesReadable(string path)
        => GetFileAttributesW(path) != InvalidFileAttributes;

    private const uint InvalidFileAttributes = 0xFFFFFFFFu;

    [LibraryImport("kernel32.dll", EntryPoint = "GetFileAttributesW",
        StringMarshalling = StringMarshalling.Utf16, SetLastError = true)]
    private static partial uint GetFileAttributesW(string path);

    /// <summary>
    /// Creates a non-persistent snapshot of the system volume, or skips the
    /// test when this host cannot make one. The caller deletes the snapshot.
    /// </summary>
    private static VssSnapshot CreateSystemRootSnapshotOrSkip(VssApi vss)
    {
        ElevationHelper.SkipIfNotElevated("VSS snapshot creation requires admin");

        string systemRoot = Path.GetPathRoot(Environment.SystemDirectory)!;
        try
        {
            return vss.CreateSnapshot(systemRoot, persistent: false);
        }
        catch (LayerMountException ex) when (VssEnvironment.IsEnvironmentalHResult(ex.HResult))
        {
            throw new SkipException(
                $"VSS could not snapshot {systemRoot} on this host: 0x{ex.HResult:X8}");
        }
    }

    [Fact]
    public void ListSnapshots_ReturnsDefinedList()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        // Narrow the tolerated exception set to HRESULTs that
        // legitimately stem from a non-operational VSS provider; a
        // blanket catch would have swallowed managed/native marshalling
        // regressions and ABI failures, masking real bugs in the
        // wrapper. Anything outside this allowlist fails the test.
        try
        {
            var list = mount.Vss.ListSnapshots();
            Assert.NotNull(list);
        }
        catch (LayerMountException ex) when (VssEnvironment.IsEnvironmentalHResult(ex.HResult))
        {
            // VSS service is absent / blocked / caller not elevated on
            // this host; the wrapper still round-tripped the failure
            // correctly.
        }
    }

    [SkippableFact]
    public void Cleanup_OnEmptyMachine_Succeeds()
    {
        ElevationHelper.SkipIfNotElevated("VSS cleanup requires admin");

        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        mount.Vss.Cleanup();
    }

    [Fact]
    public void CreateSnapshot_BogusVolume_ThrowsLayerMountException()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        Assert.Throws<LayerMountException>(() =>
            mount.Vss.CreateSnapshot(@"Z:\definitely-not-a-volume"));
    }

    /// <summary>
    /// HRESULT_FROM_WIN32(ERROR_NOT_FOUND), the code the ABI returns for an
    /// id this overlay never created.
    /// </summary>
    private const int ErrorNotFoundHResult = unchecked((int)0x80070490u);

    /// <summary>
    /// Reaches no VSS service, so it runs on every host: the id is rejected
    /// from the overlay's own snapshot table before any provider is asked.
    /// The HRESULT is asserted exactly rather than through
    /// <see cref="VssEnvironment.IsEnvironmentalHResult"/>, which also
    /// tolerates this code -- here it is the expected answer, and accepting
    /// the whole environmental set would pass on a service failure too.
    /// </summary>
    [Fact]
    public void ValidateSnapshotPath_IdFromAnotherOverlay_Throws()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());

        var ex = Assert.Throws<LayerMountException>(() =>
            mount.Vss.ValidateSnapshotPath(Guid.NewGuid().ToString()));
        Assert.Equal(ErrorNotFoundHResult, ex.HResult);
    }

    /// <summary>
    /// The live case. Separated from
    /// <see cref="ValidateSnapshotPath_IdFromAnotherOverlay_Throws"/> because
    /// only this one needs a working provider, and a host without one must
    /// skip rather than fail.
    /// </summary>
    [SkippableFact]
    public void ValidateSnapshotPath_LiveSnapshot_ReturnsTrue()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        using var snapshot = CreateSystemRootSnapshotOrSkip(mount.Vss);

        try
        {
            Assert.True(mount.Vss.ValidateSnapshotPath(snapshot.Id));
        }
        finally
        {
            mount.Vss.DeleteSnapshot(snapshot.Id);
        }

        // DeleteSnapshot erases the entry, so the id stops being one this
        // overlay owns and the call throws instead of reporting false. The
        // false result means "still owned here, path gone", which only an
        // external destruction produces and no test can stage reliably.
        var afterDelete = Assert.Throws<LayerMountException>(() =>
            mount.Vss.ValidateSnapshotPath(snapshot.Id));
        Assert.Equal(ErrorNotFoundHResult, afterDelete.HResult);
    }

    /// <summary>
    /// Pins the reason the native <c>VSSManager::ValidateSnapshotPath</c>
    /// restores the trailing separator before it queries the path.
    /// <see cref="VssSnapshot.DevicePath"/> is public and documented as
    /// stripped, so a consumer that probes it directly reads a live snapshot
    /// as unreachable. Without this fact the separator looks like an
    /// accident and the next edit removes it.
    /// </summary>
    [SkippableFact]
    public void DevicePathWithoutTrailingSeparatorDoesNotResolveAsADirectory()
    {
        using var env = new TempLayerEnvironment(0);
        using var mount = LayerMount.Create(env.BuildConfig());
        using var snapshot = CreateSystemRootSnapshotOrSkip(mount.Vss);

        try
        {
            Assert.False(
                snapshot.DevicePath.EndsWith('\\'),
                $"DevicePath is documented as stripped but was '{snapshot.DevicePath}'; "
                    + "the rest of this test measures the stripped form.");

            Assert.False(
                AttributesReadable(snapshot.DevicePath),
                $"the bare device object '{snapshot.DevicePath}' answered a "
                    + "GetFileAttributesW query, so ValidateSnapshotPath no "
                    + "longer needs to restore the separator.");

            Assert.True(
                AttributesReadable(snapshot.DevicePath + '\\'),
                $"the volume-root form '{snapshot.DevicePath}\\' did not answer "
                    + "a GetFileAttributesW query for a snapshot created moments "
                    + "ago, so the separator is not what makes the path "
                    + "reachable.");
        }
        finally
        {
            mount.Vss.DeleteSnapshot(snapshot.Id);
        }
    }
}
