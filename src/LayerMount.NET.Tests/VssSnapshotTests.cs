using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

public sealed class VssSnapshotTests
{
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
}
