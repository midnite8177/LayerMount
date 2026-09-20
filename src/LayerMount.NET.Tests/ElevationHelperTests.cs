using System;
using Xunit;

namespace LayerMount.Tests;

public sealed class ElevationHelperTests
{
    [SkippableFact]
    public void SkipIfNotElevated_WhenNotElevated_SkipsAndNamesTheReason()
    {
        Skip.If(ElevationHelper.IsElevated(),
            "This test observes the skip in a process that is not elevated.");

        var skip = Assert.Throws<SkipException>(
            () => ElevationHelper.SkipIfNotElevated("the test needs admin"));

        Assert.Contains("the test needs admin", skip.Message);
    }

    [SkippableFact]
    public void SkipIfNotElevated_WhenElevated_Returns()
    {
        Skip.IfNot(ElevationHelper.IsElevated(),
            "This test observes the pass-through in an elevated process.");

        ElevationHelper.SkipIfNotElevated("the test needs admin");
    }

    [SkippableFact]
    public void IsElevated_OnAnotherOs_IsFalse()
    {
        Skip.If(OperatingSystem.IsWindows(),
            "This test observes the answer on a non-Windows OS.");

        Assert.False(ElevationHelper.IsElevated());
    }
}
